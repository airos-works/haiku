/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_RPMH_H
#define QCOM_RPMH_H


#include <OS.h>


/*	Votes on the RPMh resources of Qualcomm SoCs (regulators, power
	domain levels) from the applications processor: the AOP's command DB
	(Linux' cmd-db.c) names them, and the apps RSC (rpmh-rsc.c) sends them,
	one active-mode request at a time through a TCS. Both are found in the
	device tree (gFDT). Compiled into each driver that needs it; drivers
	that may vote at the same time use different active TCSes (the
	SC8280XP's apps RSC has two: qcom_adsp uses 0, sdhci 1). */

struct qcom_rpmh {
	area_id			cmdDbArea;
	const uint8*	cmdDb;
	area_id			rscArea;
	volatile uint8*	rsc;
	uint32			tcsOffset;		// of the TCSes in the DRV's window
};


status_t	qcom_rpmh_init(qcom_rpmh& rpmh);
void		qcom_rpmh_uninit(qcom_rpmh& rpmh);

/*!	The address of resource \a id ("ldoc9", "lcx.lvl"), 0 if it has none;
	the number of levels of an arc in \a _levels. */
uint32		qcom_cmd_db_address(const qcom_rpmh& rpmh, const char* id,
				uint32* _levels = NULL);

/*!	\a count writes of \a data to \a addresses through active TCS \a tcs,
	waited for. */
status_t	qcom_rpmh_write(qcom_rpmh& rpmh, uint32 tcs,
				const uint32* addresses, const uint32* data, int count);

/*!	A PMIC LDO on at \a millivolts in high power mode, or off
	(rpmh-regulator: voltage at +0, enable at +4, mode at +8). */
status_t	qcom_rpmh_ldo(qcom_rpmh& rpmh, uint32 tcs, const char* name,
				bool on, uint32 millivolts);


#endif	// QCOM_RPMH_H
