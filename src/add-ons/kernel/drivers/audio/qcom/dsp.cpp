/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "dsp.h"

#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <util/AutoLock.h>


#define TRACE(x...) dprintf("qcom_audio: " x)


namespace qcom_audio {


// PRM_CMD_REQUEST_HW_RSC and _RELEASE_HW_RSC; their answer
static const uint32 kPrmRequest = 0x0100100f;
static const uint32 kPrmRelease = 0x01001010;
static const uint32 kPrmResponse = 0x02001002;
static const uint32 kPrmReleaseResponse = 0x02001003;
static const uint32 kPrmHwCore = 0x08001032;		// PARAM_ID_RSC_HW_CORE
static const uint32 kPrmAudioClock = 0x0800102c;	// PARAM_ID_RSC_AUDIO_HW_CLK


Dsp::Dsp()
	:
	fGpr(NULL),
	fDataHook(NULL),
	fDataCookie(NULL),
	fDone(-1),
	fStateLock(B_SPINLOCK_INITIALIZER),
	fToken(0),
	fOpcode(0),
	fResponseOpcode(0),
	fPending(false),
	fStatus(B_OK),
	fResponseSize(0),
	fPortsRegistered(false)
{
	mutex_init(&fLock, "qcom audio dsp");
}


Dsp::~Dsp()
{
	Uninit();
	mutex_destroy(&fLock);
}


status_t
Dsp::Init(DataHook dataHook, void* cookie)
{
	status_t status = get_module(QCOM_GPR_MODULE_NAME, (module_info**)&fGpr);
	if (status != B_OK) {
		fGpr = NULL;
		return status;
	}
	fDone = create_sem(0, "qcom audio dsp command");
	if (fDone < 0)
		return fDone;
	status = fGpr->register_port(kCommandPort, _CommandHook, this);
	if (status == B_OK) {
		fDataHook = dataHook;
		fDataCookie = cookie;
		status = fGpr->register_port(kDataPort, _DataHook, this);
		if (status != B_OK)
			fGpr->unregister_port(kCommandPort);
	}
	fPortsRegistered = status == B_OK;
	return status;
}


void
Dsp::Uninit()
{
	if (fGpr != NULL) {
		if (fPortsRegistered) {
			fGpr->unregister_port(kDataPort);
			fGpr->unregister_port(kCommandPort);
			fPortsRegistered = false;
		}
		put_module(QCOM_GPR_MODULE_NAME);
		fGpr = NULL;
	}
	if (fDone >= 0) {
		delete_sem(fDone);
		fDone = -1;
	}
}


uint32
Dsp::State(char* reason, size_t reasonSize)
{
	return fGpr != NULL ? fGpr->dsp_state(reason, reasonSize) : 0;
}


status_t
Dsp::WaitReady(bigtime_t timeout)
{
	if (fGpr == NULL)
		return B_NO_INIT;
	for (bigtime_t end = system_time() + timeout; !fGpr->ready();
			snooze(50000)) {
		if (system_time() > end)
			return B_TIMED_OUT;
	}
	return B_OK;
}


status_t
Dsp::Command(uint32 destination, uint32 opcode, const void* payload,
	uint32 size, uint32 responseOpcode, void* response,
	uint32* _responseSize, bigtime_t timeout)
{
	return _Command(kCommandPort, destination, opcode, payload, size,
		responseOpcode, response, _responseSize, timeout);
}


status_t
Dsp::DataCommand(uint32 destination, uint32 opcode, const void* payload,
	uint32 size, uint32 responseOpcode, void* response,
	uint32* _responseSize, int32 token)
{
	return _Command(kDataPort, destination, opcode, payload, size,
		responseOpcode, response, _responseSize, 1000000, token);
}


status_t
Dsp::_Command(uint32 sourcePort, uint32 destination, uint32 opcode,
	const void* payload, uint32 size, uint32 responseOpcode, void* response,
	uint32* _responseSize, bigtime_t timeout, int32 token)
{
	if (fGpr == NULL)
		return B_NO_INIT;
	MutexLocker locker(fLock);

	cpu_status state = disable_interrupts();
	acquire_spinlock(&fStateLock);
	if (token < 0)
		token = ++fToken & 0x7fffffff;
	fToken = token;
	fOpcode = opcode;
	fResponseOpcode = responseOpcode;
	fPending = true;
	fStatus = B_TIMED_OUT;
	fResponseSize = 0;
	release_spinlock(&fStateLock);
	restore_interrupts(state);
	// a stale release of a command that timed out
	while (acquire_sem_etc(fDone, 1, B_RELATIVE_TIMEOUT, 0) == B_OK)
		;

	status_t status = _Send(sourcePort, destination, opcode, token, payload,
		size);
	if (status == B_OK)
		status = acquire_sem_etc(fDone, 1, B_RELATIVE_TIMEOUT, timeout);

	state = disable_interrupts();
	acquire_spinlock(&fStateLock);
	fPending = false;
	if (status == B_OK)
		status = fStatus;
	if (response != NULL && _responseSize != NULL) {
		uint32 length = min_c(*_responseSize, fResponseSize);
		memcpy(response, fResponse, length);
		*_responseSize = length;
	}
	release_spinlock(&fStateLock);
	restore_interrupts(state);
	if (status != B_OK) {
		TRACE("command %#" B_PRIx32 " to %#" B_PRIx32 ": %#" B_PRIx32 "\n",
			opcode, destination, status);
	}
	return status;
}


status_t
Dsp::ApmCommand(uint32 destination, uint32 opcode, const void* payload,
	uint32 size, uint32 responseOpcode, void* response,
	uint32* _responseSize, bool fromDataPort)
{
	uint8* packet = (uint8*)malloc(16 + size);
	if (packet == NULL)
		return B_NO_MEMORY;
	// apm_cmd_header: payload address (none), memory map handle, size
	uint32 header[4] = { 0, 0, 0, size };
	memcpy(packet, header, sizeof(header));
	memcpy(packet + 16, payload, size);
	status_t status = _Command(fromDataPort ? kDataPort : kCommandPort,
		destination, opcode, packet, 16 + size, responseOpcode, response,
		_responseSize, 1000000);
	free(packet);
	return status;
}


status_t
Dsp::SendData(uint32 destination, uint32 opcode, uint32 token,
	const void* payload, uint32 size)
{
	if (fGpr == NULL)
		return B_NO_INIT;
	return _Send(kDataPort, destination, opcode, token, payload, size);
}


status_t
Dsp::Vote(uint32 resource, bool on)
{
	// apm_module_param_data: instance, parameter, size, error; the resource
	uint32 payload[5] = { kPrmPort, kPrmHwCore, 4, 0, resource };
	uint32 response[4];
	uint32 responseSize = sizeof(response);
	status_t status = ApmCommand(kPrmPort, on ? kPrmRequest : kPrmRelease,
		payload, sizeof(payload), on ? kPrmResponse : kPrmReleaseResponse,
		response, &responseSize);
	// the answer: the parameter, its status
	if (status == B_OK && responseSize >= 8 && response[1] != 0)
		status = B_ERROR;
	return status;
}


status_t
Dsp::SetClock(uint32 clock, uint32 rate)
{
	// one clock: its ID, the rate, LPASS_CLK_ATTRIBUTE_COUPLE_NO, the
	// default root
	uint32 payload[9] = { kPrmPort, kPrmAudioClock, 20, 0, 1, clock, rate,
		1, 0 };
	uint32 response[4];
	uint32 responseSize = sizeof(response);
	status_t status = ApmCommand(kPrmPort,
		rate != 0 ? kPrmRequest : kPrmRelease, payload, sizeof(payload),
		rate != 0 ? kPrmResponse : kPrmReleaseResponse, response,
		&responseSize);
	if (status == B_OK && responseSize >= 8 && response[1] != 0)
		status = B_ERROR;
	return status;
}


//!	Whether \a packet answers the pending command (which it then completes).
bool
Dsp::_Complete(const qcom_gpr_packet* packet)
{
	const uint32* words = (const uint32*)packet->payload;
	uint32 count = packet->payload_size / 4;

	cpu_status state = disable_interrupts();
	acquire_spinlock(&fStateLock);
	bool done = false;
	if (fPending && packet->token == fToken) {
		if (packet->opcode == kGprBasicResult && count >= 2
			&& words[0] == fOpcode) {
			fStatus = words[1] == 0 ? B_OK : (status_t)words[1];
			done = true;
		} else if (fResponseOpcode != 0 && packet->opcode == fResponseOpcode) {
			fResponseSize = min_c(packet->payload_size, sizeof(fResponse));
			memcpy(fResponse, packet->payload, fResponseSize);
			fStatus = B_OK;
			done = true;
		}
		if (done)
			fPending = false;
	}
	release_spinlock(&fStateLock);
	restore_interrupts(state);

	if (done)
		release_sem_etc(fDone, 1, B_DO_NOT_RESCHEDULE);
	return done;
}


//!	The data port's packets: answers to commands sent from it, then data.
/*static*/ void
Dsp::_DataHook(void* cookie, const qcom_gpr_packet* packet)
{
	Dsp* dsp = (Dsp*)cookie;
	if (!dsp->_Complete(packet) && dsp->fDataHook != NULL)
		dsp->fDataHook(dsp->fDataCookie, packet);
}


/*static*/ void
Dsp::_CommandHook(void* cookie, const qcom_gpr_packet* packet)
{
	Dsp* dsp = (Dsp*)cookie;
	const uint32* words = (const uint32*)packet->payload;
	uint32 count = packet->payload_size / 4;

	if (!dsp->_Complete(packet)) {
		TRACE("unexpected answer %#" B_PRIx32 " (token %" B_PRIu32 ", %"
			B_PRIu32 " bytes, first %#" B_PRIx32 " %#" B_PRIx32 ")\n",
			packet->opcode, packet->token, packet->payload_size,
			count > 0 ? words[0] : 0, count > 1 ? words[1] : 0);
	}
}


status_t
Dsp::_Send(uint32 sourcePort, uint32 destination, uint32 opcode,
	uint32 token, const void* payload, uint32 size)
{
	status_t status;
	for (bigtime_t end = system_time() + 1000000;
			(status = fGpr->send(sourcePort, destination, opcode, token,
				payload, size)) == B_BUSY; snooze(1000)) {
		if (system_time() > end)
			return B_TIMED_OUT;
	}
	return status;
}


}	// namespace qcom_audio
