/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Raspberry Pi 4's Bluetooth: a Broadcom BCM4345C0 behind a UART,
	speaking HCI with H4 framing (a type byte in front of every packet).

	The controller sits on the BCM2711's mini UART (GPIO 30 to 33, with
	RTS/CTS); the PL011 stays the serial console. The driver powers the
	controller through the firmware's BT_ON line, loads its patch file
	(BCM4345C0.hcd, a list of HCI commands) when the device is opened, and
	then passes packets between the UART and the HCI layer.

	The Raspberry Pi 3 B+ (BCM2837) has it wired the same way: the mini UART
	on GPIO 30 to 33, BT_ON on the firmware's expander. Its peripherals are
	at 0x3f000000, the mini UART's interrupt is the VideoCore's 29, and its
	pull-ups take the BCM2835's GPPUD/GPPUDCLK sequence.

	The Raspberry Pi 5 has the same controller on the BCM2712's BCM7271
	UART ("brcm,bcm7271-uart": a 16550 with its registers 4 bytes apart,
	96 MHz clock, automatic RTS/CTS) on GPIO 24 to 27, and BT_ON on the
	"gio" GPIO 29. Its line status and interrupt enable bits are where the
	mini UART has them; only the registers are elsewhere. */


#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <Drivers.h>
#include <FindDirectory.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <net_buffer.h>

#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_transport.h>

#include <rpi_firmware.h>
#include <drivers/bcm2712_pins.h>


#define DRIVER_NAME		"h4bcm"
#define DEVICE_PATH		"bluetooth/h4/" DRIVER_NAME "/0"
#define FIRMWARE_FILE	"/firmware/" DRIVER_NAME "/BCM4345C0.hcd"

//#define TRACE_H4
#ifdef TRACE_H4
#	define TRACE(x...) dprintf(DRIVER_NAME ": " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...)	dprintf(DRIVER_NAME ": " x)
#define INFO(x...)	dprintf(DRIVER_NAME ": " x)

// where the BCM2711 has things (ARM physical addresses)
#define AUX_BASE			0xfe215000
#define AUX_ENABLES			0x04
#define  AUX_ENABLE_UART	(1 << 0)
#define MU_IO				0x40
#define MU_IER				0x44
#define  MU_IER_RX			(1 << 0)
#define MU_IIR				0x48
#define  MU_IIR_CLEAR_FIFOS	0x06
#define MU_LCR				0x4c
#define  MU_LCR_8BIT		0x03
#define MU_MCR				0x50
#define MU_LSR				0x54
#define  MU_LSR_DATA_READY	(1 << 0)
#define  MU_LSR_OVERRUN		(1 << 1)
#define  MU_LSR_TX_READY	(1 << 5)
#define MU_CNTL				0x60
#define  MU_CNTL_RX			(1 << 0)
#define  MU_CNTL_TX			(1 << 1)
#define  MU_CNTL_AUTO_RTS	(1 << 2)
#define  MU_CNTL_AUTO_CTS	(1 << 3)
#define  MU_CNTL_RTS_LOW	(1 << 6)	// automatic RTS asserts low
#define  MU_CNTL_CTS_LOW	(1 << 7)	// automatic CTS asserts low
#define MU_BAUD				0x68
#define MU_INTERRUPT		(93 + 32)
// the BCM2837's (Pi 3): the kernel numbers the VideoCore's interrupts from 32
#define BCM2837_AUX_BASE	0x3f215000
#define BCM2837_GPIO_BASE	0x3f200000
#define BCM2837_MU_INTERRUPT	(29 + 32)
#define GPIO_GPPUD			0x94
#define GPIO_GPPUDCLK0		0x98
#define GPIO_GPPUDCLK1		0x9c

// the BCM2712's (Pi 5) 16550 style UART, registers 4 bytes apart
#define U16550_DATA			0x00	// with LCR_DLAB: divisor low
#define U16550_IER			0x04	// with LCR_DLAB: divisor high
#define U16550_FCR			0x08
#define  U16550_FCR_ENABLE	0x01
#define  U16550_FCR_CLEAR	0x06
#define  U16550_FCR_RX_8	0x80	// interrupt at 8 bytes in the FIFO
#define U16550_LCR			0x0c
#define  U16550_LCR_8BIT	0x03
#define  U16550_LCR_DLAB	0x80
#define U16550_MCR			0x10
#define  U16550_MCR_DTR		0x01
#define  U16550_MCR_RTS		0x02
#define  U16550_MCR_AFE		0x20	// automatic RTS/CTS
#define U16550_LSR			0x14
#define BCM2712_UART_CLOCK	96000000

#define GPIO_BASE			0xfe200000
#define GPIO_FSEL3			0x0c	// pins 30 to 39, three bits each
#define GPIO_PULL1			0xe8	// pins 16 to 31, two bits each
#define GPIO_PULL2			0xec	// pins 32 to 47
#define GPIO_ALT5			2
#define GPIO_PULL_UP		1
#define EXPANDER_GPIO_BASE	128
#define EXPANDER_BT_ON		0

#define H4_COMMAND			0x01
#define H4_ACL				0x02
#define H4_SCO				0x03
#define H4_EVENT			0x04

#define HCI_RESET					0x0c03
#define HCI_BCM_WRITE_BD_ADDR		0xfc01
#define HCI_BCM_DOWNLOAD_MINIDRIVER	0xfc2e
#define HCI_BCM_LAUNCH_RAM			0xfc4e
#define HCI_EVENT_COMMAND_COMPLETE	0x0e

#define RING_SIZE			8192
#define BAUD_RATE			115200


int32 api_version = B_CUR_DRIVER_API_VERSION;

static bt_hci_module_info* sHci;
static net_buffer_module_info* sBuffers;
static rpi_firmware_module_info* sFirmware;

static const char* sDeviceNames[] = { DEVICE_PATH, NULL };

// the UART's registers (Pi 4: the AUX block), and the GPIOs
static volatile uint8* sAux;
static area_id sAuxArea = -1;
static volatile uint8* sGpio;
static area_id sGpioArea = -1;

// the board's UART: register offsets and interrupt
static bool sBCM2837;
static uint64 sGpioBase = GPIO_BASE;
static bool sBCM2712;
static bool sBCM2712D0;
static uint64 sUartBase = AUX_BASE;
static uint64 sPinctrlBase;
static volatile uint8* sPinctrl;
static area_id sPinctrlArea = -1;
static uint32 sInterrupt = MU_INTERRUPT;
static uint32 sRegData = MU_IO;
static uint32 sRegLineStatus = MU_LSR;
static uint32 sRegInterruptEnable = MU_IER;

static mutex sLock = MUTEX_INITIALIZER("h4bcm");
static mutex sSendLock = MUTEX_INITIALIZER("h4bcm send");
static int32 sOpenCount;
static bool sReady;				// patched; packets go up to the HCI layer
static hci_id sHciId = -1;
static bt_hci_statistics sStatistics;

// received bytes, from the interrupt handler to the reader thread
static uint8 sRing[RING_SIZE];
static uint32 sRingHead;		// written by the interrupt handler
static uint32 sRingTail;
static uint32 sOverruns;
static sem_id sRingSem = -1;
static thread_id sReader = -1;
static volatile bool sRunning;

// the event the patch loader waits for
static sem_id sSetupSem = -1;
static uint8 sSetupEvent[HCI_MAX_EVENT_SIZE];
static size_t sSetupEventSize;


static inline uint32
read_aux(uint32 reg)
{
	return *(volatile uint32*)(sAux + reg);
}


static inline void
write_aux(uint32 reg, uint32 value)
{
	*(volatile uint32*)(sAux + reg) = value;
}


//	#pragma mark - UART


static int32
uart_interrupt(void* data)
{
	bool received = false;
	while (true) {
		uint32 status = read_aux(sRegLineStatus);
		if ((status & MU_LSR_OVERRUN) != 0)
			sOverruns++;
		if ((status & MU_LSR_DATA_READY) == 0)
			break;

		uint8 byte = read_aux(sRegData);
		uint32 next = (sRingHead + 1) % RING_SIZE;
		if (next != sRingTail) {
			sRing[sRingHead] = byte;
			sRingHead = next;
		} else
			sOverruns++;
		received = true;
	}

	if (!received)
		return B_UNHANDLED_INTERRUPT;

	release_sem_etc(sRingSem, 1, B_DO_NOT_RESCHEDULE);
	return B_INVOKE_SCHEDULER;
}


static void
uart_send(const uint8* data, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		// the controller's CTS holds the transmitter back when it has to
		bigtime_t timeout = system_time() + 1000000;
		while ((read_aux(sRegLineStatus) & MU_LSR_TX_READY) == 0) {
			if (system_time() > timeout) {
				ERROR("the UART does not take data\n");
				return;
			}
			snooze(50);
		}
		write_aux(sRegData, data[i]);
	}
}


static status_t
uart_init_bcm2712()
{
	// GPIO 24 to 27: TXD, RXD, CTS, RTS; pull-ups on the inputs
	for (uint32 pin = 24; pin <= 27; pin++) {
		bcm2712_set_pin(sPinctrl, sBCM2712D0, pin, BCM2712_FUNCTION_UART0,
			pin == 25 || pin == 26 ? BCM2712_PULL_UP : BCM2712_PULL_NONE);
	}
	memory_full_barrier();

	uint32 divisor = (BCM2712_UART_CLOCK + 8 * BAUD_RATE) / (16 * BAUD_RATE);
	write_aux(U16550_IER, 0);
	write_aux(U16550_LCR, U16550_LCR_8BIT | U16550_LCR_DLAB);
	write_aux(U16550_DATA, divisor & 0xff);
	write_aux(U16550_IER, divisor >> 8);
	write_aux(U16550_LCR, U16550_LCR_8BIT);
	write_aux(U16550_FCR, U16550_FCR_ENABLE | U16550_FCR_CLEAR
		| U16550_FCR_RX_8);
	write_aux(U16550_MCR, U16550_MCR_DTR | U16550_MCR_RTS | U16550_MCR_AFE);
	// whatever came before
	while ((read_aux(U16550_LSR) & MU_LSR_DATA_READY) != 0)
		read_aux(U16550_DATA);

	INFO("BCM7271 UART at %d baud (divisor %" B_PRIu32 ")\n", BAUD_RATE,
		divisor);
	return B_OK;
}


static status_t
uart_init()
{
	if (sBCM2712)
		return uart_init_bcm2712();

	// GPIO 30 to 33 to the mini UART (CTS, RTS, TXD, RXD)
	uint32 select = *(volatile uint32*)(sGpio + GPIO_FSEL3);
	for (uint32 pin = 30; pin <= 33; pin++) {
		select = (select & ~(7u << ((pin - 30) * 3)))
			| (GPIO_ALT5 << ((pin - 30) * 3));
	}
	*(volatile uint32*)(sGpio + GPIO_FSEL3) = select;

	// pull-ups on the inputs
	if (sBCM2837) {
		// GPPUD says what, the clock register which pins, 150 cycles each
		volatile uint32* gpio = (volatile uint32*)sGpio;
		gpio[GPIO_GPPUD / 4] = GPIO_PULL_UP + 1;
			// the BCM2835's codes: 0 off, 1 down, 2 up
		spin(5);
		gpio[GPIO_GPPUDCLK0 / 4] = (1u << 30) | (1u << 31);
		gpio[GPIO_GPPUDCLK1 / 4] = 1u << (33 - 32);
		spin(5);
		gpio[GPIO_GPPUD / 4] = 0;
		gpio[GPIO_GPPUDCLK0 / 4] = 0;
		gpio[GPIO_GPPUDCLK1 / 4] = 0;
	} else {
	uint32 pull = *(volatile uint32*)(sGpio + GPIO_PULL1);
	pull = (pull & ~(0xfu << ((30 - 16) * 2)))
		| (GPIO_PULL_UP << ((30 - 16) * 2));
	*(volatile uint32*)(sGpio + GPIO_PULL1) = pull;
	pull = *(volatile uint32*)(sGpio + GPIO_PULL2);
	pull = (pull & ~0xfu) | (GPIO_PULL_UP << ((33 - 32) * 2));
	*(volatile uint32*)(sGpio + GPIO_PULL2) = pull;
	}
	memory_full_barrier();

	uint32 clock = 0;
	if (sFirmware->get_clock_rate(RPI_FIRMWARE_CLOCK_CORE, false, &clock)
			!= B_OK || clock == 0) {
		return B_ERROR;
	}

	write_aux(AUX_ENABLES, read_aux(AUX_ENABLES) | AUX_ENABLE_UART);
	write_aux(MU_CNTL, 0);
	write_aux(MU_IER, 0);
	write_aux(MU_LCR, MU_LCR_8BIT);
	write_aux(MU_BAUD, (clock + 4 * BAUD_RATE) / (8 * BAUD_RATE) - 1);
	write_aux(MU_IIR, MU_IIR_CLEAR_FIFOS);
	// Hardware flow control, with both lines active low as on any UART:
	// this one's automatic RTS and CTS default to active high, which tells
	// the controller "do not send" for good.
	write_aux(MU_MCR, 0);
	write_aux(MU_CNTL, MU_CNTL_RX | MU_CNTL_TX | MU_CNTL_AUTO_RTS
		| MU_CNTL_AUTO_CTS | MU_CNTL_RTS_LOW | MU_CNTL_CTS_LOW);

	INFO("mini UART at %d baud (core clock %" B_PRIu32 " Hz)\n", BAUD_RATE,
		clock);
	return B_OK;
}


static void
set_power(bool on)
{
	if (sBCM2712) {
		bcm2712_gpio_output(sGpio, BCM2712_GPIO_BT_ON, on);
		return;
	}
	uint32 request[2] = { EXPANDER_GPIO_BASE + EXPANDER_BT_ON, on ? 1u : 0u };
	sFirmware->property(RPI_FIRMWARE_SET_GPIO_STATE, request, sizeof(request));
}


//	#pragma mark - receiving


/*!	One complete packet without its type byte. */
static void
deliver(uint8 type, uint8* data, size_t length)
{
	if (type == H4_EVENT) {
		sStatistics.successfulRX++;
		if (!sReady) {
			// the patch loader's
			if (length <= sizeof(sSetupEvent)) {
				memcpy(sSetupEvent, data, length);
				sSetupEventSize = length;
				release_sem(sSetupSem);
			}
			return;
		}
		sHci->PostTransportPacket(sHciId, BT_EVENT, data, length);
	} else if (type == H4_ACL) {
		sStatistics.successfulRX++;
		if (sReady)
			sHci->PostTransportPacket(sHciId, BT_ACL, data, length);
	}
		// voice (SCO) has nowhere to go in this stack
}


static status_t
reader_thread(void* data)
{
	static uint8 packet[HCI_MAX_FRAME_SIZE];
	uint8 type = 0;
	size_t have = 0;
	size_t headerSize = 0;
	size_t wanted = 0;

	while (sRunning) {
		acquire_sem_etc(sRingSem, 1, B_RELATIVE_TIMEOUT, 100000);

		while (sRingTail != sRingHead) {
			uint8 byte = sRing[sRingTail];
			sRingTail = (sRingTail + 1) % RING_SIZE;
			sStatistics.bytesRX++;

			if (type == 0) {
				// between packets: a type byte
				if (byte == H4_EVENT)
					headerSize = 2;
				else if (byte == H4_ACL)
					headerSize = 4;
				else if (byte == H4_SCO)
					headerSize = 3;
				else {
					// out of step; the next known type byte ends it
					sStatistics.errorRX++;
					continue;
				}
				type = byte;
				have = 0;
				wanted = headerSize;
				continue;
			}

			if (have < sizeof(packet))
				packet[have] = byte;
			have++;

			if (have == headerSize) {
				size_t payload;
				if (type == H4_EVENT)
					payload = packet[1];
				else if (type == H4_ACL)
					payload = packet[2] | (packet[3] << 8);
				else
					payload = packet[2];
				wanted = headerSize + payload;
			}

			if (have == wanted) {
				if (wanted <= sizeof(packet))
					deliver(type, packet, wanted);
				else
					sStatistics.errorRX++;
				type = 0;
			}
		}
	}
	return B_OK;
}


//	#pragma mark - sending


static status_t
send_packet(uint8 type, const uint8* data, size_t length)
{
	MutexLocker locker(sSendLock);
	uart_send(&type, 1);
	uart_send(data, length);
	sStatistics.bytesTX += length + 1;
	sStatistics.successfulTX++;
	return B_OK;
}


static status_t
send_buffer(hci_id id, net_buffer* buffer)
{
	if (id != sHciId || !sReady) {
		if (buffer->protocol == BT_ACL)
			sBuffers->free(buffer);
		return B_DEV_NOT_READY;
	}

	uint8 data[HCI_MAX_FRAME_SIZE];
	size_t size = buffer->size;
	status_t status = B_BAD_VALUE;
	if (size <= sizeof(data)
		&& sBuffers->read(buffer, 0, data, size) == B_OK) {
		if (buffer->protocol == BT_COMMAND)
			status = send_packet(H4_COMMAND, data, size);
		else if (buffer->protocol == BT_ACL)
			status = send_packet(H4_ACL, data, size);
	}

	// Commands stay the caller's; data is the transport's to free, sent or
	// not.
	if (buffer->protocol == BT_ACL)
		sBuffers->free(buffer);
	return status;
}


static bt_hci_transport_hooks sHooks = {
	&send_buffer,
	&send_buffer,
	&send_buffer,
	NULL,
	NULL,
	H4
};


//	#pragma mark - the controller's patch


/*!	Sends a command and waits for its Command Complete event. */
static status_t
setup_command(uint16 opcode, const uint8* parameters, uint8 length,
	bigtime_t timeout)
{
	uint8 command[3 + 255];
	command[0] = opcode & 0xff;
	command[1] = opcode >> 8;
	command[2] = length;
	if (length > 0)
		memcpy(command + 3, parameters, length);

	// nothing left over from before
	while (acquire_sem_etc(sSetupSem, 1, B_RELATIVE_TIMEOUT, 0) == B_OK)
		;

	send_packet(H4_COMMAND, command, 3 + length);

	bigtime_t end = system_time() + timeout;
	while (true) {
		bigtime_t left = end - system_time();
		if (left <= 0 || acquire_sem_etc(sSetupSem, 1, B_RELATIVE_TIMEOUT,
				left) != B_OK) {
			return B_TIMED_OUT;
		}
		// event code, length, packets allowed, opcode, status
		if (sSetupEventSize >= 6 && sSetupEvent[0] == HCI_EVENT_COMMAND_COMPLETE
			&& (sSetupEvent[3] | (sSetupEvent[4] << 8)) == opcode) {
			return sSetupEvent[5] == 0 ? B_OK : B_IO_ERROR;
		}
	}
}


static status_t
read_firmware(uint8*& _image, size_t& _size)
{
	static const directory_which kPlaces[] = {
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY
	};

	int fd = -1;
	for (size_t i = 0; i < B_COUNT_OF(kPlaces) && fd < 0; i++) {
		char path[B_PATH_NAME_LENGTH];
		if (find_directory(kPlaces[i], -1, false, path, sizeof(path)) != B_OK)
			continue;
		strlcat(path, FIRMWARE_FILE, sizeof(path));
		fd = open(path, B_READ_ONLY);
	}
	if (fd < 0) {
		ERROR("no patch file (data" FIRMWARE_FILE ")\n");
		return B_ENTRY_NOT_FOUND;
	}

	struct stat stat;
	if (fstat(fd, &stat) != 0 || stat.st_size < 4 || stat.st_size > 1024 * 1024) {
		close(fd);
		return B_BAD_DATA;
	}

	uint8* image = (uint8*)malloc(stat.st_size);
	if (image == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}
	ssize_t bytesRead = read(fd, image, stat.st_size);
	close(fd);
	if (bytesRead != stat.st_size) {
		free(image);
		return B_IO_ERROR;
	}

	_image = image;
	_size = stat.st_size;
	return B_OK;
}


static inline uint32
be32(const uint8* bytes)
{
	return ((uint32)bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8)
		| bytes[3];
}


/*!	The controller's address as the Raspberry Pi firmware put it into the
	device tree: the local-bd-address property of the Bluetooth node that is
	in use (the other one's is zero), low byte first. The device manager
	has no node for it, so the flattened tree itself is searched.
*/
static bool
find_address(uint8* address)
{
	int fd = open("/dev/bus/fdt/blob", O_RDONLY);
	if (fd < 0)
		return false;

	bool found = false;
	uint8 header[40];
	uint8* blob = NULL;
	if (read(fd, header, sizeof(header)) == (ssize_t)sizeof(header)
		&& be32(header) == 0xd00dfeed) {
		uint32 size = be32(header + 4);
		if (size >= sizeof(header) && size <= 1024 * 1024)
			blob = (uint8*)malloc(size);
		if (blob != NULL && pread(fd, blob, size, 0) == (ssize_t)size) {
			uint32 structOffset = be32(blob + 8);
			uint32 stringsOffset = be32(blob + 12);
			uint32 structSize = be32(blob + 36);
			uint32 offset = structOffset;
			uint32 end = structOffset + structSize;
			if (end > size)
				end = size;

			// tokens: 1 begin node (name), 2 end node, 3 property
			// (length, name offset, value), 4 nop, 9 end
			while (!found && offset + 4 <= end) {
				uint32 token = be32(blob + offset);
				offset += 4;
				if (token == 1) {
					while (offset < end && blob[offset] != 0)
						offset++;
					offset = (offset + 4) & ~3;
				} else if (token == 3) {
					if (offset + 8 > end)
						break;
					uint32 length = be32(blob + offset);
					uint32 name = stringsOffset + be32(blob + offset + 4);
					offset += 8;
					if (offset + length > end)
						break;
					static const uint8 kNone[6] = {};
					if (length == 6 && name + 17 <= size
						&& strcmp((const char*)blob + name,
							"local-bd-address") == 0
						&& memcmp(blob + offset, kNone, 6) != 0) {
						memcpy(address, blob + offset, 6);
						found = true;
					}
					offset = (offset + length + 3) & ~3;
				} else if (token == 9)
					break;
			}
		}
	}

	free(blob);
	close(fd);
	return found;
}


static status_t
setup_controller()
{
	uint8* image;
	size_t size;
	status_t status = read_firmware(image, size);
	if (status != B_OK)
		return status;

	// out of reset, at the UART speed it starts with
	set_power(false);
	snooze(50000);
	sRingTail = sRingHead;
	set_power(true);
	snooze(200000);

	status = setup_command(HCI_RESET, NULL, 0, 2000000);
	if (status != B_OK) {
		ERROR("the controller does not answer a reset: %s\n",
			strerror(status));
		if (sBCM2712) {
			ERROR("UART: line status %#" B_PRIx32 ", interrupt enable %#"
				B_PRIx32 ", modem control %#" B_PRIx32 "; ring head %" B_PRIu32
				" tail %" B_PRIu32 ", %" B_PRIu32 " overruns\n",
				read_aux(U16550_LSR), read_aux(U16550_IER),
				read_aux(U16550_MCR), sRingHead, sRingTail, sOverruns);
			free(image);
			return status;
		}
		ERROR("UART: line status %#" B_PRIx32 ", status %#" B_PRIx32
			", control %#" B_PRIx32 ", interrupt enable %#" B_PRIx32
			", identify %#" B_PRIx32 ", baud %#" B_PRIx32 "; ring head %"
			B_PRIu32 " tail %" B_PRIu32 ", %" B_PRIu32 " overruns, %"
			B_PRIu32 " bytes received\n", read_aux(MU_LSR), read_aux(0x64),
			read_aux(MU_CNTL), read_aux(MU_IER), read_aux(MU_IIR),
			read_aux(MU_BAUD), sRingHead, sRingTail, sOverruns,
			(uint32)sStatistics.bytesRX);
		ERROR("pins: function select %#" B_PRIx32 ", levels %#" B_PRIx32
			" %#" B_PRIx32 "\n", *(volatile uint32*)(sGpio + GPIO_FSEL3),
			*(volatile uint32*)(sGpio + 0x34),
			*(volatile uint32*)(sGpio + 0x38));
		free(image);
		return status;
	}

	status = setup_command(HCI_BCM_DOWNLOAD_MINIDRIVER, NULL, 0, 2000000);
	if (status != B_OK) {
		ERROR("the controller does not take a patch: %s\n", strerror(status));
		free(image);
		return status;
	}
	snooze(50000);

	// the file is the commands to send: opcode, length, parameters
	size_t offset = 0;
	uint32 records = 0;
	while (offset + 3 <= size) {
		uint16 opcode = image[offset] | (image[offset + 1] << 8);
		uint8 length = image[offset + 2];
		if (offset + 3 + length > size) {
			status = B_BAD_DATA;
			break;
		}

		status = setup_command(opcode, image + offset + 3, length, 2000000);
		if (status != B_OK && opcode == HCI_BCM_LAUNCH_RAM) {
			// it starts the patched code; an answer is a courtesy
			status = B_OK;
		}
		if (status != B_OK) {
			ERROR("patch record %" B_PRIu32 " (opcode %#x) failed: %s\n",
				records, opcode, strerror(status));
			break;
		}
		offset += 3 + length;
		records++;
	}
	free(image);
	if (status != B_OK)
		return status;

	// the patched controller starts over
	snooze(300000);
	sRingTail = sRingHead;
	status = setup_command(HCI_RESET, NULL, 0, 2000000);
	if (status != B_OK) {
		ERROR("the patched controller does not answer: %s\n",
			strerror(status));
		return status;
	}

	uint8 address[6];
	if (find_address(address)) {
		status = setup_command(HCI_BCM_WRITE_BD_ADDR, address, 6, 2000000);
		INFO("address %02x:%02x:%02x:%02x:%02x:%02x%s\n", address[5],
			address[4], address[3], address[2], address[1], address[0],
			status == B_OK ? "" : " (not taken)");
	} else
		INFO("no address in the device tree; the controller keeps its own\n");

	INFO("controller patched, %" B_PRIu32 " records\n", records);
	return B_OK;
}


//	#pragma mark - device


static status_t
bring_up()
{
	const phys_addr_t kPageMask = B_PAGE_SIZE - 1;
	uint64 gpioBase = sBCM2712 ? BCM2712_GIO_BASE : sGpioBase;
	sAuxArea = map_physical_memory("h4bcm uart", sUartBase & ~kPageMask,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&sAux);
	sGpioArea = map_physical_memory("h4bcm gpio", gpioBase & ~kPageMask,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&sGpio);
	if (sAuxArea < 0 || sGpioArea < 0)
		return B_NO_MEMORY;
	sAux += sUartBase & kPageMask;
	sGpio += gpioBase & kPageMask;
	if (sBCM2712) {
		sPinctrlArea = map_physical_memory("h4bcm pinctrl",
			sPinctrlBase & ~kPageMask, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&sPinctrl);
		if (sPinctrlArea < 0)
			return B_NO_MEMORY;
		sPinctrl += sPinctrlBase & kPageMask;
	}

	sRingSem = create_sem(0, "h4bcm ring");
	sSetupSem = create_sem(0, "h4bcm setup");
	if (sRingSem < 0 || sSetupSem < 0)
		return B_NO_MEMORY;

	sRingHead = sRingTail = 0;
	sReady = false;
	memset(&sStatistics, 0, sizeof(sStatistics));

	status_t status = uart_init();
	if (status != B_OK)
		return status;

	sRunning = true;
	sReader = spawn_kernel_thread(reader_thread, "h4bcm reader",
		B_URGENT_DISPLAY_PRIORITY, NULL);
	if (sReader < 0)
		return sReader;
	resume_thread(sReader);

	status = install_io_interrupt_handler(sInterrupt, uart_interrupt, NULL,
		0);
	if (status != B_OK)
		return status;
	write_aux(sRegInterruptEnable, MU_IER_RX);

	status = setup_controller();
	if (status != B_OK)
		return status;

	bluetooth_device* device;
	status = sHci->RegisterDriver(&sHooks, &device);
	if (status != B_OK)
		return status;
	sHciId = device->index;
	sReady = true;
	return B_OK;
}


static void
take_down()
{
	if (sHciId >= 0) {
		sReady = false;
		sHci->UnregisterDriver(sHciId);
		sHciId = -1;
	}

	if (sAux != NULL) {
		write_aux(sRegInterruptEnable, 0);
		remove_io_interrupt_handler(sInterrupt, uart_interrupt, NULL);
	}
	if (sReader >= 0) {
		sRunning = false;
		release_sem(sRingSem);
		status_t result;
		wait_for_thread(sReader, &result);
		sReader = -1;
	}
	if (sAux != NULL)
		set_power(false);

	if (sRingSem >= 0)
		delete_sem(sRingSem);
	if (sSetupSem >= 0)
		delete_sem(sSetupSem);
	sRingSem = sSetupSem = -1;
	if (sAuxArea >= 0)
		delete_area(sAuxArea);
	if (sGpioArea >= 0)
		delete_area(sGpioArea);
	if (sPinctrlArea >= 0)
		delete_area(sPinctrlArea);
	sAuxArea = sGpioArea = sPinctrlArea = -1;
	sAux = sGpio = sPinctrl = NULL;
}


static status_t
h4_open(const char* name, uint32 flags, void** _cookie)
{
	MutexLocker locker(sLock);
	if (sOpenCount > 0)
		return B_BUSY;

	status_t status = bring_up();
	if (status != B_OK) {
		// take_down() tolerates the interrupt handler not being there
		take_down();
		return status;
	}

	sOpenCount = 1;
	*_cookie = NULL;
	return B_OK;
}


static status_t
h4_close(void* cookie)
{
	return B_OK;
}


static status_t
h4_free(void* cookie)
{
	MutexLocker locker(sLock);
	take_down();
	sOpenCount = 0;
	return B_OK;
}


static status_t
h4_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	if (buffer == NULL || !IS_USER_ADDRESS(buffer))
		return B_BAD_VALUE;

	switch (op) {
		case ISSUE_BT_COMMAND:
		{
			// opcode, length, parameters; no type byte
			uint8 command[3 + 255];
			if (length < 3 || length > sizeof(command))
				return B_BAD_VALUE;
			if (user_memcpy(command, buffer, length) != B_OK)
				return B_BAD_ADDRESS;
			if (!sReady)
				return B_DEV_NOT_READY;
			return send_packet(H4_COMMAND, command, length);
		}

		case BT_UP:
			// the reader has been running since the patch went in
			return sReady ? B_OK : B_DEV_NOT_READY;

		case GET_STATS:
			return user_memcpy(buffer, &sStatistics, sizeof(sStatistics));

		case GET_HCI_ID:
			return user_memcpy(buffer, &sHciId, sizeof(hci_id));
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
h4_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_OK;
}


static status_t
h4_write(void* cookie, off_t position, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sDeviceHooks = {
	h4_open,
	h4_close,
	h4_free,
	h4_control,
	h4_read,
	h4_write
};


//	#pragma mark - driver


static bool has_node(const char* compatible);


/*!	Only on a board with the BCM2711: the addresses above are its. */
static bool
is_raspberry_pi_4()
{
	return has_node("brcm,bcm2711-emmc2");
}


/*!	The Pi 3 (BCM2837): its cores' local interrupt controller is unique to
	the BCM2836/BCM2837. */
static bool
find_bcm2837()
{
	if (!has_node("brcm,bcm2836-l1-intc"))
		return false;
	sBCM2837 = true;
	sUartBase = BCM2837_AUX_BASE;
	sGpioBase = BCM2837_GPIO_BASE;
	sInterrupt = BCM2837_MU_INTERRUPT;
	return true;
}


static bool
has_node(const char* compatible)
{
	device_manager_info* manager;
	if (get_module(B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&manager)
			!= B_OK) {
		return false;
	}

	device_attr attributes[] = {
		{ "fdt/compatible", B_STRING_TYPE, { .string = compatible } },
		{}
	};
	device_node* root = manager->get_root_node();
	device_node* node = NULL;
	bool found = manager->find_child_node(root, attributes, &node) == B_OK;
	if (found)
		manager->put_node(node);
	manager->put_node(root);
	put_module(B_DEVICE_MANAGER_MODULE_NAME);
	return found;
}


/*!	The Pi 5: the BCM7271 UART with the Bluetooth controller, and the pin
	multiplexer. */
static bool
find_bcm2712()
{
	uint64 base, size, interrupt, pinctrl;
	bool d0;
	if (!bcm2712_find_node("brcm,bcm7271-uart", 0, 0, &base, &size,
			&interrupt)
		|| interrupt == 0 || !bcm2712_find_pinctrl(&pinctrl, &d0)) {
		return false;
	}

	sBCM2712 = true;
	sBCM2712D0 = d0;
	sUartBase = base;
	sInterrupt = interrupt;
	sPinctrlBase = pinctrl;
	sRegData = U16550_DATA;
	sRegLineStatus = U16550_LSR;
	sRegInterruptEnable = U16550_IER;
	return true;
}


status_t
init_hardware()
{
	return find_bcm2712() || find_bcm2837() || is_raspberry_pi_4()
		? B_OK : B_ERROR;
}


status_t
init_driver()
{
	status_t status = get_module(BT_HCI_MODULE_NAME, (module_info**)&sHci);
	if (status != B_OK)
		return status;
	status = get_module(NET_BUFFER_MODULE_NAME, (module_info**)&sBuffers);
	if (status != B_OK) {
		put_module(BT_HCI_MODULE_NAME);
		return status;
	}
	if (find_bcm2712())
		return B_OK;
	find_bcm2837();
	status = get_module(RPI_FIRMWARE_MODULE_NAME, (module_info**)&sFirmware);
	if (status != B_OK) {
		put_module(NET_BUFFER_MODULE_NAME);
		put_module(BT_HCI_MODULE_NAME);
		return status;
	}
	return B_OK;
}


void
uninit_driver()
{
	if (sFirmware != NULL)
		put_module(RPI_FIRMWARE_MODULE_NAME);
	put_module(NET_BUFFER_MODULE_NAME);
	put_module(BT_HCI_MODULE_NAME);
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sDeviceHooks;
}
