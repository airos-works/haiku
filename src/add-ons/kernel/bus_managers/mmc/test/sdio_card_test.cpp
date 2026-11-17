/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Host test of the MMC bus' SDIO protocol (sdio_card.cpp) against a
	simulated SDIO card: CMD5 identification, the CCCR, FBR and CIS parsing,
	the framing of CMD52 and CMD53 (block and byte mode, incrementing and
	fixed addresses), and the interrupt bookkeeping. The card checks every
	argument the way a card does: a CMD53 whose count field does not fit the
	data the host sends, or whose block size is not the function's, fails. */


#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <vector>

#include "sdio_card.h"


static int sFailures;

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			sFailures++; \
		} \
	} while (false)


struct Transfer {
	uint32	argument;
	bool	write;
	uint16	blockSize;
	uint16	blockCount;
};


class FakeCard {
public:
	FakeCard(uint8 functions, uint32 busyPolls)
		:
		fFunctions(functions),
		fBusyPolls(busyPolls),
		fAnswersCmd5(true),
		fRca(0),
		fReady(false),
		fClock(0)
	{
		fSpace[0].assign(0x20000, 0);
		for (int i = 1; i <= functions; i++) {
			fSpace[i].assign(0x20000, 0);
			for (uint32 a = 0; a < 0x20000; a++)
				fSpace[i][a] = (uint8)(a * 7 + i);
		}
	}

	void PutCis(uint32 pointerRegister, uint32 address,
		const std::vector<uint8>& cis)
	{
		for (int i = 0; i < 3; i++)
			fSpace[0][pointerRegister + i] = (address >> (8 * i)) & 0xff;
		memcpy(&fSpace[0][address], cis.data(), cis.size());
	}

	status_t Command(uint8 command, uint32 argument, uint32* response)
	{
		fCommands.push_back(std::make_pair(command, argument));
		switch (command) {
			case 5:
			{
				if (!fAnswersCmd5)
					return B_TIMED_OUT;
				uint32 ocr = 0x00ff8000 | ((uint32)fFunctions << 28);
				if (argument != 0) {
					fVoltage = argument;
					if (fBusyPolls > 0)
						fBusyPolls--;
					else {
						fReady = true;
						ocr |= SDIO_OCR_READY;
					}
				}
				*response = ocr;
				return B_OK;
			}
			case 3:
				if (!fReady)
					return B_TIMED_OUT;
				fRca = 0x0001;
				*response = (uint32)fRca << 16 | 0x0500;
				return B_OK;
			case 52:
				return Direct(argument, response);
		}
		return B_TIMED_OUT;
	}

	status_t Direct(uint32 argument, uint32* response)
	{
		if (fRca == 0)
			return B_TIMED_OUT;
		bool write = (argument & SDIO_ARG_WRITE) != 0;
		uint8 function = (argument >> 28) & 7;
		bool raw = (argument & SDIO_ARG_RAW) != 0;
		uint32 address = (argument >> 9) & 0x1ffff;
		uint8 data = argument & 0xff;
		CHECK((argument & (1 << 26)) == 0 && (argument & (1 << 8)) == 0);
			// stuff bits

		uint32 status = 0x1000;	// CMD state
		if (function > fFunctions) {
			*response = status | SDIO_R5_FUNCTION_NUMBER;
			return B_OK;
		}
		uint8& cell = fSpace[function][address];
		if (write) {
			if (function == 0 && address == SDIO_CCCR_SPEED)
				data = (data & SDIO_SPEED_EHS) | SDIO_SPEED_SHS;
			cell = data;
			fWrites.push_back(argument);
		}
		*response = status | (!write || raw ? cell : 0);
		return B_OK;
	}

	uint16 BlockSize(uint8 function)
	{
		uint32 base = function == 0 ? SDIO_CCCR_BLOCK_SIZE
			: SDIO_FBR_BASE(function) + SDIO_FBR_BLOCK_SIZE;
		return fSpace[0][base] | (fSpace[0][base + 1] << 8);
	}

	status_t Extended(uint32 argument, bool write, uint8* buffer,
		uint16 blockSize, uint16 blockCount, uint32* response)
	{
		fTransfers.push_back(Transfer{argument, write, blockSize, blockCount});
		bool argumentWrite = (argument & SDIO_ARG_WRITE) != 0;
		uint8 function = (argument >> 28) & 7;
		bool blockMode = (argument & SDIO_ARG_BLOCK_MODE) != 0;
		bool increment = (argument & SDIO_ARG_INCREMENT) != 0;
		uint32 address = (argument >> 9) & 0x1ffff;
		uint32 count = argument & 0x1ff;

		// what a card checks, and what the host's data phase must match
		if (argumentWrite != write || function == 0 || function > fFunctions
			|| (blockCount != 0) != blockMode) {
			*response = SDIO_R5_ERROR;
			return B_OK;
		}
		uint32 length;
		if (blockMode) {
			if (count == 0 || count != blockCount
				|| blockSize != BlockSize(function)) {
				*response = SDIO_R5_ERROR;
				return B_OK;
			}
			length = count * blockSize;
		} else {
			length = count == 0 ? 512 : count;
			if (length != blockSize) {
				*response = SDIO_R5_ERROR;
				return B_OK;
			}
		}
		if (increment && address + length > 0x20000) {
			*response = SDIO_R5_OUT_OF_RANGE;
			return B_OK;
		}

		for (uint32 i = 0; i < length; i++) {
			uint32 at = increment ? address + i : address;
			if (write) {
				if (increment)
					fSpace[function][at] = buffer[i];
				else
					fFifo[function].push_back(buffer[i]);
			} else
				buffer[i] = fSpace[function][at] + (increment ? 0 : i);
		}
		*response = 0x1000;
		return B_OK;
	}

	uint8							fFunctions;
	uint32							fBusyPolls;
	bool							fAnswersCmd5;
	uint16							fRca;
	bool							fReady;
	uint32							fVoltage;
	uint32							fClock;
	std::vector<uint8>				fSpace[8];
	std::map<int, std::vector<uint8> > fFifo;
	std::vector<std::pair<uint8, uint32> > fCommands;
	std::vector<uint32>				fWrites;
	std::vector<Transfer>			fTransfers;
};


static bigtime_t sTime;


static status_t
fake_command(void* cookie, uint8 command, uint32 argument, uint32* response)
{
	return ((FakeCard*)cookie)->Command(command, argument, response);
}


static status_t
fake_transfer(void* cookie, uint32 argument, bool write, void* buffer,
	uint16 blockSize, uint16 blockCount, uint32* response)
{
	return ((FakeCard*)cookie)->Extended(argument, write, (uint8*)buffer,
		blockSize, blockCount, response);
}


static void
fake_sleep(void* cookie, bigtime_t duration)
{
	sTime += duration;
}


static bigtime_t
fake_time(void* cookie)
{
	return sTime;
}


static sdio_host_ops
ops_for(FakeCard& card)
{
	sdio_host_ops ops = { &card, fake_command, fake_transfer, fake_sleep,
		fake_time };
	return ops;
}


/*!	A card like the RTL8723BS on the laptop of #133 (vendor 0x024c, device
	0xb723), with a second function that has its own ID and a 1.00 style
	function extension. */
static void
setup_card(FakeCard& card)
{
	std::vector<uint8>& cccr = card.fSpace[0];
	cccr[SDIO_CCCR_REVISION] = 0x43;		// SDIO 3.00, CCCR 3.00
	cccr[SDIO_CCCR_CAPABILITY] = SDIO_CAP_SDC | SDIO_CAP_SMB | SDIO_CAP_S4MI;
	cccr[SDIO_CCCR_SPEED] = SDIO_SPEED_SHS;
	cccr[SDIO_FBR_BASE(1) + SDIO_FBR_INTERFACE] = 0x0f;
	cccr[SDIO_FBR_BASE(1) + SDIO_FBR_INTERFACE_EXT] = 0x09;
	cccr[SDIO_FBR_BASE(2) + SDIO_FBR_INTERFACE] = 0x42;	// CSA bit + code 2

	card.PutCis(SDIO_CCCR_CIS_POINTER, 0x1000, {
		0x00,									// a null tuple
		0x15, 0x05, 0x01, 0x00, 'A', 0x00, 0xff,	// VERS_1
		0x20, 0x04, 0x4c, 0x02, 0x23, 0xb7,		// MANFID 024c:b723
		0x21, 0x02, 0x0c, 0x00,					// FUNCID
		0x22, 0x04, 0x00, 0x00, 0x02, 0x32,		// FUNCE: 512, 25 MHz
		0x80, 0x03, 0x01, 0x02, 0x03,			// a vendor tuple
		0xff
	});

	std::vector<uint8> function1 = { 0x21, 0x02, 0x0c, 0x00, 0x22, 42 };
	std::vector<uint8> funce1(42, 0);
	funce1[0] = 0x01;
	funce1[12] = 0x00;
	funce1[13] = 0x02;						// 512
	funce1[28] = 0x0a;						// 10 * 10 ms
	function1.insert(function1.end(), funce1.begin(), funce1.end());
	function1.push_back(0xff);
	card.PutCis(SDIO_FBR_BASE(1) + SDIO_FBR_CIS_POINTER, 0x1100, function1);

	std::vector<uint8> function2 = { 0x20, 0x04, 0x4c, 0x02, 0x24, 0xb7,
		0x22, 28 };
	std::vector<uint8> funce2(28, 0);
	funce2[0] = 0x01;
	funce2[12] = 64;						// 64, SDIO 1.00 length
	function2.insert(function2.end(), funce2.begin(), funce2.end());
	function2.push_back(0xff);
	card.PutCis(SDIO_FBR_BASE(2) + SDIO_FBR_CIS_POINTER, 0x1200, function2);
}


static void
test_identification()
{
	FakeCard card(2, 3);
	sdio_host_ops ops = ops_for(card);
	sTime = 0;
	uint32 ocr = 0;
	CHECK(sdio_negotiate_ocr(ops, 0x00300000, &ocr) == B_OK);
	CHECK((ocr & SDIO_OCR_READY) != 0);
	CHECK(SDIO_OCR_FUNCTION_COUNT(ocr) == 2);
	CHECK(card.fVoltage == 0x00300000);		// the common window only
	CHECK(card.fCommands.size() == 5);		// inquiry, 3 busy, ready
	CHECK(card.fCommands[0].second == 0);
	CHECK(sTime == 30000);

	// a card that never gets ready
	FakeCard slow(1, 1000);
	ops = ops_for(slow);
	sTime = 0;
	CHECK(sdio_negotiate_ocr(ops, 0x00ff8000, &ocr) == B_TIMED_OUT);

	// a memory card does not answer CMD5
	FakeCard memory(1, 0);
	memory.fAnswersCmd5 = false;
	ops = ops_for(memory);
	CHECK(sdio_negotiate_ocr(ops, 0x00ff8000, &ocr) == B_DEVICE_NOT_FOUND);

	// no common voltage
	FakeCard low(1, 0);
	ops = ops_for(low);
	CHECK(sdio_negotiate_ocr(ops, 0x00000080, &ocr) == B_NOT_SUPPORTED);
	printf("identification: done\n");
}


static void
test_card_reading()
{
	FakeCard card(2, 0);
	setup_card(card);
	sdio_host_ops ops = ops_for(card);
	uint32 ocr;
	CHECK(sdio_negotiate_ocr(ops, 0x00ff8000, &ocr) == B_OK);
	uint32 response;
	CHECK(card.Command(3, 0, &response) == B_OK);

	sdio_card info;
	CHECK(sdio_read_card(ops, ocr, &info) == B_OK);
	CHECK(info.info.vendor == 0x024c && info.info.device == 0xb723);
	CHECK(info.info.function_count == 2);
	CHECK(info.info.cccr_revision == 3 && info.info.sdio_revision == 4);
	CHECK(info.info.capabilities == (SDIO_CAP_SDC | SDIO_CAP_SMB
		| SDIO_CAP_S4MI));
	CHECK(info.info.max_block_size[0] == 512);
	CHECK(info.max_clock == 25000);
	CHECK(info.function_vendor[1] == 0x024c && info.function_device[1] == 0xb723);
	CHECK(info.function_vendor[2] == 0x024c && info.function_device[2] == 0xb724);
	CHECK(info.function_class[1] == 0x09);
	CHECK(info.function_class[2] == 0x02);
	CHECK(info.info.max_block_size[1] == 512);
	CHECK(info.info.max_block_size[2] == 64);
	CHECK(info.enable_timeout[1] == 100);
	CHECK(info.enable_timeout[2] == 1000);
	printf("CIS: %04x:%04x, functions %u, function 2 %04x:%04x max block %u\n",
		info.info.vendor, info.info.device, info.info.function_count,
		info.function_vendor[2], info.function_device[2],
		info.info.max_block_size[2]);

	// a CIS pointer outside the CIS area
	card.fSpace[0][SDIO_FBR_BASE(2) + SDIO_FBR_CIS_POINTER + 1] = 0x00;
	CHECK(sdio_read_card(ops, ocr, &info) == B_BAD_DATA);

	// a CIS without an end: reading stops
	FakeCard endless(1, 0);
	setup_card(endless);
	endless.fFunctions = 1;
	for (uint32 a = 0x1100; a < 0x18000; a++)
		endless.fSpace[0][a] = 0x00;
	ops = ops_for(endless);
	CHECK(sdio_negotiate_ocr(ops, 0x00ff8000, &ocr) == B_OK);
	CHECK(endless.Command(3, 0, &response) == B_OK);
	size_t before = endless.fCommands.size();
	CHECK(sdio_read_card(ops, ocr, &info) == B_OK);
	CHECK(endless.fCommands.size() - before < 5000);
	printf("CIS: done\n");
}


static void
test_direct()
{
	FakeCard card(2, 0);
	setup_card(card);
	sdio_host_ops ops = ops_for(card);
	uint32 ocr, response;
	sdio_negotiate_ocr(ops, 0x00ff8000, &ocr);
	card.Command(3, 0, &response);

	// the Broadcom backplane window write: function 1, 0x1000a, RAW
	uint8 value = 0;
	CHECK(sdio_rw_direct(ops, true, 1, 0x1000a, 0x18, &value, true) == B_OK);
	CHECK(value == 0x18);
	CHECK(card.fWrites.back() == (0x80000000u | 1u << 28 | 1u << 27
		| 0x1000au << 9 | 0x18));
	// without RAW the card does not send the register back
	CHECK(sdio_rw_direct(ops, true, 1, 0x1000b, 0x22, &value) == B_OK);
	CHECK(value == 0);
	CHECK(card.fWrites.back() == (0x80000000u | 1u << 28 | 0x1000bu << 9
		| 0x22));
	CHECK(sdio_rw_direct(ops, false, 1, 0x1000b, 0, &value) == B_OK);
	CHECK(value == 0x22);
	CHECK(card.fCommands.back().second == (1u << 28 | 0x1000bu << 9));

	// errors in R5
	CHECK(sdio_rw_direct(ops, false, 5, 0, 0, &value) == B_BAD_VALUE);
		// the card has two functions
	CHECK(sdio_rw_direct(ops, false, 8, 0, 0, &value) == B_BAD_VALUE);
	CHECK(sdio_rw_direct(ops, false, 1, 0x20000, 0, &value) == B_BAD_VALUE);
	printf("CMD52: done\n");
}


static void
set_block_size(const sdio_host_ops& ops, uint8 function, uint16 size)
{
	uint32 base = SDIO_FBR_BASE(function) + SDIO_FBR_BLOCK_SIZE;
	sdio_rw_direct(ops, true, 0, base, size & 0xff, NULL);
	sdio_rw_direct(ops, true, 0, base + 1, size >> 8, NULL);
}


static void
test_extended()
{
	FakeCard card(2, 0);
	setup_card(card);
	sdio_host_ops ops = ops_for(card);
	uint32 ocr, response;
	sdio_negotiate_ocr(ops, 0x00ff8000, &ocr);
	card.Command(3, 0, &response);
	set_block_size(ops, 1, 512);
	set_block_size(ops, 2, 64);

	// 1300 bytes at 512 byte blocks: two blocks in one command, then 276
	// bytes in byte mode
	std::vector<uint8> buffer(1300);
	card.fTransfers.clear();
	CHECK(sdio_rw_extended(ops, false, 1, 0x8000, true, buffer.data(),
		buffer.size(), 512, true, 512) == B_OK);
	CHECK(card.fTransfers.size() == 2);
	CHECK(card.fTransfers[0].blockCount == 2
		&& card.fTransfers[0].blockSize == 512
		&& card.fTransfers[0].argument == (1u << 28 | 1u << 27 | 1u << 26
			| 0x8000u << 9 | 2));
	CHECK(card.fTransfers[1].blockCount == 0
		&& card.fTransfers[1].blockSize == 276
		&& card.fTransfers[1].argument == (1u << 28 | 1u << 26
			| (0x8000u + 1024) << 9 | 276));
	bool same = true;
	for (uint32 i = 0; i < buffer.size(); i++)
		same &= buffer[i] == card.fSpace[1][0x8000 + i];
	CHECK(same);

	// without multi-block transfers: 512, 512 (count 0) and 276 bytes
	card.fTransfers.clear();
	CHECK(sdio_rw_extended(ops, false, 1, 0x8000, true, buffer.data(),
		buffer.size(), 512, false, 512) == B_OK);
	CHECK(card.fTransfers.size() == 3);
	CHECK((card.fTransfers[0].argument & 0x1ff) == 0
		&& card.fTransfers[0].blockSize == 512
		&& card.fTransfers[0].blockCount == 0);
	CHECK((card.fTransfers[2].argument & 0x1ff) == 276);

	// a write to a FIFO (fixed address): 200 bytes at 64 byte blocks of
	// function 2: three blocks and 8 bytes, all at the same address
	std::vector<uint8> frame(200);
	for (uint32 i = 0; i < frame.size(); i++)
		frame[i] = i;
	card.fTransfers.clear();
	CHECK(sdio_rw_extended(ops, true, 2, 0x8000, false, frame.data(),
		frame.size(), 64, true, 64) == B_OK);
	CHECK(card.fTransfers.size() == 2);
	CHECK(card.fTransfers[0].argument == (0x80000000u | 2u << 28 | 1u << 27
		| 0x8000u << 9 | 3));
	CHECK(card.fTransfers[1].argument == (0x80000000u | 2u << 28
		| 0x8000u << 9 | 8));
	CHECK(card.fFifo[2] == frame);

	// more than 511 blocks: split
	std::vector<uint8> large(600 * 64);
	card.fTransfers.clear();
	CHECK(sdio_rw_extended(ops, false, 2, 0, true, large.data(), large.size(),
		64, true, 64) == B_OK);
	CHECK(card.fTransfers.size() == 2);
	CHECK(card.fTransfers[0].blockCount == 511
		&& card.fTransfers[1].blockCount == 89);

	// no block size set: byte mode in pieces of the byte mode maximum
	card.fTransfers.clear();
	CHECK(sdio_rw_extended(ops, false, 1, 0, true, buffer.data(), 1300, 0,
		true, 512) == B_OK);
	CHECK(card.fTransfers.size() == 3
		&& card.fTransfers[2].blockSize == 276);

	// exactly a block: block mode, as the Raspberry Pi's Wi-Fi was brought
	// up with
	card.fTransfers.clear();
	CHECK(sdio_rw_extended(ops, true, 2, 0x8000, false, frame.data(), 64, 64,
		true, 64) == B_OK);
	CHECK(card.fTransfers.size() == 1 && card.fTransfers[0].blockCount == 1);

	// a 4 byte register read, as the Wi-Fi drivers do
	uint32 word = 0;
	card.fTransfers.clear();
	CHECK(sdio_rw_extended(ops, false, 1, 0x8000, true, (uint8*)&word, 4,
		512, true, 512) == B_OK);
	CHECK(card.fTransfers.size() == 1 && card.fTransfers[0].blockSize == 4
		&& card.fTransfers[0].blockCount == 0);

	// the card refuses a block size that is not its FBR's
	CHECK(sdio_rw_extended(ops, false, 1, 0, true, buffer.data(), 1024, 256,
		true, 512) == B_IO_ERROR);
	// past the 17 bit address space
	CHECK(sdio_rw_extended(ops, false, 1, 0x1ff00, true, buffer.data(), 512,
		512, true, 512) == B_BAD_VALUE);
	printf("CMD53: done\n");
}


static void
test_interrupts()
{
	// one handler: the pending register is not read, it is called
	CHECK(sdio_interrupts_to_serve(1 << 1, 0, false) == (1 << 1));
	// several: those that are pending
	CHECK(sdio_interrupts_to_serve(1 << 1 | 1 << 2, 1 << 2, true) == 1 << 2);
	CHECK(sdio_interrupts_to_serve(1 << 1 | 1 << 2, 1 << 3 | 1, true) == 0);

	// IENx: the master bit with the first function, gone with the last
	uint8 enable = sdio_interrupt_enable(0, 1, true);
	CHECK(enable == 0x03);
	enable = sdio_interrupt_enable(enable, 2, true);
	CHECK(enable == 0x07);
	enable = sdio_interrupt_enable(enable, 1, false);
	CHECK(enable == 0x05);
	enable = sdio_interrupt_enable(enable, 2, false);
	CHECK(enable == 0x00);
	CHECK(sdio_interrupt_enable(0, 0, true) == 0);

	CHECK(sdio_tran_speed(0x32) == 25000);
	CHECK(sdio_tran_speed(0x5a) == 50000);
	CHECK(sdio_tran_speed(0x48) == 400 * 10 / 10 * 1);
		// 4.0 * 100 kbit/s
	CHECK(sdio_tran_speed(0x07) == 0);
	printf("interrupts and speeds: done\n");
}


int
main()
{
	test_identification();
	test_card_reading();
	test_direct();
	test_extended();
	test_interrupts();
	if (sFailures != 0) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all SDIO checks passed\n");
	return 0;
}
