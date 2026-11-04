/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _PCI_HOTPLUG_H
#define _PCI_HOTPLUG_H


#include <module.h>


#define B_PCI_HOTPLUG_MODULE_NAME	"bus_managers/pci/hotplug/v1"


/*!	PCI Express hot plug: devices that appear behind, or vanish from, the
	hot plug capable ports (root ports and switch downstream ports with a
	hot plug slot, such as the PCIe tunnels of USB4 and Thunderbolt).
*/
typedef struct pci_hotplug_module_info {
	module_info	info;

	// Looks at every hot plug port: enumerates, configures and publishes
	// the devices behind a port whose link came up, and removes those
	// behind a port whose link went down.
	status_t	(*rescan)(uint32* _added, uint32* _removed);
} pci_hotplug_module_info;


#endif	// _PCI_HOTPLUG_H
