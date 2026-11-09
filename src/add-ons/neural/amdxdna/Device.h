/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef XDNA_DEVICE_H
#define XDNA_DEVICE_H


#include <Locker.h>
#include <SupportDefs.h>

#include "XdnaInterface.h"

class BMessage;


//! A buffer object of the driver.
struct Buffer {
	uint32	handle;
	uint32	type;
	size_t	size;
	//! the address the NPU has it at (for data: its physical address)
	uint64	address;
	//! where this team has it
	void*	data;
	bool	mapped;

	Buffer() : handle(0), type(0), size(0), address(0), data(NULL),
		mapped(false) {}
};


/*!	The NPU, as the back end's programs share it.

	The node is opened when the first program is made and closed when the
	last one goes: the driver powers the NPU up for the first client and
	down after the last, so a team without a program leaves it off. The
	one open file holds the team's device heap (64 MiB of the driver's
	buffer memory, which design configurations and instruction sequences
	are put in); every program has its hardware context on it.
*/
class Device {
public:
								Device();
								~Device();

	//! Whether the node is there. Opens nothing.
	static	bool				Exists();

	/*!	For a program: opens the node and makes the heap unless another
		program has. Only root can. */
			status_t			Acquire();
			void				Release();

			int					FD() const { return fFD; }
	//! core tile rows of a column: a context's tiles are columns times this
			uint32				CoreRows() const
									{ return fMetadata.core.row_count; }
			uint32				Columns() const { return fMetadata.cols; }

	/*!	What is known of the device: before it was ever opened only the
		path and the driver's name. */
			void				GetDetails(BMessage* details);

	/*!	A buffer; those of the heap (XDNA_BO_DEV) need no mapping of
		their own. The memory is zeroed. */
			status_t			CreateBuffer(uint32 type, size_t size,
									Buffer& buffer);
			void				DeleteBuffer(Buffer& buffer);

	/*!	Before the NPU reads what the CPU wrote to a buffer, and before
		the CPU reads what the NPU wrote. */
			void				Flush(const Buffer& buffer);

private:
			status_t			_Open();
			void				_Close();

			BLocker				fLock;
			int					fFD;
			int32				fUsers;
			Buffer				fHeap;

			// kept after the node is closed
			bool				fKnown;
			xdna_aie_metadata	fMetadata;
			xdna_firmware_version fFirmware;
			char				fDriver[64];
			int32				fDriverVersion[2];
};


#endif	// XDNA_DEVICE_H
