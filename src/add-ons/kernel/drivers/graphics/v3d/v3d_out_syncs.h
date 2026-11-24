/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef V3D_OUT_SYNCS_H
#define V3D_OUT_SYNCS_H


/*	The sync objects a job signals, as its submit request names them: the
	request's single out_sync, or the list of a multi-sync extension
	(Vulkan).

	Header only, so that a host test can run the same code on chains of
	extensions no Mesa sends (src/tests/add-ons/kernel/drivers/graphics/v3d).
	Whoever includes it has v3d_drm.h and a
		status_t v3d_copy_from_user(void* to, uint64 address, size_t size);
	that reads the submitting program's memory. */


#define MAX_OUT_SYNCS		16


/*!	Fills \a outSyncs, an array of MAX_OUT_SYNCS entries, with the handles of
	the sync objects the job signals, and returns their number in \a _count.
	The extension's in_syncs need no handling: jobs run in the order they
	were submitted.
*/
static status_t
read_out_syncs(uint32 flags, uint64 extensions, uint32 outSync,
	uint32* outSyncs, uint32& _count)
{
	_count = 0;
	if ((flags & DRM_V3D_SUBMIT_EXTENSION) == 0) {
		if (outSync != 0)
			outSyncs[_count++] = outSync;
		return B_OK;
	}

	// The chain ends at the second extension at the latest: only one kind
	// is known, and that one only once.
	bool multiSync = false;
	uint64 address = extensions;
	while (address != 0) {
		drm_v3d_extension header;
		if (v3d_copy_from_user(&header, address, sizeof(header)) != B_OK)
			return B_BAD_ADDRESS;

		if (header.id == DRM_V3D_EXT_ID_MULTI_SYNC) {
			// One per job, as with Linux. The array the entries go to has
			// room for one extension's: those of a second one (or of the
			// same one again, when a "next" points back) were written past
			// its end, on the kernel stack of the submitting thread (#193).
			if (multiSync)
				return B_BAD_VALUE;
			multiSync = true;

			drm_v3d_multi_sync sync;
			if (v3d_copy_from_user(&sync, address, sizeof(sync)) != B_OK)
				return B_BAD_ADDRESS;
			if (sync.out_sync_count > MAX_OUT_SYNCS - _count)
				return B_BAD_VALUE;
			for (uint32 k = 0; k < sync.out_sync_count; k++) {
				drm_v3d_sem semaphore;
				uint64 entry = sync.out_syncs + k * sizeof(drm_v3d_sem);
				if (v3d_copy_from_user(&semaphore, entry, sizeof(semaphore))
						!= B_OK) {
					return B_BAD_ADDRESS;
				}
				outSyncs[_count++] = semaphore.handle;
			}
		} else {
			// CPU jobs and their kin
			return B_NOT_SUPPORTED;
		}
		address = header.next;
	}
	return B_OK;
}


#endif	// V3D_OUT_SYNCS_H
