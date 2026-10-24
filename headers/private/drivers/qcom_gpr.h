/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _QCOM_GPR_H
#define _QCOM_GPR_H


#include <module.h>


/*	GPR, the packet router to the SC8280XP's audio DSP (its AudioReach
	framework: the APM, the PRM and the graphs' modules), as the qcom_adsp
	driver runs it over GLINK: for the audio driver. Packets go from one of
	the caller's ports to a port (a module instance) on the DSP; the DSP's
	answers and events come back to the hook registered for the port they
	are addressed to. The hook runs on qcom_adsp's GLINK thread; it may send,
	but must not block. Ports below 0x100 are qcom_adsp's own. */

#define QCOM_GPR_MODULE_NAME "drivers/power/qcom_adsp/gpr/v1"

typedef struct qcom_gpr_packet {
	uint32		source_port;
	uint32		destination_port;
	uint32		token;
	uint32		opcode;
	const void*	payload;
	uint32		payload_size;
} qcom_gpr_packet;

typedef void (*qcom_gpr_hook)(void* cookie, const qcom_gpr_packet* packet);

struct qcom_gpr_module_info {
	module_info	info;
	//! whether the DSP's audio framework said it is ready
	bool		(*ready)(void);
	status_t	(*register_port)(uint32 port, qcom_gpr_hook hook,
					void* cookie);
	void		(*unregister_port)(uint32 port);
	//! B_BUSY while the DSP has no room for it (try again)
	status_t	(*send)(uint32 sourcePort, uint32 destinationPort,
					uint32 opcode, uint32 token, const void* payload,
					uint32 payloadSize);
};


#endif	// _QCOM_GPR_H
