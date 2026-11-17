/*
 * Copyright 2018 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		B Krishnan Iyer, krishnaniyer97@gmail.com
 */
#ifndef MMC_BUS_H
#define MMC_BUS_H


#include <new>
#include <stdio.h>
#include <string.h>

#include <condition_variable.h>
#include <lock.h>
#include <util/AutoLock.h>
#include "mmc.h"
#include "sdio.h"
#include "sdio_card.h"


#define MMCBUS_TRACE
#ifdef MMCBUS_TRACE
#	define TRACE(x...)		dprintf("\33[33mmmc_bus:\33[0m " x)
#else
#	define TRACE(x...)
#endif
#define TRACE_ALWAYS(x...)	dprintf("\33[33mmmc_bus:\33[0m " x)
#define ERROR(x...)			dprintf("\33[33mmmc_bus:\33[0m " x)
#define CALLED() 			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


class CidWrapper {
	public:
		CidWrapper(const uint32 cid[4])
		{
			fWords[0] = cid[0];
			fWords[1] = cid[1];
			fWords[2] = cid[2];
			fWords[3] = cid[3];
		}

		uint32_t VendorID() const
		{
			return mmc_response_bits(fWords, 120, 8);
		}

		uint32_t ProductSerial() const
		{
			return mmc_response_bits(fWords, 24, 32);
		}

		uint16_t ProductRevision() const
		{
			uint16 rev = mmc_response_bits(fWords, 60, 4);
			rev *= 100;
			rev += mmc_response_bits(fWords, 56, 4);
			return rev;
		}

	protected:
		uint32_t fWords[4];
};


// per Physical Layer Simplified Specification Version 9.10
// sec 5.2
class SDCid : public CidWrapper {
	public:
		SDCid(const uint32 cid[4])
			: 
			CidWrapper(cid)
		{
		}

		void ProductName(char out[6]) const
		{
			out[0] = (char)(fWords[2] >> 24);
			out[1] = (char)(fWords[2] >> 16);
			out[2] = (char)(fWords[2] >> 8);
			out[3] = (char)(fWords[2]);
			out[4] = (char)(fWords[1] >> 24);
			out[5] = '\0';
		}

		uint8_t ManufactureMonth() const { return mmc_response_bits(fWords, 8, 4); }
		uint16_t ManufactureYear() const { return 2000 + mmc_response_bits(fWords, 12, 8); }
};


// per JEDEC Standard No. 84-B51, sec 7.2
class MMCCid : public CidWrapper {
	public:
		MMCCid(const uint32 cid[4])
			:
			CidWrapper(cid)
		{
		}

		void ProductName(char out[7]) const
		{
			for (unsigned i = 0; i < 6; i++)
				out[i] = (char)mmc_response_bits(fWords, 96 - 8 * i, 8);
			out[6] = '\0';
		}

		uint32_t ProductSerial() const { return mmc_response_bits(fWords, 16, 32); }
		uint16_t ProductRevision() const
		{
			return 100 * mmc_response_bits(fWords, 52, 4)
				+ mmc_response_bits(fWords, 48, 4);
		}

		uint8_t ManufactureMonth() const
		{
			// detailed in sec 7.2.7 MDT[15:8]
			return mmc_response_bits(fWords, 12, 4);
		}

		uint16_t ManufactureYear(bool modern) const
		{
			// For eMMC 4.41 and later devices,
			// indicated by a value larger than 4 in EXT_CSD_REV[192]
			// year offset is 2013 instead of 1997.
			uint8_t yearOffset = mmc_response_bits(fWords, 8, 4);
			return  yearOffset + (modern? 2013 : 1997);
		}
};


extern device_manager_info *gDeviceManager;


class MMCBus;

class MMCBus {
public:

								MMCBus(device_node *node);
								~MMCBus();
				status_t		InitCheck();
				void			Rescan();

				status_t		ExecuteCommand(uint16_t rca, uint8_t command,
									uint32_t argument, uint32_t* response);
				status_t		DoIO(uint16_t rca, uint8_t command,
									IOOperation* operation,
									bool offsetAsSectors);

				status_t		SetClock(int frequency);
				status_t		ReadExtendedCsd(uint16_t rca, uint8_t data[512]);
				status_t		SwitchSdFunction(uint16_t rca, uint32_t argument,
									uint8_t data[64]);
				void			SetBusWidth(int width);
				void			SetCardType(card_type type);

				void			AcquireBus() { acquire_sem(fLockSemaphore); }
				void			ReleaseBus() { release_sem(fLockSemaphore); }

				// SDIO, for the function nodes (sdio_function_interface).
				// They take the bus lock themselves.
				status_t		SdioReadByte(uint8 function, uint32 address,
									uint8* _value);
				status_t		SdioWriteByte(uint8 function, uint32 address,
									uint8 value, uint8* _readBack);
				status_t		SdioTransfer(bool write, uint8 function,
									uint32 address, bool increment,
									uint8* buffer, size_t length);
				status_t		SdioSetBlockSize(uint8 function, uint16 size);
				status_t		SdioEnableFunction(uint8 function,
									bool enable);
				status_t		SdioSetInterruptHandler(uint8 function,
									sdio_interrupt_handler handler,
									void* data);
				status_t		SdioGetCardInfo(sdio_card_info* info);

private:
				status_t		_ActivateDevice(uint16_t rca);
				void			_AcquireScanSemaphore();
				void			_TerminateBus();
		static	status_t		_WorkerThread(void*);

				bool			_SdioCapable() const;
				status_t		_ProbeSdio();
				bool			_SdioAlive();
				void			_PublishSdioFunctions();
				sdio_host_ops	_SdioHostOps();
				status_t		_SdioDirect(bool write, uint8 function,
									uint32 address, uint8 value,
									uint8* _value, bool readAfterWrite);
		static	status_t		_SdioCommand(void* cookie, uint8 command,
									uint32 argument, uint32* response);
		static	status_t		_SdioHostTransfer(void* cookie,
									uint32 argument, bool write, void* buffer,
									uint16 blockSize, uint16 blockCount,
									uint32* response);
		static	void			_SdioSleep(void* cookie, bigtime_t duration);
		static	bigtime_t		_SdioTime(void* cookie);
		static	void			_SdioInterrupt(void* data);
		static	status_t		_SdioInterruptThread(void* data);
				void			_StopSdioInterrupts();

private:

		device_node* 			fNode;
		mmc_bus_interface* 		fController;
		void* 					fCookie;
		status_t				fStatus;
		thread_id				fWorkerThread;
		sem_id					fScanSemaphore;
		sem_id					fLockSemaphore;
		uint16					fActiveDevice;
		card_type				fCardType;

		// the SDIO card, if there is one (fSdioRca != 0)
		uint16					fSdioRca;
		sdio_card				fSdio;
		mutex					fSdioHandlerLock;
			// the handlers, IENx, and the host's card interrupt
		sdio_interrupt_handler	fSdioHandlers[SDIO_MAX_FUNCTIONS];
		void*					fSdioHandlerData[SDIO_MAX_FUNCTIONS];
		uint8					fSdioInterruptEnable;
		sem_id					fSdioInterruptSemaphore;
		thread_id				fSdioInterruptThread;
		bool					fSdioHostHandlerSet;
		bool					fSdioDispatching;
		ConditionVariable		fSdioDispatchDone;
		// FIXME card type shall be extended to map card type to RCA.
		// In theory RCA is 16 bit allowing for 2^16 devices on signle bus
		// Practically this is limited by bus max capacitance(ver low speed)
		// which narrows down to theoritical 40 cards, imperically 10 cards only.
		// numbers qouted from untrusted sources but seems more than reasonable.
		// Thus, an array of 40 card_types where RCA is the index shall work.
};


#endif /*MMC_BUS_H*/
