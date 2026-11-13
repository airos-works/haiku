/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <sys/haiku-module.h>


HAIKU_FBSD_DRIVER_GLUE(rtl8125, rge, pci)
NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_REENABLE_INTERRUPTS();
HAIKU_DRIVER_REQUIREMENTS(FBSD_FAST_TASKQUEUE);
HAIKU_FIRMWARE_VERSION(0);
NO_HAIKU_FIRMWARE_NAME_MAP();


/* rge_encap() turns CSUM_IP/CSUM_TCP/CSUM_UDP into descriptor bits; checked
 * on an RTL8126 (airos-works/haiku#124) */
int __haiku_tx_checksum_offload = 1;
