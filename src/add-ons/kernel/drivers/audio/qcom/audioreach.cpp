/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "audioreach.h"

#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <driver_settings.h>

#include "dsp.h"


#define TRACE(x...) dprintf("qcom_audio: " x)


namespace qcom_audio {


// APM commands and parameters (Linux' audioreach.h)
static const uint32 kGraphOpen = 0x01001000;
static const uint32 kGraphPrepare = 0x01001001;
static const uint32 kGraphStart = 0x01001002;
static const uint32 kGraphStop = 0x01001003;
static const uint32 kGraphClose = 0x01001004;
static const uint32 kSetConfig = 0x01001006;
static const uint32 kMapMemory = 0x0100100c;
static const uint32 kUnmapMemory = 0x0100100d;
static const uint32 kMapMemoryResponse = 0x02001001;
static const uint32 kWriteBuffer = 0x0400100a;	// DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2

static const uint32 kParamSubGraphConfig = 0x08001001;
static const uint32 kParamContainerConfig = 0x08001000;
static const uint32 kParamModuleList = 0x08001002;
static const uint32 kParamModuleProperties = 0x08001003;
static const uint32 kParamModuleConnections = 0x08001004;
static const uint32 kParamSubGraphList = 0x08001005;
static const uint32 kPropPerfMode = 0x0800100e;
static const uint32 kPropDirection = 0x0800100f;
static const uint32 kPropScenario = 0x08001010;
static const uint32 kPropCapabilities = 0x08001011;
static const uint32 kPropGraphPosition = 0x08001012;
static const uint32 kPropStackSize = 0x08001013;
static const uint32 kPropProcDomain = 0x08001014;
static const uint32 kPropPortInfo = 0x08001015;

static const uint32 kParamPcmOutputFormat = 0x08001008;
static const uint32 kParamMediaFormat = 0x0800100c;
static const uint32 kParamHwEndpointFormat = 0x08001017;
static const uint32 kParamFrameSizeFactor = 0x08001018;
static const uint32 kParamMfcOutputFormat = 0x08001024;
static const uint32 kParamModuleEnable = 0x08001026;
static const uint32 kParamLoggingConfig = 0x08001031;
static const uint32 kParamVolumeGain = 0x08001035;
static const uint32 kParamCodecDmaInterface = 0x08001063;
static const uint32 kParamPowerMode = 0x08001176;

static const uint32 kMediaFormatPcm = 0x09001000;
static const uint32 kFixedPoint = 1;
static const uint32 kSharedMemoryPool = 3;	// APM_MEMORY_MAP_SHMEM8_4K_POOL

enum {
	kWriteEndpoint = 0x07001000,
	kPcmConverter = 0x07001003,
	kPcmDecoder = 0x07001005,
	kMfc = 0x07001015,
	kLogger = 0x0700101a,
	kVolume = 0x0700101b,
	kCodecDmaSink = 0x07001023,
};


struct Module {
	uint32	id;
	uint32	instance;
	uint32	inputs;
	uint32	outputs;
	uint32	next;			// the instance output port 1 feeds (input port 2)
	uint32	interleave;		// the PCM modules' layout
	uint32	logCode;
	uint32	hwType;			// the codec DMA's LPAIF type, index, data format
	uint32	hwIndex;
	uint32	dataFormat;
};

struct SubGraph {
	uint32			id;
	uint32			perfMode;
	uint32			direction;
	uint32			scenario;
	uint32			container;
	uint32			capability;
	uint32			stackSize;
	uint32			graphPosition;
	uint32			procDomain;
	const Module*	modules;
	uint32			moduleCount;
};


/*	The board's topology (linux-firmware's SC8280XP-Radxa-Dragon-Q8B-tplg.bin,
	as its AR_TKN_* tokens say): MultiMedia1's stream (stream0) into the
	routing mixer of RX_CODEC_DMA_RX_0 (device113), which feeds the RX
	macro's codec DMA interface RX0 and through it the WCD9385. */
static const Module kStreamModules[] = {
	{ kWriteEndpoint, 0x6001, 0, 1, 0x6002, 0, 0, 0, 0, 0 },
	{ kPcmDecoder, 0x6002, 1, 1, 0x6003, 1, 0, 0, 0, 0 },
	{ kPcmConverter, 0x6003, 1, 1, 0x6004, 3, 0, 0, 0, 0 },
	{ kVolume, 0x6004, 1, 1, 0x6005, 0, 0, 0, 0, 0 },
	{ kMfc, 0x6005, 1, 1, 0x6006, 0, 0, 0, 0, 0 },
	{ kLogger, 0x6006, 1, 1, 0, 0, 0x19af, 0, 0, 0 },
};
static const SubGraph kStream = { 0x4001, 2, 2, 1, 0x4001, 2, 0x2000, 1, 2,
	kStreamModules, B_COUNT_OF(kStreamModules) };

static const Module kDeviceModules[] = {
	{ kLogger, 0x6110, 1, 1, 0x6111, 0, 0x19ab, 0, 0, 0 },
	{ kMfc, 0x6111, 1, 1, 0x6112, 0, 0, 0, 0, 0 },
	{ kCodecDmaSink, 0x6112, 1, 0, 0, 0, 0, 1, 1, 1 },
};
static const SubGraph kDevice = { 0x4011, 2, 1, 1, 0x4011, 3, 0x2000, 1, 2,
	kDeviceModules, B_COUNT_OF(kDeviceModules) };

// the routing mixer's connection: the stream's logger into the device's
static const uint32 kStreamOutput = 0x6006, kDeviceInput = 0x6110;
static const uint32 kWriteEndpointInstance = 0x6001;
static const uint32 kVolumeInstance = 0x6004;


//!	A packet's payload, built in 32-bit words.
class Payload {
public:
	Payload() : fCount(0) {}

	void Add(uint32 word)
	{
		if (fCount < B_COUNT_OF(fWords))
			fWords[fCount] = word;
		fCount++;
	}
	void Add16(uint16 low, uint16 high) { Add(low | (uint32)high << 16); }
	void Align8() { if ((fCount & 1) != 0) Add(0); }
	uint32 Mark() const { return fCount; }
	//! The byte size from \a mark up to here, stored at \a mark's word.
	void SetSize(uint32 at, uint32 from) { fWords[at] = (fCount - from) * 4; }
	void Set(uint32 at, uint32 word) { fWords[at] = word; }
	bool Valid() const { return fCount <= B_COUNT_OF(fWords); }
	const void* Data() const { return fWords; }
	uint32 Size() const { return fCount * 4; }

	//! apm_module_param_data, its size to be set with EndParameter()
	uint32 BeginParameter(uint32 instance, uint32 parameter)
	{
		Add(instance);
		Add(parameter);
		uint32 sizeAt = Mark();
		Add(0);
		Add(0);		// error code
		return sizeAt;
	}
	void EndParameter(uint32 sizeAt) { SetSize(sizeAt, sizeAt + 2); }

private:
	uint32	fWords[256];
	uint32	fCount;
};


Playback::Playback(Dsp& dsp)
	:
	fDsp(dsp),
	fSampleRate(48000),
	fChannels(2),
	fBits(16),
	fSkipLoggers(false),
	fSkipVolume(false),
	fMemoryHandle(0),
	fFrontOpen(false),
	fBackOpen(false),
	fRunning(false)
{
}


Playback::~Playback()
{
	Close();
}


status_t
Playback::Open(uint32 sampleRate, uint32 channels, uint32 bitsPerSample)
{
	fSampleRate = sampleRate;
	fChannels = channels;
	fBits = bitsPerSample;
	// lab: leave the loggers, the volume as they are
	void* settings = load_driver_settings("qcom_audio");
	fSkipLoggers = get_driver_boolean_parameter(settings, "skip_loggers", false,
		true);
	fSkipVolume = get_driver_boolean_parameter(settings, "skip_volume", false,
		true);
	unload_driver_settings(settings);

	status_t status = _OpenGraph(kDevice, false);
	if (status != B_OK)
		return status;
	fBackOpen = true;
	status = _OpenGraph(kStream, true);
	if (status != B_OK) {
		Close();
		return status;
	}
	fFrontOpen = true;
	status = _SetFormats(kDevice);
	if (status == B_OK)
		status = _SetFormats(kStream);
	if (status == B_OK && !fSkipVolume)
		status = SetVolume(0x2000);		// VOL_CTRL_DEFAULT_GAIN: unity
	if (status != B_OK) {
		Close();
		return status;
	}
	TRACE("AudioReach playback graph open: %" B_PRIu32 " Hz, %" B_PRIu32
		" channels, %" B_PRIu32 " bit\n", fSampleRate, fChannels, fBits);
	return B_OK;
}


void
Playback::Close()
{
	Stop();
	if (fMemoryHandle != 0) {
		uint32 handle = fMemoryHandle;
		fDsp.DataCommand(kApmPort, kUnmapMemory, &handle, sizeof(handle));
		fMemoryHandle = 0;
	}
	if (fFrontOpen)
		_GraphCommand(kGraphClose, kStream);
	if (fBackOpen)
		_GraphCommand(kGraphClose, kDevice);
	fFrontOpen = fBackOpen = false;
}


status_t
Playback::MapMemory(phys_addr_t address, size_t size)
{
	// apm_cmd_shared_mem_map_regions, one region (no APM command header);
	// the DSP wants whole 4 KiB pages
	uint32 payload[5] = { kSharedMemoryPool | 1u << 16, 0,
		(uint32)address, (uint32)((uint64)address >> 32),
		(uint32)((size + 4095) & ~(size_t)4095) };
	uint32 handle = 0;
	uint32 handleSize = sizeof(handle);
	// from the port the buffers go from: the DSP keeps the handle for it
	// (as Linux sends both from the graph's port)
	status_t status = fDsp.DataCommand(kApmPort, kMapMemory, payload,
		sizeof(payload), kMapMemoryResponse, &handle, &handleSize);
	if (status == B_OK && (handleSize < 4 || handle == 0))
		status = B_ERROR;
	if (status != B_OK)
		return status;
	fMemoryHandle = handle;
	TRACE("shared memory %#" B_PRIxPHYSADDR " (%" B_PRIuSIZE " bytes) is DSP "
		"handle %#" B_PRIx32 "\n", address, size, handle);
	return B_OK;
}


status_t
Playback::Start(bool startDevice)
{
	if (fRunning)
		return B_OK;
	status_t status = _GraphCommand(kGraphPrepare, kDevice);
	if (status == B_OK)
		status = _GraphCommand(kGraphPrepare, kStream);
	if (status == B_OK && startDevice)
		status = _GraphCommand(kGraphStart, kDevice);
	if (status == B_OK)
		status = _GraphCommand(kGraphStart, kStream);
	fRunning = status == B_OK;
	return status;
}


status_t
Playback::Stop()
{
	if (!fRunning)
		return B_OK;
	fRunning = false;
	status_t status = _GraphCommand(kGraphStop, kStream);
	_GraphCommand(kGraphStop, kDevice);
	return status;
}


status_t
Playback::SetVolume(uint16 gain)
{
	Payload payload;
	uint32 sizeAt = payload.BeginParameter(kVolumeInstance, kParamVolumeGain);
	payload.Add16(gain, 0);
	payload.EndParameter(sizeAt);
	return _SetConfig(payload.Data(), payload.Size());
}


status_t
Playback::Write(phys_addr_t address, uint32 size, uint32 token)
{
	// apm_data_cmd_wr_sh_mem_ep_data_buffer_v2: no time stamp (flags
	// 0xff00, as Linux sends them), no metadata
	uint32 payload[11] = { (uint32)address, (uint32)((uint64)address >> 32),
		fMemoryHandle, size, 0, 0, 0xff00, 0, 0, 0, 0 };
	return fDsp.SendData(kWriteEndpointInstance, kWriteBuffer, token,
		payload, sizeof(payload));
}


/*!	APM_CMD_GRAPH_OPEN for one sub-graph with its one container, as
	audioreach_alloc_graph_pkt() builds it: the sub-graph's and the
	container's properties, the module list, each module's ports, the
	connections (and, for the stream, the one into the device's sub-graph).
*/
status_t
Playback::_OpenGraph(const SubGraph& subGraph, bool withConnection)
{
	Payload payload;

	uint32 sizeAt = payload.BeginParameter(kApmPort, kParamSubGraphConfig);
	payload.Add(1);
	payload.Add(subGraph.id);
	payload.Add(3);
	payload.Add(kPropPerfMode);
	payload.Add(4);
	payload.Add(subGraph.perfMode);
	payload.Add(kPropDirection);
	payload.Add(4);
	payload.Add(subGraph.direction);
	payload.Add(kPropScenario);
	payload.Add(4);
	payload.Add(subGraph.scenario);
	payload.Align8();
	payload.EndParameter(sizeAt);

	sizeAt = payload.BeginParameter(kApmPort, kParamContainerConfig);
	payload.Add(1);
	payload.Add(subGraph.container);
	payload.Add(4);
	payload.Add(kPropCapabilities);
	payload.Add(8);
	payload.Add(1);
	payload.Add(subGraph.capability);
	payload.Add(kPropGraphPosition);
	payload.Add(4);
	payload.Add(subGraph.graphPosition);
	payload.Add(kPropStackSize);
	payload.Add(4);
	payload.Add(subGraph.stackSize);
	payload.Add(kPropProcDomain);
	payload.Add(4);
	payload.Add(subGraph.procDomain);
	payload.Align8();
	payload.EndParameter(sizeAt);

	sizeAt = payload.BeginParameter(kApmPort, kParamModuleList);
	payload.Add(1);
	payload.Add(subGraph.id);
	payload.Add(subGraph.container);
	payload.Add(subGraph.moduleCount);
	for (uint32 i = 0; i < subGraph.moduleCount; i++) {
		payload.Add(subGraph.modules[i].id);
		payload.Add(subGraph.modules[i].instance);
	}
	payload.Align8();
	payload.EndParameter(sizeAt);

	sizeAt = payload.BeginParameter(kApmPort, kParamModuleProperties);
	payload.Add(subGraph.moduleCount);
	for (uint32 i = 0; i < subGraph.moduleCount; i++) {
		payload.Add(subGraph.modules[i].instance);
		payload.Add(1);
		payload.Add(kPropPortInfo);
		payload.Add(8);
		payload.Add(subGraph.modules[i].inputs);
		payload.Add(subGraph.modules[i].outputs);
	}
	payload.Align8();
	payload.EndParameter(sizeAt);

	sizeAt = payload.BeginParameter(kApmPort, kParamModuleConnections);
	uint32 countAt = payload.Mark();
	payload.Add(0);
	uint32 connections = 0;
	for (uint32 i = 0; i < subGraph.moduleCount; i++) {
		uint32 next = subGraph.modules[i].next;
		if (subGraph.modules[i].instance == kStreamOutput && withConnection)
			next = kDeviceInput;
		if (next == 0)
			continue;
		// apm_module_conn_obj: source and its output port, destination
		// and its input port
		payload.Add(subGraph.modules[i].instance);
		payload.Add(1);
		payload.Add(next);
		payload.Add(2);
		connections++;
	}
	payload.Set(countAt, connections);
	payload.Align8();
	payload.EndParameter(sizeAt);

	if (!payload.Valid())
		return B_BUFFER_OVERFLOW;
	status_t status = fDsp.ApmCommand(kApmPort, kGraphOpen, payload.Data(),
		payload.Size());
	TRACE("sub-graph %#" B_PRIx32 " open: %s\n", subGraph.id,
		strerror(status));
	return status;
}


status_t
Playback::_GraphCommand(uint32 opcode, const SubGraph& subGraph)
{
	Payload payload;
	uint32 sizeAt = payload.BeginParameter(kApmPort, kParamSubGraphList);
	payload.Add(1);
	payload.Add(subGraph.id);
	payload.Align8();
	payload.EndParameter(sizeAt);
	status_t status = fDsp.ApmCommand(kApmPort, opcode, payload.Data(),
		payload.Size());
	if (status != B_OK) {
		TRACE("sub-graph %#" B_PRIx32 ": command %#" B_PRIx32 " failed: %s\n",
			subGraph.id, opcode, strerror(status));
	}
	return status;
}


//!	Each module's media format, as audioreach_set_media_format() sets it.
status_t
Playback::_SetFormats(const SubGraph& subGraph)
{
	for (uint32 i = 0; i < subGraph.moduleCount; i++) {
		const Module& module = subGraph.modules[i];
		Payload payload;
		uint32 sizeAt;
		uint32 destination = kApmPort;
		switch (module.id) {
			case kWriteEndpoint:
			{
				// PARAM_ID_MEDIA_FORMAT: media_format, payload_media_fmt_pcm
				// with the channel map (to the module itself)
				destination = module.instance;
				sizeAt = payload.BeginParameter(module.instance,
					kParamMediaFormat);
				payload.Add(kFixedPoint);
				payload.Add(kMediaFormatPcm);
				uint32 innerAt = payload.Mark();
				payload.Add(0);
				payload.Add(fSampleRate);
				payload.Add16(fBits, 1);			// LSB aligned
				payload.Add16(fBits, fBits - 1);	// bits per sample, Q
				payload.Add16(1, fChannels);		// little endian
				payload.Add(1 | 2 << 8);			// FL, FR
				payload.Align8();
				payload.SetSize(innerAt, innerAt + 1);
				payload.EndParameter(sizeAt);
				break;
			}
			case kPcmDecoder:
			case kPcmConverter:
			{
				// PARAM_ID_PCM_OUTPUT_FORMAT_CFG
				sizeAt = payload.BeginParameter(module.instance,
					kParamPcmOutputFormat);
				payload.Add(kFixedPoint);
				payload.Add(kMediaFormatPcm);
				uint32 innerAt = payload.Mark();
				payload.Add(0);
				uint32 from = payload.Mark();
				payload.Add16(fBits, 1);
				payload.Add16(fBits, fBits - 1);
				payload.Add16(1, module.interleave);
				payload.Add16(0, fChannels);
				payload.Add(1 | 2 << 8);
				payload.SetSize(innerAt, from);
				payload.Align8();
				payload.EndParameter(sizeAt);
				break;
			}
			case kMfc:
				// PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT, 16-bit channel map
				sizeAt = payload.BeginParameter(module.instance,
					kParamMfcOutputFormat);
				payload.Add(fSampleRate);
				payload.Add16(fBits, fChannels);
				payload.Add16(1, 2);
				payload.EndParameter(sizeAt);
				break;
			case kLogger:
			{
				if (fSkipLoggers)
					continue;
				Payload enable;
				uint32 at = enable.BeginParameter(module.instance,
					kParamModuleEnable);
				enable.Add(1);
				enable.EndParameter(at);
				status_t status = _SetConfig(enable.Data(), enable.Size());
				if (status != B_OK)
					return status;
				sizeAt = payload.BeginParameter(module.instance,
					kParamLoggingConfig);
				payload.Add(module.logCode);
				payload.Add(1);			// the tap point
				payload.Add(0);			// mode
				payload.EndParameter(sizeAt);
				break;
			}
			case kCodecDmaSink:
				// the format, the frame size factor, the interface, the
				// power mode, each a parameter of its own
				sizeAt = payload.BeginParameter(module.instance,
					kParamHwEndpointFormat);
				payload.Add(fSampleRate);
				payload.Add16(fBits, fChannels);
				payload.Add(module.dataFormat);
				payload.Align8();
				payload.EndParameter(sizeAt);
				sizeAt = payload.BeginParameter(module.instance,
					kParamFrameSizeFactor);
				payload.Add(1);
				payload.Align8();
				payload.EndParameter(sizeAt);
				sizeAt = payload.BeginParameter(module.instance,
					kParamCodecDmaInterface);
				payload.Add(module.hwType);
				payload.Add(module.hwIndex);
				payload.Add((1u << fChannels) - 1);
				payload.Align8();
				payload.EndParameter(sizeAt);
				sizeAt = payload.BeginParameter(module.instance,
					kParamPowerMode);
				payload.Add(0);
				payload.Align8();
				payload.EndParameter(sizeAt);
				break;
			default:
				continue;
		}
		if (!payload.Valid())
			return B_BUFFER_OVERFLOW;
		status_t status = fDsp.ApmCommand(destination, kSetConfig,
			payload.Data(), payload.Size(), 0, NULL, NULL,
			module.id == kWriteEndpoint);
		if (status != B_OK) {
			TRACE("module %#" B_PRIx32 ": media format: %s\n", module.instance,
				strerror(status));
			return status;
		}
	}
	return B_OK;
}


status_t
Playback::_SetConfig(const void* payload, uint32 size)
{
	return fDsp.ApmCommand(kApmPort, kSetConfig, payload, size);
}


}	// namespace qcom_audio
