/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <arch/generic/debug_uart.h>
#include <arch/arm64/arch_uart_geni.h>
#include <new>


#define SE_GENI_STATUS			0x040
#define SE_GENI_FW_REVISION		0x068
#define SE_UART_TX_TRANS_LEN	0x270
#define SE_GENI_M_CMD0			0x600
#define SE_GENI_M_CMD_CTRL		0x604
#define SE_GENI_M_IRQ_STATUS	0x610
#define SE_GENI_M_IRQ_CLEAR		0x618
#define SE_GENI_S_CMD0			0x630
#define SE_GENI_S_IRQ_STATUS	0x640
#define SE_GENI_S_IRQ_CLEAR		0x648
#define SE_GENI_TX_FIFO			0x700
#define SE_GENI_RX_FIFO			0x780
#define SE_GENI_RX_FIFO_STATUS	0x804

#define M_GENI_CMD_ACTIVE		(1 << 0)
#define S_GENI_CMD_ACTIVE		(1 << 12)
#define M_GENI_CMD_ABORT		(1 << 1)
#define M_CMD_DONE				(1 << 0)
#define M_CMD_ABORT				(1 << 5)
#define OPCODE_SHIFT			27
#define UART_START_TX			1
#define UART_START_READ			1

#define FW_PROTOCOL(x)			(((x) >> 8) & 0xff)
#define FW_PROTOCOL_UART		2

#define RX_FIFO_WORDS(x)		((x) & 0x1ffffff)
#define RX_LAST					(1u << 31)
#define RX_LAST_BYTES(x)		(((x) >> 28) & 0x7)

// Bounded polls: a stuck engine must not hang the boot.
#define POLL_LIMIT				1000000


ArchUARTGeni::ArchUARTGeni(addr_t base, int64 clock)
	:
	DebugUART(base, clock),
	fRxWord(0),
	fRxBytes(0)
{
}


ArchUARTGeni::~ArchUARTGeni()
{
}


void
ArchUARTGeni::Out32(int reg, uint32 data)
{
	*(volatile uint32*)(Base() + reg) = data;
}


uint32
ArchUARTGeni::In32(int reg)
{
	return *(volatile uint32*)(Base() + reg);
}


bool
ArchUARTGeni::WaitFor(int reg, uint32 mask)
{
	for (int i = 0; i < POLL_LIMIT; i++) {
		if ((In32(reg) & mask) != 0)
			return true;
	}
	return false;
}


void
ArchUARTGeni::InitEarly()
{
	// Only drive an engine the firmware left in UART mode.
	if (FW_PROTOCOL(In32(SE_GENI_FW_REVISION)) != FW_PROTOCOL_UART)
		Disable();
}


void
ArchUARTGeni::InitPort(uint32 baud)
{
	// The firmware set the baud rate; keep it.
}


int
ArchUARTGeni::PutChar(char c)
{
	if (!Enabled())
		return -1;

	// One transfer per character: slow, but independent of the FIFO packing
	// the firmware chose (the first byte of a word is its low byte either way).
	Out32(SE_GENI_M_IRQ_CLEAR, M_CMD_DONE);
	Out32(SE_UART_TX_TRANS_LEN, 1);
	Out32(SE_GENI_M_CMD0, UART_START_TX << OPCODE_SHIFT);
	Out32(SE_GENI_TX_FIFO, (uint8)c);

	if (!WaitFor(SE_GENI_M_IRQ_STATUS, M_CMD_DONE)) {
		Out32(SE_GENI_M_CMD_CTRL, M_GENI_CMD_ABORT);
		WaitFor(SE_GENI_M_IRQ_STATUS, M_CMD_ABORT);
		Out32(SE_GENI_M_IRQ_CLEAR, M_CMD_ABORT);
		return -1;
	}
	Out32(SE_GENI_M_IRQ_CLEAR, M_CMD_DONE);
	return 0;
}


void
ArchUARTGeni::StartRead()
{
	if ((In32(SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) == 0) {
		Out32(SE_GENI_S_IRQ_CLEAR, 0xffffffff);
		Out32(SE_GENI_S_CMD0, UART_START_READ << OPCODE_SHIFT);
	}
}


int
ArchUARTGeni::GetChar(bool wait)
{
	if (!Enabled())
		return -1;

	do {
		if (fRxBytes > 0) {
			int c = fRxWord & 0xff;
			fRxWord >>= 8;
			fRxBytes--;
			return c;
		}

		StartRead();
		uint32 status = In32(SE_GENI_RX_FIFO_STATUS);
		if (RX_FIFO_WORDS(status) != 0) {
			fRxWord = In32(SE_GENI_RX_FIFO);
			fRxBytes = 4;
			if (RX_FIFO_WORDS(status) == 1 && (status & RX_LAST) != 0
				&& RX_LAST_BYTES(status) != 0) {
				fRxBytes = RX_LAST_BYTES(status);
			}
		}
	} while (wait || fRxBytes > 0);

	return -1;
}


void
ArchUARTGeni::FlushTx()
{
	// PutChar waits for each character.
}


void
ArchUARTGeni::FlushRx()
{
	fRxBytes = 0;
}


ArchUARTGeni*
arch_get_uart_geni(addr_t base, int64 clock)
{
	static char buffer[sizeof(ArchUARTGeni)];
	ArchUARTGeni* uart = new(buffer) ArchUARTGeni(base, clock);
	return uart;
}
