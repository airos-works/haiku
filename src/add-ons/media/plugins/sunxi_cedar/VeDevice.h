/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef VE_DEVICE_H
#define VE_DEVICE_H


#include <vector>

#include <OS.h>
#include <String.h>

#include <sunxi_ve.h>


/*!	Memory the engine reads or writes: an area of the driver's, cloned. */
struct VeBuffer {
	uint32		id;
	area_id		clone;
	uint8*		address;
	size_t		size;
	uint32		bus;		// the address the engine uses

	VeBuffer() : id(0xffffffff), clone(-1), address(NULL), size(0), bus(0) {}
	bool IsValid() const { return clone >= 0; }
};


/*!	/dev/misc/sunxi_ve: buffers, and slices as lists of register writes. */
class VeDevice {
public:
								VeDevice();
								~VeDevice();

			status_t			Open(BString* _error = NULL);
			void				Close();
			bool				IsOpen() const { return fDevice >= 0; }

			status_t			Allocate(VeBuffer& buffer, size_t size);
			void				Free(VeBuffer& buffer);
			//!	The CPU wrote \a size bytes at \a offset: to memory with them.
			void				SyncForDevice(const VeBuffer& buffer,
									size_t offset, size_t size);
			//!	The CPU is about to read: drop what the cache has.
			void				SyncForCpu(const VeBuffer& buffer,
									size_t offset, size_t size);

			// one slice
			void				Begin() { fOps.clear(); }
			void				Write(uint32 offset, uint32 value);
			void				PollClear(uint32 offset, uint32 mask);
			void				WriteBack(uint32 offset);
			status_t			Run(uint32 triggerRegister,
									uint32 triggerValue,
									uint32 statusRegister, uint32& status);
			//!	The slices that follow belong to the same picture: no other
			//	program's go in between until EndPicture().
			void				SetInPicture(bool inPicture)
									{ fInPicture = inPicture; }
			void				EndPicture();

			const sunxi_ve_info& Info() const { return fInfo; }
			bigtime_t			EngineTime() const { return fEngineTime; }
			uint32				Slices() const { return fSlices; }

private:
			int					fDevice;
			sunxi_ve_info		fInfo;
			std::vector<sunxi_ve_op> fOps;
			bigtime_t			fEngineTime;
			uint32				fSlices;
			bool				fInPicture;
};


#endif	// VE_DEVICE_H
