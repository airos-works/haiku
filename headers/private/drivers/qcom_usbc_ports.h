/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _QCOM_USBC_PORTS_H
#define _QCOM_USBC_PORTS_H


#include <module.h>


/*	The USB-C ports' state as the SC8280XP's ADSP reports it over pmic_glink
	(the qcom_adsp driver, alternate mode reports): for the display driver,
	which brings DisplayPort up on a port once a sink is there. */

#define QCOM_USBC_PORTS_MODULE_NAME "drivers/power/qcom_adsp/usbc_ports/v1"
#define QCOM_USBC_PORT_COUNT 2

struct qcom_usbc_port {
	uint32	generation;		// counts the reports; 0: none yet
	uint8	display_port;	// in the DisplayPort alternate mode (SVID 0xff01)
	uint8	hpd;			// the sink's hot plug detect
	uint8	hpd_irq;
	uint8	pin_assignment;	// as reported (the DisplayPort pin assignment)
	uint8	orientation;	// 0 normal, 1 reverse
	uint8	mux;
	uint8	reserved[2];
};

struct qcom_usbc_ports_module_info {
	module_info	info;
	status_t	(*get_port)(uint32 index, qcom_usbc_port* port);
};


#endif	// _QCOM_USBC_PORTS_H
