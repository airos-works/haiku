/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef __DEV_UART_GENI_H
#define __DEV_UART_GENI_H


#include <arch/generic/debug_uart.h>


#define UART_KIND_GENI "geni"


// Qualcomm GENI serial engine in UART mode, as left configured by the
// firmware (the "qcom,geni-debug-uart" of Snapdragon SoCs). The firmware
// must have the engine's clocks running; nothing here touches clocks or
// the baud rate.
class ArchUARTGeni : public DebugUART {
public:
						ArchUARTGeni(addr_t base, int64 clock);
						~ArchUARTGeni();

			void		InitEarly();
			void		InitPort(uint32 baud);

			int			PutChar(char c);
			int			GetChar(bool wait);

			void		FlushTx();
			void		FlushRx();

private:
			void		Out32(int reg, uint32 data);
			uint32		In32(int reg);
			bool		WaitFor(int reg, uint32 mask);
			void		StartRead();

			uint32		fRxWord;
			int			fRxBytes;
};


ArchUARTGeni* arch_get_uart_geni(addr_t base, int64 clock);


#endif
