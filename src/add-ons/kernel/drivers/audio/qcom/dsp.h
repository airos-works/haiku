/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_AUDIO_DSP_H
#define QCOM_AUDIO_DSP_H


#include <OS.h>
#include <lock.h>

#include <qcom_gpr.h>


namespace qcom_audio {


// GPR's basic result: the command's opcode, its status
static const uint32 kGprBasicResult = 0x02001005;
// the DSP's services
static const uint32 kApmPort = 1;		// APM_MODULE_INSTANCE_ID
static const uint32 kPrmPort = 2;		// GPR_PRM_MODULE_IID


/*!	The audio DSP's services through qcom_adsp's GPR (qcom_gpr.h): one
	command at a time from our command port, waiting for its result, as
	audioreach_send_cmd_sync() does; and a data port for a graph's buffer
	events. */
class Dsp {
public:
	typedef void (*DataHook)(void* cookie, const qcom_gpr_packet* packet);

								Dsp();
								~Dsp();

			status_t			Init(DataHook dataHook, void* cookie);
			void				Uninit();

			status_t			WaitReady(bigtime_t timeout);

			//! The status the DSP answered with, or an error of ours; with
			//! \a responseOpcode, that answer's payload (\a response).
			status_t			Command(uint32 destination, uint32 opcode,
									const void* payload, uint32 size,
									uint32 responseOpcode = 0,
									void* response = NULL,
									uint32* _responseSize = NULL,
									bigtime_t timeout = 1000000);
			//! As Command(), with the APM command header (no shared memory)
			//! in front of the payload.
			status_t			ApmCommand(uint32 destination, uint32 opcode,
									const void* payload, uint32 size,
									uint32 responseOpcode = 0,
									void* response = NULL,
									uint32* _responseSize = NULL);
			status_t			SendData(uint32 destination, uint32 opcode,
									uint32 token, const void* payload,
									uint32 size);

			// PRM (q6prm.c): hardware votes and LPASS clocks
			status_t			Vote(uint32 resource, bool on);
			status_t			SetClock(uint32 clock, uint32 rate);

			uint32				DataPort() const { return kDataPort; }

private:
	static	void				_CommandHook(void* cookie,
									const qcom_gpr_packet* packet);
			status_t			_Send(uint32 sourcePort, uint32 destination,
									uint32 opcode, uint32 token,
									const void* payload, uint32 size);

	static const uint32			kCommandPort = 0x10000001;
	static const uint32			kDataPort = 0x10000002;

			qcom_gpr_module_info* fGpr;
			mutex				fLock;
			sem_id				fDone;
			spinlock			fStateLock;
			uint32				fToken;
			uint32				fOpcode;
			uint32				fResponseOpcode;
			bool				fPending;
			status_t			fStatus;
			uint8				fResponse[256];
			uint32				fResponseSize;
			bool				fPortsRegistered;
};


}	// namespace qcom_audio


#endif	// QCOM_AUDIO_DSP_H
