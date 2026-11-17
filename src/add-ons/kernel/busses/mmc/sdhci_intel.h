/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SDHCI_INTEL_H
#define _SDHCI_INTEL_H


#include <SupportDefs.h>


bool	sdhci_intel_acpi_is_sdio_slot(const char* hid, const char* uid);
bool	sdhci_intel_acpi_is_slot(const char* hid, const char* uid);
uint32	sdhci_intel_acpi_slot_quirks(const char* hid, const char* uid);
uint32	sdhci_intel_pci_slot_quirks(uint16 vendor, uint16 device);
bool	sdhci_intel_pci_is_baytrail(uint16 vendor, uint16 device);
void	sdhci_intel_byt_ocp_fixup();
void	sdhci_acpi_fix_up_power(const char* path);


#endif	// _SDHCI_INTEL_H
