/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef VAAPI_LIBRARY_H
#define VAAPI_LIBRARY_H


#include <String.h>

#include "va/va.h"
#include "va/va_drm.h"


/*!	libva, opened when first needed: on air/OS it is part of the AMD GPU
	user land, which is only linked into place where amdgpu_drm drives a
	GPU. */
struct VaapiLibrary {
	VADisplay	(*GetDisplayDRM)(int fd);
	VAStatus	(*Initialize)(VADisplay, int*, int*);
	VAStatus	(*Terminate)(VADisplay);
	const char*	(*QueryVendorString)(VADisplay);
	const char*	(*ErrorStr)(VAStatus);
	int			(*MaxNumProfiles)(VADisplay);
	int			(*MaxNumEntrypoints)(VADisplay);
	VAStatus	(*QueryConfigProfiles)(VADisplay, VAProfile*, int*);
	VAStatus	(*QueryConfigEntrypoints)(VADisplay, VAProfile,
					VAEntrypoint*, int*);
	VAStatus	(*GetConfigAttributes)(VADisplay, VAProfile, VAEntrypoint,
					VAConfigAttrib*, int);
	VAStatus	(*CreateConfig)(VADisplay, VAProfile, VAEntrypoint,
					VAConfigAttrib*, int, VAConfigID*);
	VAStatus	(*DestroyConfig)(VADisplay, VAConfigID);
	VAStatus	(*CreateSurfaces)(VADisplay, unsigned int, unsigned int,
					unsigned int, VASurfaceID*, unsigned int,
					VASurfaceAttrib*, unsigned int);
	VAStatus	(*DestroySurfaces)(VADisplay, VASurfaceID*, int);
	VAStatus	(*CreateContext)(VADisplay, VAConfigID, int, int, int,
					VASurfaceID*, int, VAContextID*);
	VAStatus	(*DestroyContext)(VADisplay, VAContextID);
	VAStatus	(*CreateBuffer)(VADisplay, VAContextID, VABufferType,
					unsigned int, unsigned int, void*, VABufferID*);
	VAStatus	(*DestroyBuffer)(VADisplay, VABufferID);
	VAStatus	(*MapBuffer)(VADisplay, VABufferID, void**);
	VAStatus	(*UnmapBuffer)(VADisplay, VABufferID);
	VAStatus	(*BeginPicture)(VADisplay, VAContextID, VASurfaceID);
	VAStatus	(*RenderPicture)(VADisplay, VAContextID, VABufferID*, int);
	VAStatus	(*EndPicture)(VADisplay, VAContextID);
	VAStatus	(*SyncSurface)(VADisplay, VASurfaceID);
	VAStatus	(*DeriveImage)(VADisplay, VASurfaceID, VAImage*);
	VAStatus	(*CreateImage)(VADisplay, VAImageFormat*, int, int,
					VAImage*);
	VAStatus	(*DestroyImage)(VADisplay, VAImageID);
	VAStatus	(*PutImage)(VADisplay, VASurfaceID, VAImageID, int, int,
					unsigned int, unsigned int, int, int, unsigned int,
					unsigned int);
	VAStatus	(*QuerySurfaceAttributes)(VADisplay, VAConfigID,
					VASurfaceAttrib*, unsigned int*);
	VAMessageCallback (*SetErrorCallback)(VADisplay, VAMessageCallback,
					void*);
	VAMessageCallback (*SetInfoCallback)(VADisplay, VAMessageCallback,
					void*);

	static	const VaapiLibrary*	Get();
};


/*!	An opened VA display on one DRM render node. */
class VaapiDisplay {
public:
								VaapiDisplay();
								~VaapiDisplay();

			status_t			Open(const char* renderNode, BString& error);
			void				Close();

			VADisplay			Display() const { return fDisplay; }
			const VaapiLibrary*	Library() const { return fLibrary; }
			BString				Vendor() const;

private:
			const VaapiLibrary*	fLibrary;
			int					fFD;
			VADisplay			fDisplay;
};


// The render nodes in /dev/dri, renderD128 first.
int		vaapi_render_nodes(BString* nodes, int maxCount);
// "AMD Radeon Pro WX 5100" from Mesa's "Mesa Gallium driver 25.3.6 for
// AMD Radeon Pro WX 5100 Graphics (radeonsi, polaris10, ...)"
BString	vaapi_device_name(const char* vendorString);


#endif	// VAAPI_LIBRARY_H
