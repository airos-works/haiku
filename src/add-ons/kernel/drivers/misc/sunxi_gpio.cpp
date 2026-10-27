/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	/dev/misc/sunxi_gpio: the pins of the Radxa Cubie A7S's 30-pin (J11) and
	15-pin (J6) headers for programs such as AirPins, with the interface of
	the Raspberry Pi's rpi_gpio (see <rpi_gpio.h>, RPI_GPIO_BOARD_CUBIE_A7S).
	The driver attaches to the A733's PIO node of the device tree when the
	system starts, on this board only; it finds the R_PIO (ports L and M)
	itself.

	Pin numbers are header positions: J11 pin n is pin n, J6 pin n is pin
	30 + n. The header map is the board's schematic (RS504 v1.10, sheet 14
	"40PIN Connector"), the register layout that of Allwinner's pinctrl
	driver for the A733 (pinctrl-sun60iw2.c and -r.c, hardware types 4 and
	1 of pinctrl-sunxi.c): a PIO bank n (A = 0) at 0x80 + 0x80 * n, with its
	interrupt registers 0x40 further on; R_PIO banks 0x30 apart from 0, their
	interrupt registers at 0x200 + 0x20 * n. The device tree lists one
	interrupt per bank, PIO from bank B and R_PIO from bank L.

	An input reports its edges from the bank interrupt: the pin is switched
	to its interrupt function (14), in which its data bit still reads the
	pad, and its programs see it as an input. Ports whose interrupt is in
	use by something else, or could not be installed, are sampled every
	millisecond instead.

	The data registers have no set and clear registers here that every
	A733 is known to have, so levels are written with read-modify-write
	accesses, as Linux does; the only other writers of these banks' data
	registers are sunxi_regulator (port M, once at boot) and sunxi_display
	(the USB-C VBUS switch on port L, when the port changes role). */


#include <Drivers.h>
#include <KernelExport.h>

#include <new>
#include <string.h>

#include <bus/FDT.h>
#include <condition_variable.h>
#include <device_manager.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <rpi_gpio.h>


//#define TRACE_GPIO
#ifdef TRACE_GPIO
#	define TRACE(x...)	dprintf("sunxi_gpio: " x)
#else
#	define TRACE(x...)	do {} while (false)
#endif
#define ERROR(x...)		dprintf("sunxi_gpio: " x)

#define DRIVER_MODULE_NAME	"drivers/misc/sunxi_gpio/driver_v1"
#define DEVICE_MODULE_NAME	"drivers/misc/sunxi_gpio/device_v1"

#define PIO_COMPATIBLE		"allwinner,sun60i-a733-pinctrl"
#define R_PIO_COMPATIBLE	"allwinner,sun60i-a733-r-pinctrl"
#define BOARD_COMPATIBLE	"radxa,cubie-a7s"

// pin multiplexer values
#define FUNCTION_INPUT		0
#define FUNCTION_OUTPUT		1
#define FUNCTION_INTERRUPT	14

// interrupt configuration: both edges
#define IRQ_BOTH_EDGES		4

#define PIN_COUNT			46		// J11 1..30, J6 31..45
#define EVENT_RING_SIZE		8192
#define WAIT_BATCH			64

// More edges than this within THROTTLE_WINDOW: the inputs are sampled for
// THROTTLE_PAUSE instead.
#define THROTTLE_WINDOW		10000
#define THROTTLE_EVENTS		400
#define THROTTLE_PAUSE		50000
// without an interrupt, inputs are sampled at this period
#define SAMPLE_PERIOD		1000


enum {
	PIO = 0,
	R_PIO = 1
};

struct header_pin {
	uint8			position;
	char			port;
	uint8			number;
	bool			claimable;
};

// The header's PIO pins; the other positions are power and ground.
static const header_pin kHeader[] = {
	{ 3, 'J', 23, true },		// TWI7-SDA
	{ 5, 'J', 22, true },		// TWI7-SCK
	{ 7, 'B', 0, true },
	{ 8, 'B', 9, false },		// UART0-TX: the system's serial console
	{ 10, 'B', 10, false },		// UART0-RX
	{ 11, 'B', 1, true },
	{ 12, 'B', 5, true },		// I2S0-BCLK
	{ 13, 'L', 6, true },
	{ 15, 'L', 7, true },
	{ 16, 'J', 24, true },
	{ 18, 'J', 25, true },
	{ 19, 'D', 12, true },		// SPI1-MOSI
	{ 21, 'D', 13, true },		// SPI1-MISO
	{ 22, 'L', 5, true },
	{ 23, 'D', 11, true },		// SPI1-CLK
	{ 24, 'D', 10, true },		// SPI1-CS0
	{ 26, 'D', 14, true },
	{ 27, 'D', 17, true },		// TWI2-SDA
	{ 28, 'D', 16, true },		// TWI2-SCK
	{ 29, 'B', 2, true },
	{ 30 + 1, 'B', 3, true },
	{ 30 + 2, 'M', 3, true },
	{ 30 + 3, 'M', 4, true },
	{ 30 + 5, 'B', 6, true },	// I2S0-LRCK
	{ 30 + 6, 'B', 4, true },	// I2S0-MCLK
	{ 30 + 7, 'B', 8, true },	// I2S0-DIN0
	{ 30 + 8, 'B', 7, true },	// I2S0-DOUT0
	{ 30 + 10, 'G', 0, true },
	{ 30 + 11, 'G', 1, true },
	{ 30 + 12, 'G', 2, true },
	{ 30 + 13, 'G', 3, true },
	{ 30 + 14, 'G', 4, true },
	{ 30 + 15, 'G', 5, true },
};

#define HEADER_GPIOS	(sizeof(kHeader) / sizeof(kHeader[0]))

struct pin_bank {
	char			port;
	uint8			controller;			// PIO or R_PIO
	uint32			registers;			// offset of the bank's registers
	uint32			interruptRegisters;
	uint32			interruptIndex;		// in the controller's node
	uint32			pull;				// offset of the pull registers
	uint32			headerBits;			// pins of the bank on the header
	uint64			positions;			// header pins of the bank
	uint32			vector;
	bool			interruptInstalled;
};

// every bank that has header pins
static pin_bank sBanks[] = {
	{ 'B', PIO }, { 'D', PIO }, { 'G', PIO }, { 'J', PIO },
	{ 'L', R_PIO }, { 'M', R_PIO }
};

#define BANK_COUNT		(sizeof(sBanks) / sizeof(sBanks[0]))

// position -> its pin's bank and bit
struct pin_location {
	pin_bank*		bank;
	uint8			number;
	bool			claimable;
};

struct gpio_client {
	mutex			waitLock;
	uint64			readPosition;	// in event numbers
	uint64			claimed;		// pins; changed under sLock + sEventLock
	uint32			lost;
	bool			closed;
	rpi_gpio_event	batch[WAIT_BATCH];
};

struct saved_pin {
	uint8			function;
	uint8			pull;
	uint8			level;
};


static device_manager_info* sDeviceManager;

static area_id sRegistersArea[2] = { -1, -1 };
static volatile uint8* sRegisters[2];
static pin_location sPins[PIN_COUNT];
static uint64 sClaimable;
static uint64 sNoInterrupt;			// header pins of banks that are sampled

// pin ownership and configuration
static mutex sLock = MUTEX_INITIALIZER("sunxi_gpio");
static gpio_client* sOwner[PIN_COUNT];
static saved_pin sSaved[PIN_COUNT];
static int32 sOpenCount;

// events; everything below is protected by sEventLock (interrupts off)
static spinlock sEventLock = B_SPINLOCK_INITIALIZER;
static ConditionVariable sEventCondition;
static rpi_gpio_event sEvents[EVENT_RING_SIZE];
static uint64 sEventCount;			// events ever recorded
static uint64 sEdgeMask;			// claimed inputs
static uint64 sInterruptMask;		// claimed inputs in their interrupt function
static uint64 sLastLevels;			// as last recorded
static bool sThrottled;
static bigtime_t sWindowStart;
static uint32 sWindowCount;
static timer sThrottleTimer;
static timer sSampleTimer;
static bool sSampling;


static inline uint32
read_reg(const pin_bank& bank, uint32 offset)
{
	return *(volatile uint32*)(sRegisters[bank.controller] + offset);
}


static inline void
write_reg(const pin_bank& bank, uint32 offset, uint32 value)
{
	*(volatile uint32*)(sRegisters[bank.controller] + offset) = value;
}


static inline void
update_reg(const pin_bank& bank, uint32 offset, uint32 mask, uint32 value)
{
	write_reg(bank, offset, (read_reg(bank, offset) & ~mask) | value);
}


static inline uint32
data_offset(const pin_bank& bank)
{
	return bank.registers + 0x10;
}


static uint8
get_function(uint32 pin)
{
	const pin_location& location = sPins[pin];
	if (location.bank == NULL)
		return FUNCTION_INPUT;
	uint32 shift = (location.number % 8) * 4;
	return (read_reg(*location.bank, location.bank->registers
		+ (location.number / 8) * 4) >> shift) & 0xf;
}


static void
set_function(uint32 pin, uint8 function)
{
	const pin_location& location = sPins[pin];
	uint32 shift = (location.number % 8) * 4;
	update_reg(*location.bank, location.bank->registers
		+ (location.number / 8) * 4, 0xfu << shift, (uint32)function << shift);
}


static uint8
get_pull(uint32 pin)
{
	const pin_location& location = sPins[pin];
	if (location.bank == NULL)
		return RPI_GPIO_PULL_NONE;
	uint32 shift = (location.number % 16) * 2;
	return (read_reg(*location.bank, location.bank->pull
		+ (location.number / 16) * 4) >> shift) & 3;
}


static void
set_pull(uint32 pin, uint8 pull)
{
	const pin_location& location = sPins[pin];
	uint32 shift = (location.number % 16) * 2;
	update_reg(*location.bank, location.bank->pull
		+ (location.number / 16) * 4, 3u << shift, (uint32)pull << shift);
}


static void
set_level(uint32 pin, uint8 level)
{
	const pin_location& location = sPins[pin];
	uint32 bit = 1u << location.number;
	update_reg(*location.bank, data_offset(*location.bank), bit,
		level != 0 ? bit : 0);
}


//! The levels of all header pins, bit n for pin n.
static uint64
read_levels()
{
	uint32 data[BANK_COUNT];
	for (uint32 i = 0; i < BANK_COUNT; i++)
		data[i] = read_reg(sBanks[i], data_offset(sBanks[i]));

	uint64 levels = 0;
	for (uint32 pin = 0; pin < PIN_COUNT; pin++) {
		const pin_location& location = sPins[pin];
		if (location.bank != NULL
			&& (data[location.bank - sBanks] >> location.number & 1) != 0) {
			levels |= 1ull << pin;
		}
	}
	return levels;
}


static inline uint8
pin_level(uint64 levels, uint32 pin)
{
	return (levels >> pin) & 1;
}


//	#pragma mark - events (sEventLock held)


static void
record_event(uint32 pin, uint8 level, uint8 flags, bigtime_t time)
{
	rpi_gpio_event& event = sEvents[sEventCount % EVENT_RING_SIZE];
	event.time = time;
	event.pin = pin;
	event.level = level;
	event.flags = flags;
	memset(event._reserved, 0, sizeof(event._reserved));
	sEventCount++;

	if (level != 0)
		sLastLevels |= 1ull << pin;
	else
		sLastLevels &= ~(1ull << pin);
}


/*!	Records the inputs in \a pins that had an edge. One whose level is the
	one last recorded had two: a pulse that ended before it was looked at.
*/
static void
record_edges(uint64 pins, uint64 levels, uint8 flags, bigtime_t time)
{
	while (pins != 0) {
		uint32 pin = __builtin_ctzll(pins);
		pins &= pins - 1;
		uint8 level = pin_level(levels, pin);
		if (pin_level(sLastLevels, pin) == level)
			record_event(pin, !level, flags, time);
		record_event(pin, level, flags, time);
	}
}


//! Turns the interrupts of the inputs in \a pins on or off.
static void
enable_interrupts(uint64 pins, bool on)
{
	for (uint32 i = 0; i < BANK_COUNT; i++) {
		pin_bank& bank = sBanks[i];
		uint64 bankPins = pins & bank.positions;
		if (bankPins == 0)
			continue;
		uint32 bits = 0;
		for (; bankPins != 0; bankPins &= bankPins - 1)
			bits |= 1u << sPins[__builtin_ctzll(bankPins)].number;

		if (on) {
			// nothing that happened before counts
			write_reg(bank, bank.interruptRegisters + 0x14, bits);
			update_reg(bank, bank.interruptRegisters + 0x10, bits, bits);
		} else {
			update_reg(bank, bank.interruptRegisters + 0x10, bits, 0);
			write_reg(bank, bank.interruptRegisters + 0x14, bits);
		}
	}
}


static void
set_edge_detection(uint32 pin, bool on)
{
	uint64 bit = 1ull << pin;
	if (on)
		sEdgeMask |= bit;
	else
		sEdgeMask &= ~bit;

	if ((bit & sInterruptMask) == 0 || (on && sThrottled))
		return;

	if (on) {
		const pin_location& location = sPins[pin];
		uint32 shift = (location.number % 8) * 4;
		update_reg(*location.bank, location.bank->interruptRegisters
			+ (location.number / 8) * 4, 0xfu << shift,
			(uint32)IRQ_BOTH_EDGES << shift);
	}
	enable_interrupts(bit, on);
}


/*!	Samples the inputs: the period of a throttled interrupt ends, or a bank
	has no interrupt.
*/
static int32
sample_inputs(timer* timer)
{
	SpinLocker locker(sEventLock);

	uint64 pins = sEdgeMask & sNoInterrupt;
	if (timer == &sThrottleTimer) {
		pins = sEdgeMask;
		if (sThrottled) {
			sThrottled = false;
			sWindowStart = system_time();
			sWindowCount = 0;
			enable_interrupts(sEdgeMask & sInterruptMask, true);
		}
	}

	uint64 levels = read_levels();
	uint64 changed = (levels ^ sLastLevels) & pins;
	if (changed == 0)
		return B_HANDLED_INTERRUPT;

	bigtime_t now = system_time();
	while (changed != 0) {
		uint32 pin = __builtin_ctzll(changed);
		changed &= changed - 1;
		record_event(pin, pin_level(levels, pin), RPI_GPIO_EVENT_SAMPLED, now);
	}
	return sEventCondition.NotifyAll() > 0
		? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


static int32
gpio_interrupt(void* data)
{
	pin_bank& bank = *(pin_bank*)data;
	SpinLocker locker(sEventLock);

	uint32 status = read_reg(bank, bank.interruptRegisters + 0x14)
		& bank.headerBits;
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;
	write_reg(bank, bank.interruptRegisters + 0x14, status);
	uint64 levels = read_levels();
	bigtime_t now = system_time();

	uint64 pins = 0;
	for (uint64 bankPins = bank.positions; bankPins != 0;
			bankPins &= bankPins - 1) {
		uint32 pin = __builtin_ctzll(bankPins);
		if ((status >> sPins[pin].number & 1) != 0)
			pins |= 1ull << pin;
	}
	pins &= sEdgeMask & sInterruptMask;
	record_edges(pins, levels, 0, now);

	if (now - sWindowStart > THROTTLE_WINDOW) {
		sWindowStart = now;
		sWindowCount = 0;
	}
	sWindowCount += __builtin_popcountll(pins);
	if (sWindowCount > THROTTLE_EVENTS && !sThrottled) {
		// too fast to report every edge: look again in a while
		sThrottled = true;
		enable_interrupts(sEdgeMask & sInterruptMask, false);
		add_timer(&sThrottleTimer, &sample_inputs, THROTTLE_PAUSE,
			B_ONE_SHOT_RELATIVE_TIMER);
	}

	return sEventCondition.NotifyAll() > 0
		? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


//	#pragma mark - pins (sLock held)


/*!	The function a program sees: an input of ours in its interrupt function
	is an input.
*/
static uint8
visible_function(uint32 pin)
{
	uint8 function = get_function(pin);
	if (function == FUNCTION_INTERRUPT && sOwner[pin] != NULL
		&& (sInterruptMask & (1ull << pin)) != 0) {
		return FUNCTION_INPUT;
	}
	return function;
}


static void
restore_pin(gpio_client* client, uint32 pin)
{
	uint64 bit = 1ull << pin;
	{
		InterruptsSpinLocker locker(sEventLock);
		set_edge_detection(pin, false);
		sInterruptMask &= ~bit;
		client->claimed &= ~bit;
	}
	sOwner[pin] = NULL;

	const saved_pin& saved = sSaved[pin];
	if (saved.function == FUNCTION_OUTPUT)
		set_level(pin, saved.level);
	set_function(pin, saved.function);
	set_pull(pin, saved.pull);
	TRACE("pin %" B_PRIu32 " back to function %u pull %u\n", pin,
		saved.function, saved.pull);
}


static status_t
claim_pin(gpio_client* client, const rpi_gpio_claim& request)
{
	uint32 pin = request.pin;
	if (pin >= PIN_COUNT || (sClaimable & (1ull << pin)) == 0
		|| (request.function != RPI_GPIO_INPUT
			&& request.function != RPI_GPIO_OUTPUT)
		|| request.pull > RPI_GPIO_PULL_DOWN
		|| request.level < -1 || request.level > 1
		|| (request.flags & ~RPI_GPIO_CLAIM_DETACH) != 0) {
		return B_BAD_VALUE;
	}
	bool detach = (request.flags & RPI_GPIO_CLAIM_DETACH) != 0;
	uint64 bit = 1ull << pin;

	MutexLocker locker(sLock);
	if (sOwner[pin] != NULL && sOwner[pin] != client)
		return B_BUSY;
	bool owned = sOwner[pin] == client;

	if (!owned && !detach) {
		sSaved[pin].function = get_function(pin);
		sSaved[pin].pull = get_pull(pin);
		sSaved[pin].level = pin_level(read_levels(), pin);
	}

	// An input of ours that only gets another pull keeps reporting its
	// edges: the change of level is one. Anything else reports nothing
	// while it changes.
	bool keepEdges = owned && !detach && request.function == RPI_GPIO_INPUT
		&& visible_function(pin) == FUNCTION_INPUT;
	if (!keepEdges) {
		InterruptsSpinLocker eventLocker(sEventLock);
		set_edge_detection(pin, false);
		sInterruptMask &= ~bit;
	}

	// a detached input gets no interrupt: nobody would answer it
	bool interrupt = request.function == RPI_GPIO_INPUT && !detach
		&& (sNoInterrupt & bit) == 0;
	if (request.function == RPI_GPIO_OUTPUT) {
		// the output starts at the level asked for, or the one it has
		int8 level = request.level;
		if (level < 0)
			level = pin_level(read_levels(), pin);
		set_level(pin, level);
		set_function(pin, FUNCTION_OUTPUT);
	} else if (!keepEdges)
		set_function(pin, interrupt ? FUNCTION_INTERRUPT : FUNCTION_INPUT);
	set_pull(pin, request.pull);

	InterruptsSpinLocker eventLocker(sEventLock);
	if (detach) {
		sOwner[pin] = NULL;
		client->claimed &= ~bit;
		return B_OK;
	}

	sOwner[pin] = client;
	client->claimed |= bit;
	if (!keepEdges) {
		if (interrupt)
			sInterruptMask |= bit;
		if (request.function == RPI_GPIO_INPUT)
			set_edge_detection(pin, true);
		// the level it starts with, or the one it changed to
		uint8 level = pin_level(read_levels(), pin);
		if (!owned || level != pin_level(sLastLevels, pin)) {
			record_event(pin, level, RPI_GPIO_EVENT_CLAIMED, system_time());
			sEventCondition.NotifyAll();
		}
	}

	TRACE("pin %" B_PRIu32 " claimed: function %u pull %u level %d\n", pin,
		request.function, request.pull, request.level);
	return B_OK;
}


static status_t
release_pin(gpio_client* client, uint32 pin)
{
	MutexLocker locker(sLock);
	if (pin >= PIN_COUNT || sOwner[pin] != client)
		return B_BAD_VALUE;

	restore_pin(client, pin);
	return B_OK;
}


static status_t
write_pins(gpio_client* client, const rpi_gpio_write& request)
{
	MutexLocker locker(sLock);
	if ((request.mask & ~client->claimed) != 0)
		return B_NOT_ALLOWED;

	uint64 mask = request.mask;
	for (uint64 pins = mask; pins != 0; pins &= pins - 1) {
		if (get_function(__builtin_ctzll(pins)) != FUNCTION_OUTPUT)
			return B_NOT_ALLOWED;
	}

	for (uint64 pins = mask; pins != 0; pins &= pins - 1) {
		uint32 pin = __builtin_ctzll(pins);
		set_level(pin, pin_level(request.levels, pin));
	}

	InterruptsSpinLocker eventLocker(sEventLock);
	bigtime_t now = system_time();
	uint64 changed = (request.levels ^ sLastLevels) & mask;
	while (changed != 0) {
		uint32 pin = __builtin_ctzll(changed);
		changed &= changed - 1;
		record_event(pin, pin_level(request.levels, pin),
			RPI_GPIO_EVENT_WRITTEN, now);
	}
	sEventCondition.NotifyAll();
	return B_OK;
}


static void
get_state(gpio_client* client, rpi_gpio_state& state)
{
	memset(&state, 0, sizeof(state));

	MutexLocker locker(sLock);
	state.levels = read_levels();
	state.time = system_time();
	for (uint32 pin = 0; pin < PIN_COUNT; pin++) {
		state.function[pin] = visible_function(pin);
		state.pull[pin] = get_pull(pin);
		if (sOwner[pin] == client)
			state.claimed |= 1ull << pin;
		else if (sOwner[pin] != NULL)
			state.claimed_elsewhere |= 1ull << pin;
	}
}


static status_t
wait_events(gpio_client* client, rpi_gpio_wait& request)
{
	if (request.capacity == 0 || request.events == NULL
		|| !IS_USER_ADDRESS(request.events)) {
		return B_BAD_VALUE;
	}
	uint32 capacity = min_c(request.capacity, (uint32)WAIT_BATCH);

	bigtime_t deadline = B_INFINITE_TIMEOUT;
	if (request.timeout <= 0)
		deadline = 0;
	else if (request.timeout != B_INFINITE_TIMEOUT)
		deadline = system_time() + request.timeout;

	MutexLocker waitLocker(client->waitLock);
	InterruptsSpinLocker locker(sEventLock);
	uint32 count = 0;
	status_t status = B_OK;
	while (true) {
		if (client->closed) {
			status = B_FILE_ERROR;
			break;
		}
		if (sEventCount - client->readPosition > EVENT_RING_SIZE) {
			uint64 oldest = sEventCount - EVENT_RING_SIZE;
			client->lost += oldest - client->readPosition;
			client->readPosition = oldest;
		}
		while (client->readPosition < sEventCount && count < capacity) {
			const rpi_gpio_event& event
				= sEvents[client->readPosition % EVENT_RING_SIZE];
			client->readPosition++;
			if ((client->claimed & (1ull << event.pin)) != 0)
				client->batch[count++] = event;
		}
		if (count > 0 || deadline == 0
			|| (deadline != B_INFINITE_TIMEOUT && system_time() >= deadline)) {
			break;
		}

		ConditionVariableEntry entry;
		sEventCondition.Add(&entry);
		locker.Unlock();
		status = entry.Wait(B_CAN_INTERRUPT
			| (deadline != B_INFINITE_TIMEOUT ? B_ABSOLUTE_TIMEOUT : 0),
			deadline);
		locker.Lock();
		if (status == B_INTERRUPTED)
			break;
		status = B_OK;
	}

	request.count = count;
	request.lost = client->lost;
	client->lost = 0;
	locker.Unlock();

	if (status != B_OK)
		return status;
	if (count > 0 && user_memcpy(request.events, client->batch,
			count * sizeof(rpi_gpio_event)) != B_OK) {
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


//	#pragma mark - device hooks


static status_t
gpio_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static status_t
gpio_open(void* deviceCookie, const char* path, int openMode, void** _cookie)
{
	if (sRegisters[PIO] == NULL)
		return B_DEVICE_NOT_FOUND;

	gpio_client* client = new(std::nothrow) gpio_client;
	if (client == NULL)
		return B_NO_MEMORY;
	mutex_init(&client->waitLock, "sunxi_gpio wait");
	client->claimed = 0;
	client->lost = 0;
	client->closed = false;
	{
		InterruptsSpinLocker locker(sEventLock);
		client->readPosition = sEventCount;
	}

	MutexLocker locker(sLock);
	if (sOpenCount++ == 0 && sNoInterrupt != 0) {
		sSampling = true;
		add_timer(&sSampleTimer, &sample_inputs, SAMPLE_PERIOD,
			B_PERIODIC_TIMER);
	}

	*_cookie = client;
	return B_OK;
}


static status_t
gpio_close(void* cookie)
{
	gpio_client* client = (gpio_client*)cookie;

	// a thread that waits for events gives up
	InterruptsSpinLocker locker(sEventLock);
	client->closed = true;
	sEventCondition.NotifyAll();
	return B_OK;
}


static status_t
gpio_free(void* cookie)
{
	gpio_client* client = (gpio_client*)cookie;

	MutexLocker locker(sLock);
	for (uint32 pin = 0; pin < PIN_COUNT; pin++) {
		if (sOwner[pin] == client)
			restore_pin(client, pin);
	}
	if (--sOpenCount == 0 && sSampling) {
		// the hook takes sEventLock only, so waiting for it here is fine
		cancel_timer(&sSampleTimer);
		sSampling = false;
	}
	locker.Unlock();

	mutex_destroy(&client->waitLock);
	delete client;
	return B_OK;
}


static status_t
gpio_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	gpio_client* client = (gpio_client*)cookie;

	if (!IS_USER_ADDRESS(buffer))
		return B_BAD_ADDRESS;

	switch (op) {
		case RPI_GPIO_GET_INFO:
		{
			rpi_gpio_info info;
			memset(&info, 0, sizeof(info));
			info.api_version = RPI_GPIO_API_VERSION;
			info.pin_count = PIN_COUNT;
			info.claimable = sClaimable;
			info.board = RPI_GPIO_BOARD_CUBIE_A7S;
			if ((sNoInterrupt & sClaimable) == 0)
				info.flags |= RPI_GPIO_INFO_EDGE_INTERRUPTS;
			return user_memcpy(buffer, &info, sizeof(info));
		}

		case RPI_GPIO_GET_STATE:
		{
			rpi_gpio_state state;
			get_state(client, state);
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case RPI_GPIO_CLAIM:
		{
			rpi_gpio_claim request;
			if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
				return B_BAD_ADDRESS;
			return claim_pin(client, request);
		}

		case RPI_GPIO_RELEASE:
		{
			uint32 pin;
			if (user_memcpy(&pin, buffer, sizeof(pin)) != B_OK)
				return B_BAD_ADDRESS;
			return release_pin(client, pin);
		}

		case RPI_GPIO_WRITE:
		{
			rpi_gpio_write request;
			if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
				return B_BAD_ADDRESS;
			return write_pins(client, request);
		}

		case RPI_GPIO_WAIT_EVENTS:
		{
			rpi_gpio_wait request;
			if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
				return B_BAD_ADDRESS;
			status_t status = wait_events(client, request);
			if (status != B_OK && status != B_INTERRUPTED)
				return status;
			if (user_memcpy(buffer, &request, sizeof(request)) != B_OK)
				return B_BAD_ADDRESS;
			return status;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
gpio_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
gpio_write(void* cookie, off_t position, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


//	#pragma mark - driver


/*!	Whether the device tree's root node says the board is a Cubie A7S: its
	header map is this board's alone.
*/
static bool
is_cubie_a7s(device_node* node)
{
	// the PIO node itself is not the root
	device_node* current = sDeviceManager->get_parent_node(node);
	bool found = false;
	while (current != NULL) {
		uint32 fdtNode;
		if (sDeviceManager->get_attr_uint32(current, "fdt/node", &fdtNode,
				false) == B_OK && fdtNode == 0) {
			device_attr* attr = NULL;
			while (sDeviceManager->get_next_attr(current, &attr) == B_OK) {
				if (strcmp(attr->name, "fdt/compatible") == 0
					&& attr->type == B_STRING_TYPE
					&& strcmp(attr->value.string, BOARD_COMPATIBLE) == 0) {
					found = true;
				}
			}
			sDeviceManager->put_node(current);
			break;
		}
		device_node* parent = sDeviceManager->get_parent_node(current);
		sDeviceManager->put_node(current);
		current = parent;
	}
	return found;
}


//! The registers and interrupts of an FDT node.
static status_t
map_controller(device_node* node, uint32 controller)
{
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(node,
		(driver_module_info**)&fdt, (void**)&device);
	if (status != B_OK)
		return status;

	uint64 base, size;
	if (!fdt->get_reg(device, 0, &base, &size))
		return B_BAD_DATA;

	phys_addr_t page = base & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* address;
	sRegistersArea[controller] = map_physical_memory(
		controller == PIO ? "sunxi_gpio pio" : "sunxi_gpio r_pio", page,
		ROUNDUP(base + size - page, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (sRegistersArea[controller] < 0)
		return sRegistersArea[controller];
	sRegisters[controller] = (volatile uint8*)address + (base - page);

	for (uint32 i = 0; i < BANK_COUNT; i++) {
		pin_bank& bank = sBanks[i];
		if (bank.controller != controller)
			continue;
		uint64 vector;
		if (fdt->get_interrupt(device, bank.interruptIndex, NULL, &vector))
			bank.vector = vector;
		else
			bank.vector = 0;
	}
	return B_OK;
}


static void
unmap_controllers()
{
	for (uint32 controller = 0; controller < 2; controller++) {
		if (sRegistersArea[controller] >= 0)
			delete_area(sRegistersArea[controller]);
		sRegistersArea[controller] = -1;
		sRegisters[controller] = NULL;
	}
}


static void
init_tables()
{
	for (uint32 i = 0; i < BANK_COUNT; i++) {
		pin_bank& bank = sBanks[i];
		if (bank.controller == PIO) {
			uint32 number = bank.port - 'A';
			bank.registers = 0x80 + 0x80 * number;
			bank.interruptRegisters = bank.registers + 0x40;
			bank.interruptIndex = bank.port - 'B';
			bank.pull = bank.registers + 0x30;
		} else {
			uint32 number = bank.port - 'L';
			bank.registers = 0x30 * number;
			bank.interruptRegisters = 0x200 + 0x20 * number;
			bank.interruptIndex = number;
			bank.pull = bank.registers + 0x24;
		}
		bank.headerBits = 0;
		bank.positions = 0;
	}

	memset(sPins, 0, sizeof(sPins));
	sClaimable = 0;
	for (uint32 i = 0; i < HEADER_GPIOS; i++) {
		const header_pin& pin = kHeader[i];
		for (uint32 j = 0; j < BANK_COUNT; j++) {
			pin_bank& bank = sBanks[j];
			if (bank.port != pin.port)
				continue;
			bank.headerBits |= 1u << pin.number;
			bank.positions |= 1ull << pin.position;
			sPins[pin.position].bank = &bank;
			sPins[pin.position].number = pin.number;
			sPins[pin.position].claimable = pin.claimable;
			if (pin.claimable)
				sClaimable |= 1ull << pin.position;
		}
	}
}


static void
install_interrupts()
{
	sNoInterrupt = 0;
	for (uint32 i = 0; i < BANK_COUNT; i++) {
		pin_bank& bank = sBanks[i];
		bank.interruptInstalled = false;

		// Nothing on the header reports edges until a program asks.
		update_reg(bank, bank.interruptRegisters + 0x10, bank.headerBits, 0);
		write_reg(bank, bank.interruptRegisters + 0x14, bank.headerBits);

		uint32 others = read_reg(bank, bank.interruptRegisters + 0x10)
			& ~bank.headerBits;
		status_t status = B_OK;
		if (bank.vector == 0)
			status = B_DEVICE_NOT_FOUND;
		else if (others != 0) {
			// enabled by someone else, whom nobody here would answer
			status = B_BUSY;
		} else {
			status = install_io_interrupt_handler(bank.vector, &gpio_interrupt,
				&bank, 0);
		}
		if (status == B_OK)
			bank.interruptInstalled = true;
		else {
			ERROR("port %c: interrupt %" B_PRIu32 ": %s (others enabled %#"
				B_PRIx32 "); its pins are sampled\n", bank.port, bank.vector,
				strerror(status), others);
			sNoInterrupt |= bank.positions;
		}
	}
}


static void
remove_interrupts()
{
	InterruptsSpinLocker locker(sEventLock);
	for (uint32 i = 0; i < BANK_COUNT; i++) {
		pin_bank& bank = sBanks[i];
		update_reg(bank, bank.interruptRegisters + 0x10, bank.headerBits, 0);
		write_reg(bank, bank.interruptRegisters + 0x14, bank.headerBits);
	}
	locker.Unlock();

	for (uint32 i = 0; i < BANK_COUNT; i++) {
		pin_bank& bank = sBanks[i];
		if (bank.interruptInstalled)
			remove_io_interrupt_handler(bank.vector, &gpio_interrupt, &bank);
		bank.interruptInstalled = false;
	}
	cancel_timer(&sThrottleTimer);
}


static float
gpio_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}
	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK || strcmp(compatible, PIO_COMPATIBLE) != 0
		|| !is_cubie_a7s(parent)) {
		return 0.0f;
	}
	return 0.6f;
}


static status_t
gpio_register_device(device_node* parent)
{
	// kept loaded: claimed pins and the event ring live in the driver
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Radxa Cubie A7S pin headers" } },
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED } },
		{}
	};
	return sDeviceManager->register_node(parent, DRIVER_MODULE_NAME, attrs,
		NULL, NULL);
}


static status_t
gpio_init_driver(device_node* node, void** _cookie)
{
	init_tables();

	device_node* pio = sDeviceManager->get_parent_node(node);
	status_t status = map_controller(pio, PIO);
	sDeviceManager->put_node(pio);

	device_node* root = sDeviceManager->get_root_node();
	device_attr attrs[] = {
		{ "fdt/compatible", B_STRING_TYPE, { .string = R_PIO_COMPATIBLE } },
		{}
	};
	device_node* rPio = NULL;
	if (status == B_OK && root != NULL
		&& sDeviceManager->find_child_node(root, attrs, &rPio) == B_OK) {
		status = map_controller(rPio, R_PIO);
		sDeviceManager->put_node(rPio);
	} else if (status == B_OK)
		status = B_DEVICE_NOT_FOUND;
	if (root != NULL)
		sDeviceManager->put_node(root);

	if (status != B_OK) {
		ERROR("no PIO and R_PIO registers in the device tree: %s\n",
			strerror(status));
		unmap_controllers();
		return status;
	}

	sEventCondition.Init(sEvents, "sunxi_gpio events");
	install_interrupts();
	sLastLevels = read_levels();

	dprintf("sunxi_gpio: %u header pins, %u claimable, levels %#" B_PRIx64
		"\n", (unsigned)HEADER_GPIOS, (unsigned)__builtin_popcountll(sClaimable),
		sLastLevels);
	*_cookie = node;
	return B_OK;
}


static void
gpio_uninit_driver(void* cookie)
{
	remove_interrupts();
	unmap_controllers();
}


static status_t
gpio_register_child_devices(void* cookie)
{
	return sDeviceManager->publish_device((device_node*)cookie,
		"misc/sunxi_gpio", DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static device_module_info sGpioDevice = {
	{
		DEVICE_MODULE_NAME,
		0,
		NULL
	},
	gpio_init_device,
	NULL,	// uninit
	NULL,	// removed
	gpio_open,
	gpio_close,
	gpio_free,
	gpio_read,
	gpio_write,
	NULL,	// io
	gpio_control,
	NULL,	// select
	NULL	// deselect
};


static driver_module_info sGpioDriver = {
	{
		DRIVER_MODULE_NAME,
		0,
		NULL
	},
	gpio_supports_device,
	gpio_register_device,
	gpio_init_driver,
	gpio_uninit_driver,
	gpio_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sGpioDriver,
	(module_info*)&sGpioDevice,
	NULL
};
