/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "VaapiLibrary.h"

#include <Autolock.h>
#include <Locker.h>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>


static BLocker sLock("libva");
static bool sTried;
static VaapiLibrary sLibrary;
static bool sLoaded;


template<typename T>
static bool
find_symbol(void* image, const char* name, T& function)
{
	function = (T)dlsym(image, name);
	return function != NULL;
}


static void
quiet(void*, const char*)
{
}


const VaapiLibrary*
VaapiLibrary::Get()
{
	BAutolock _(sLock);
	if (sTried)
		return sLoaded ? &sLibrary : NULL;
	sTried = true;

	void* va = dlopen("libva.so.2", RTLD_NOW | RTLD_GLOBAL);
	void* drm = va != NULL ? dlopen("libva-drm.so.2", RTLD_NOW) : NULL;
	if (va == NULL || drm == NULL) {
		if (va != NULL)
			dlclose(va);
		return NULL;
	}

	VaapiLibrary& l = sLibrary;
	sLoaded = find_symbol(drm, "vaGetDisplayDRM", l.GetDisplayDRM)
		&& find_symbol(va, "vaInitialize", l.Initialize)
		&& find_symbol(va, "vaTerminate", l.Terminate)
		&& find_symbol(va, "vaQueryVendorString", l.QueryVendorString)
		&& find_symbol(va, "vaErrorStr", l.ErrorStr)
		&& find_symbol(va, "vaMaxNumProfiles", l.MaxNumProfiles)
		&& find_symbol(va, "vaMaxNumEntrypoints", l.MaxNumEntrypoints)
		&& find_symbol(va, "vaQueryConfigProfiles", l.QueryConfigProfiles)
		&& find_symbol(va, "vaQueryConfigEntrypoints",
			l.QueryConfigEntrypoints)
		&& find_symbol(va, "vaGetConfigAttributes", l.GetConfigAttributes)
		&& find_symbol(va, "vaCreateConfig", l.CreateConfig)
		&& find_symbol(va, "vaDestroyConfig", l.DestroyConfig)
		&& find_symbol(va, "vaCreateSurfaces", l.CreateSurfaces)
		&& find_symbol(va, "vaDestroySurfaces", l.DestroySurfaces)
		&& find_symbol(va, "vaCreateContext", l.CreateContext)
		&& find_symbol(va, "vaDestroyContext", l.DestroyContext)
		&& find_symbol(va, "vaCreateBuffer", l.CreateBuffer)
		&& find_symbol(va, "vaDestroyBuffer", l.DestroyBuffer)
		&& find_symbol(va, "vaMapBuffer", l.MapBuffer)
		&& find_symbol(va, "vaUnmapBuffer", l.UnmapBuffer)
		&& find_symbol(va, "vaBeginPicture", l.BeginPicture)
		&& find_symbol(va, "vaRenderPicture", l.RenderPicture)
		&& find_symbol(va, "vaEndPicture", l.EndPicture)
		&& find_symbol(va, "vaSyncSurface", l.SyncSurface)
		&& find_symbol(va, "vaDeriveImage", l.DeriveImage)
		&& find_symbol(va, "vaCreateImage", l.CreateImage)
		&& find_symbol(va, "vaDestroyImage", l.DestroyImage)
		&& find_symbol(va, "vaPutImage", l.PutImage)
		&& find_symbol(va, "vaQuerySurfaceAttributes",
			l.QuerySurfaceAttributes)
		&& find_symbol(va, "vaSetErrorCallback", l.SetErrorCallback)
		&& find_symbol(va, "vaSetInfoCallback", l.SetInfoCallback);
	return sLoaded ? &sLibrary : NULL;
}


VaapiDisplay::VaapiDisplay()
	:
	fLibrary(NULL),
	fFD(-1),
	fDisplay(NULL)
{
}


VaapiDisplay::~VaapiDisplay()
{
	Close();
}


status_t
VaapiDisplay::Open(const char* renderNode, BString& error)
{
	fLibrary = VaapiLibrary::Get();
	if (fLibrary == NULL) {
		error = "VA-API (libva) is not installed.";
		return B_NOT_SUPPORTED;
	}
	fFD = open(renderNode, O_RDWR);
	if (fFD < 0) {
		error.SetToFormat("Cannot open %s.", renderNode);
		return B_ERROR;
	}
	fDisplay = fLibrary->GetDisplayDRM(fFD);
	if (fDisplay == NULL) {
		error = "libva has no display for the GPU.";
		Close();
		return B_ERROR;
	}
	fLibrary->SetInfoCallback(fDisplay, quiet, NULL);
	int major, minor;
	VAStatus status = fLibrary->Initialize(fDisplay, &major, &minor);
	if (status != VA_STATUS_SUCCESS) {
		error.SetToFormat("VA-API did not start: %s",
			fLibrary->ErrorStr(status));
		fLibrary->Terminate(fDisplay);
		fDisplay = NULL;
		Close();
		return B_ERROR;
	}
	return B_OK;
}


void
VaapiDisplay::Close()
{
	if (fDisplay != NULL) {
		fLibrary->Terminate(fDisplay);
		fDisplay = NULL;
	}
	if (fFD >= 0) {
		close(fFD);
		fFD = -1;
	}
}


BString
VaapiDisplay::Vendor() const
{
	if (fDisplay == NULL)
		return BString();
	const char* vendor = fLibrary->QueryVendorString(fDisplay);
	return BString(vendor != NULL ? vendor : "");
}


int
vaapi_render_nodes(BString* nodes, int maxCount)
{
	const char* forced = getenv("MEDIACONVERTER_VA_DEVICE");
	if (forced != NULL && forced[0] != '\0') {
		if (maxCount > 0)
			nodes[0] = forced;
		return maxCount > 0 ? 1 : 0;
	}

	std::vector<std::string> names;
	DIR* dir = opendir("/dev/dri");
	if (dir == NULL)
		return 0;
	while (dirent* entry = readdir(dir)) {
		if (strncmp(entry->d_name, "renderD", 7) == 0)
			names.push_back(entry->d_name);
	}
	closedir(dir);
	std::sort(names.begin(), names.end());

	int count = 0;
	for (size_t i = 0; i < names.size() && count < maxCount; i++) {
		nodes[count].SetToFormat("/dev/dri/%s", names[i].c_str());
		count++;
	}
	return count;
}


BString
vaapi_device_name(const char* vendorString)
{
	BString name(vendorString);
	int32 start = name.FindFirst(" for ");
	if (start >= 0)
		name.Remove(0, start + 5);
	int32 end = name.FindFirst(" (");
	if (end >= 0)
		name.Truncate(end);
	if (name.EndsWith(" Graphics"))
		name.Truncate(name.Length() - 9);
	name.Trim();
	return name;
}
