/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_SMMU_H
#define QCOM_SMMU_H


#include <OS.h>


/*!	An address space of its own on the SC8280XP's apps SMMU (MMU-500) for a
	device's streams, as qcom_iris has for the video codec: a free context
	bank, stage 1 with stage 2 bypassed, a 4 GiB table, the streams routed
	to it. Only what is mapped can be reached; a stray access faults in the
	context bank (its FSR and FAR tell), which the SoC survives, where a
	stream passed through or left untranslated that strays hangs it.
	Compiled into each driver that needs it.
*/
class QcomSmmuDomain {
public:
								QcomSmmuDomain();
								~QcomSmmuDomain();

			//! \a streams: stream ID, mask pairs.
			status_t			Attach(const char* name,
									const uint32 (*streams)[2],
									uint32 streamCount);
			void				Detach();

			//! \a cached: write-back for a coherent device, else
			//! non-cacheable.
			status_t			Map(uint64 address, phys_addr_t physical,
									size_t size, bool cached);
			void				Unmap(uint64 address, size_t size);

			//! Its fault status; with \a when, logged if there is a fault.
			uint32				Faults(const char* when = NULL);

private:
			uint64*				_Table(phys_addr_t& physical);
			uint64*				_Level3(uint64 address, bool create);
			void				_FlushTlb();
			uint32				_Read(uint32 offset) const;
			void				_Write(uint32 offset, uint32 value);

			const char*			fName;
			area_id				fRegisterArea;
			volatile uint8*		fRegisters;
			uint32				fPageSize;
			uint32				fContexts;		// offset of context bank 0
			int32				fBank;
			int32				fEntries[4];	// the stream match entries
			uint32				fEntryCount;
			uint64*				fLevel1;
			phys_addr_t			fLevel1Physical;
			uint64*				fLevel2[4];
			uint64*				fLevel3[2048];	// one for each 2 MiB
			area_id				fTableAreas[64];
			uint32				fTableAreaCount;
};


#endif	// QCOM_SMMU_H
