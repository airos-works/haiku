/* Stand-in for Linux's uapi <drm/drm.h>, which pvr_drm.h includes: its
   types and the DRM_IOW()/DRM_IOWR() request encoding come from
   linux_compat.h, so the DRM_IOCTL_PVR_* codes are Linux's (pvr_drv.c's
   ioctl table is built from them). Userland uses pvr_haiku.h's op codes. */
#include <linux_compat.h>

#define DRM_COMMAND_BASE	0x40
