/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_SCM_H
#define QCOM_SCM_H


#include <OS.h>


/*	Calls into the Qualcomm secure world (TrustZone's SCM interface, as
	Linux' qcom_scm), and its peripheral authentication service (PAS):
	loading a signed firmware image ("MDT" ELF with a hash segment) into its
	reserved region and having the secure world authenticate and start it.
	Compiled into each driver that needs it (adreno, qcom_iris). */

#define QCOM_SCM_SVC_PIL			0x02
#define QCOM_SCM_SVC_MP				0x0c
#define QCOM_SCM_SVC_IO				0x05

#define QCOM_SCM_ARG_VAL			0
#define QCOM_SCM_ARG_RW				2
#define QCOM_SCM_ARGS(count)		((uint32)(count))
#define QCOM_SCM_ARG(index, type)	((uint32)(type) << (4 + 2 * (index)))


/*!	A standard (yielding) SMC64 call to the SIP service \a service. It is
	made on CPU 0: on the Dragon Q8B, PAS calls from other CPUs never return.
	Returns the secure world's result (0: done). */
int64		qcom_scm_call(uint32 service, uint32 command, uint32 argumentInfo,
				uint64 a0 = 0, uint64 a1 = 0, uint64 a2 = 0, uint64 a3 = 0,
				uint64* result = NULL);

/*!	Loads the MDT image \a image (\a size bytes) for peripheral \a pasId:
	its metadata to the secure world, its loadable segments into the
	reserved region at \a regionBase. Then qcom_pas_auth_and_reset() starts
	it. \a name is for the log. */
/*!	The same as a fast (atomic) call, as Linux' qcom_scm_call_atomic():
	the secure register accesses take it. */
int64		qcom_scm_call_atomic(uint32 service, uint32 command,
				uint32 argumentInfo, uint64 a0 = 0, uint64 a1 = 0,
				uint64* result = NULL);

/*!	Clears the download mode cookie in the TCSR (bits 5:4 at \a address,
	the device tree's "qcom,dload-mode"), as Linux' qcom_scm does at probe:
	a crash then resets the board as usual, not into the boot firmware's
	USB crash dump mode, which waits until the power goes. */
status_t	qcom_scm_clear_download_mode(phys_addr_t address);

status_t	qcom_pas_load(const char* name, uint32 pasId, const uint8* image,
				size_t size, phys_addr_t regionBase, size_t regionSize);
status_t	qcom_pas_auth_and_reset(const char* name, uint32 pasId);
status_t	qcom_pas_shutdown(uint32 pasId);


#endif	// QCOM_SCM_H
