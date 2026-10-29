/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef HARDWARE_CAPS_H
#define HARDWARE_CAPS_H


#include <Locker.h>
#include <String.h>

#include <vector>

#include "Formats.h"


class VideoEncoder;


enum gpu_vendor {
	kVendorAMD = 0,
	kVendorNVIDIA,
	kVendorIntel,
	kVendorOther,
};


struct GpuEncodeFormat {
	video_codec		codec;
	int				maxWidth;
	int				maxHeight;
	bool			bFrames;
};


/*!	A graphics card and what its video engines can do. When the driver is
	there, the encoder formats are the ones it reports; otherwise they come
	from the card's generation, so the window can say what a missing driver
	would give. */
struct GpuDevice {
	BString			id;				// stable key: "vaapi:/dev/dri/renderD128"
	BString			name;
	gpu_vendor		vendor;
	uint16			vendorID;
	uint16			deviceID;
	BString			generation;		// "Polaris", "Pascal"
	BString			engine;			// "VCE 3.4 and UVD 6.3", "NVENC 6th gen."
	encoder_backend	backend;
	BString			devicePath;
	bool			ready;			// can encode now
	BString			status;			// why not, when it cannot

	std::vector<GpuEncodeFormat> encode;
	std::vector<BString> decode;	// names, for information

	const GpuEncodeFormat* Format(video_codec codec) const;
	bool			CanEncode(video_codec codec, int width,
						int height) const;
};


class HardwareInventory {
public:
	static	HardwareInventory&	Default();

			void				Probe();
			bool				IsProbed() const { return fProbed; }

			int32				CountDevices() const;
			const GpuDevice&	DeviceAt(int32 index) const;

	// The GPU that will make `codec` at that size, NULL if it is the CPU's
	// job. A preferred device (by id) wins when it can.
			const GpuDevice*	EncoderFor(video_codec codec, int width,
									int height,
									const char* preferred = NULL) const;
	// Why no GPU can: names the card and what it lacks.
			BString				WhyNotHardware(video_codec codec, int width,
									int height) const;
	// Whether any GPU here has a video encoder at all.
			bool				HasHardwareEncoder() const;

	static	VideoEncoder*		CreateEncoder(const GpuDevice& device);

private:
								HardwareInventory();

			void				_ProbeVaapi();
			void				_ProbeIris();
			void				_ProbeCedar();
			void				_ProbePci();
			void				_AddTableDevice(uint16 vendor, uint16 device);

			BLocker				fLock;
			bool				fProbed;
			std::vector<GpuDevice> fDevices;
};


// What MediaConverter's own hardware encoders implement, whatever a GPU
// says it can do.
bool	vaapi_encoder_implements(video_codec codec);
bool	nvenc_encoder_implements(video_codec codec);
bool	iris_encoder_implements(video_codec codec);
bool	cedar_encoder_implements(video_codec codec);


#endif	// HARDWARE_CAPS_H
