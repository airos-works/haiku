/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef PI_BOOT_H
#define PI_BOOT_H


#include <ObjectList.h>
#include <Path.h>
#include <String.h>
#include <StringList.h>

#include "Release.h"


class BPartition;
class BDiskDevice;


// The Raspberry Pi's FAT boot partition. Its firmware loads the loader and
// the boot archive (the kernel and the modules that mount the system volume),
// so each installed version keeps its own there, in airos/<version>/, and
// airos-boot.txt, which config.txt includes, names the one to start. The
// image's own pair stays at the top of the partition.
class PiBoot {
public:
								PiBoot();
								~PiBoot();

	static	bool				IsPi();

			status_t			Mount(BString& _error);
			void				Unmount();
			const char*			Path() const { return fMountPoint.Path(); }

			status_t			Install(const char* version,
									const BObjectList<BootFile, true>& files,
									const BStringList& sources,
									BString& _error);
			bool				HasVersion(const char* version) const;
			status_t			Select(const char* version, BString& _error);
									// NULL or "": the image's own pair
			BString				SelectedVersion() const;
			status_t			Prune(const BStringList& keep);

private:
			status_t			_EnsureInclude(BString& _error);

private:
			BDiskDevice*		fDevice;
			BPartition*			fPartition;
			BPath				fMountPoint;
			bool				fMounted;
};


#endif	// PI_BOOT_H
