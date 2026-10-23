/*
 * Copyright 2003-2008, Marcus Overhagen. All rights reserved.
 * Copyright 2005-2008, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 *
 * Distributed under the terms of the MIT License.
 */
#ifndef __PCI_H__
#define __PCI_H__

#include <bus/PCI.h>
#include <bus/PCIInterrupts.h>

#include <VectorMap.h>

#include "pci_msi.h"


#define TRACE_PCI
#ifndef TRACE_PCI
#	define TRACE(x)
#else
#	define TRACE(x) dprintf x
#endif

struct PCIDev;


class PCIResourceWindow {
public:
	status_t		AllocateResource(uint64 size, uint64 alignment, pci_resource_range& out, uint8 flags);
	void			FreeResource(pci_resource_range resource);
	status_t		ReserveResource(uint64 start, uint64 size, pci_resource_range& out);

private:
	void			_MergeResourcesAt(int lower, int upper);

	Vector<pci_resource_range> fResources;
};


struct PCIBus {
	PCIDev *			parent;
	PCIDev *			child;
	uint8				domain;
	uint8				bus;

	PCIResourceWindow	io_window;
	PCIResourceWindow	memory_window;
};


struct PCIDev {
	PCIDev *			next;
	PCIBus *			parent;
	PCIBus *			child;
	uint8				domain;
	uint8				bus;
	uint8				device;
	uint8				function;
	pci_info			info;

	msi_info			msi;
	msix_info			msix;
	ht_mapping_info		ht_mapping;
	bool				intx_enabled = false;
	uint8				intx_pin = 0;

	pci_resource_range	bar[6];

	// state saved while suspended
	bool				config_saved;
	uint32				saved_config[64];
	uint32				saved_msix_table[32 * 4];
};


struct domain_data {
	~domain_data()
	{
		if (intx_controller != NULL)
			put_module(intx_controller->info.name);
#if !(defined(__i386__) || defined(__x86_64__))
		if (io_port_area >= 0)
			delete_area(io_port_area);
#endif
	}

	// These two are set in PCI::AddController:
	pci_controller_module_info *controller;
	void *				controller_cookie;
	device_node *		root_node;
	PCIBus *			bus;

	// All the rest is set in PCI::InitDomainData
	int					max_bus_devices;
	Vector<pci_resource_range> ranges;
	pci_intx_controller_module_info* intx_controller = NULL;
	status_t			intx_status = B_OK;

#if !(defined(__i386__) || defined(__x86_64__))
	area_id				io_port_area = -1;
	uint8 *				io_port_adr = NULL;
#endif
};


class PCI {
public:
							PCI();
							~PCI();

			void			InitDomainData(domain_data &data);
			void			InitBus(PCIBus *bus);

			status_t		AddController(pci_controller_module_info *controller,
								void *controllerCookie, device_node *rootNode,
								domain_data **domainData);

			status_t		LookupRange(uint8 domain, uint32 type,
								phys_addr_t pciAddr, pci_resource_range &range);
			status_t		LookupRange(uint32 type, phys_addr_t pciAddr,
								uint8 &domain, pci_resource_range &range, uint8 **mappedAdr = NULL);

			status_t		GetNthInfo(long index, pci_info *outInfo);

			status_t		ReadConfig(uint8 domain, uint8 bus, uint8 device,
								uint8 function, uint16 offset, uint8 size,
								uint32 *value);
			uint32			ReadConfig(uint8 domain, uint8 bus, uint8 device,
								uint8 function, uint16 offset, uint8 size);
			uint32			ReadConfig(PCIDev *device, uint16 offset,
								uint8 size);

			status_t		WriteConfig(uint8 domain, uint8 bus, uint8 device,
								uint8 function, uint16 offset, uint8 size,
								uint32 value);
			status_t		WriteConfig(PCIDev *device, uint16 offset,
								uint8 size, uint32 value);

			status_t		FindCapability(uint8 domain, uint8 bus,
								uint8 device, uint8 function, uint8 capID,
								uint8 *offset = NULL);
			status_t		FindCapability(PCIDev *device, uint8 capID,
								uint8 *offset = NULL);
			status_t		FindExtendedCapability(uint8 domain, uint8 bus,
								uint8 device, uint8 function, uint16 capID,
								uint16 *offset = NULL);
			status_t		FindExtendedCapability(PCIDev *device,
								uint16 capID, uint16 *offset = NULL);
			status_t		FindHTCapability(uint8 domain, uint8 bus,
								uint8 device, uint8 function, uint16 capID,
								uint8 *offset);
			status_t		FindHTCapability(PCIDev *device,
								uint16 capID, uint8 *offset = NULL);

			status_t		ResolveVirtualBus(uint8 virtualBus, uint8 *domain,
								uint8 *bus);

			PCIDev *		FindDevice(uint8 domain, uint8 bus, uint8 device,
								uint8 function);

			void			ClearDeviceStatus(PCIBus *bus, bool dumpStatus);

			uint8			GetPowerstate(PCIDev *device);
			status_t		GetPowerstate(uint8 domain, uint8 bus, uint8 device,
								uint8 function, uint8* state);
			void			SetPowerstate(PCIDev *device, uint8 state);
			status_t		SetPowerstate(uint8 domain, uint8 bus, uint8 device,
								uint8 function, uint8 newState);

			void			RefreshDeviceInfo();

			void			SaveConfiguration();
			void			RestoreConfiguration();

			status_t		UpdateInterruptLine(uint8 domain, uint8 bus,
								uint8 device, uint8 function,
								uint8 newInterruptLineValue);

			status_t		GetIntxIRQ(PCIDev* device, uint32* irq);
			status_t		SetIntxEnabled(PCIDev* device, bool enabled);

			uint32			GetMSICount(PCIDev *device);
			status_t		ConfigureMSI(PCIDev *device, uint32 count, uint32 *startVector);
			status_t		UnconfigureMSI(PCIDev *device);
			status_t		EnableMSI(PCIDev *device);
			status_t		DisableMSI(PCIDev *device);
			uint32			GetMSIXCount(PCIDev *device);
			status_t		ConfigureMSIX(PCIDev *device, uint32 count, uint32 *startVector);
			status_t		EnableMSIX(PCIDev *device);

private:
			void			_EnumerateBus(uint8 domain, uint8 bus,
								uint8 *subordinateBus = NULL);

			void			_FixupDevices(uint8 domain, uint8 bus);

			void			_DiscoverBus(PCIBus *bus);
			void			_DiscoverAdditionalRootBuses(PCIBus *bus);
			bool			_IsBusKnown(PCIBus *bus, uint8 busNumber);
			void			_DiscoverDevice(PCIBus *bus, uint8 dev,
								uint8 function);

			PCIDev *		_CreateDevice(PCIBus *parent, uint8 dev,
								uint8 function);
			PCIBus *		_CreateBus(PCIDev *parent, uint8 domain,
								uint8 bus);

			status_t		_GetNthInfo(PCIBus *bus, long *currentIndex,
								long wantIndex, pci_info *outInfo);
			void			_ReadBasicInfo(PCIDev *dev);
			void			_ReadHeaderInfo(PCIDev *dev);

			void			_ConfigureBridges(PCIBus *bus);
			void			_ReserveBARs(PCIBus *bus);
			void			_AssignBARs(PCIBus *bus);
			void			_RefreshDeviceInfo(PCIBus *bus);
			void			_SaveConfiguration(PCIBus *bus);
			void			_RestoreConfiguration(PCIBus *bus);
			void			_RestoreDeviceConfiguration(PCIDev *device);

			uint64			_BarSize(uint64 bits);
			size_t			_GetBarInfo(PCIDev *dev, uint8 offset,
								uint32 &ramAddress, uint32 &pciAddress,
								uint32 &size, uint8 &flags,
								uint32 *highRAMAddress = NULL,
								uint32 *highPCIAddress = NULL,
								uint32 *highSize = NULL);
			void			_GetRomBarInfo(PCIDev *dev, uint8 offset,
								uint32 &address, uint32 *size = NULL,
								uint8 *flags = NULL);

public:
			domain_data *	_GetDomainData(uint8 domain);

private:
			status_t		_CreateVirtualBus(uint8 domain, uint8 bus,
								uint8 *virtualBus);

			int				_NumFunctions(uint8 domain, uint8 bus,
								uint8 device);
			PCIDev *		_FindDevice(PCIBus *current, uint8 domain,
								uint8 bus, uint8 device, uint8 function);

			void			_HtMSIMap(PCIDev *device, uint64 address);
			status_t		_AllocateMSIVectors(PCIDev* device, uint32 count,
								uint32* startVector, uint64* address, uint32* data);
			void			_ReadMSIInfo(PCIDev *device);
			void			_ReadMSIXInfo(PCIDev *device);
			void			_ReadHtMappingInfo(PCIDev *device);
			status_t		_UnconfigureMSIX(PCIDev *device);
			status_t		_DisableMSIX(PCIDev *device);

private:
	enum { MAX_PCI_DOMAINS = 8 };

	domain_data				fDomainData[MAX_PCI_DOMAINS];
	uint8					fDomainCount;
	bool					fBusEnumeration;

	typedef VectorMap<uint8, uint16> VirtualBusMap;

	VirtualBusMap			fVirtualBusMap;
	int						fNextVirtualBus;
};

extern PCI *gPCI;



extern "C" {

status_t	pci_init(void);
status_t	pci_init_deferred(void);
void		pci_uninit(void);

long		pci_get_nth_pci_info(long index, pci_info *outInfo);

uint32		pci_read_config(uint8 virtualBus, uint8 device, uint8 function,
				uint16 offset, uint8 size);
void		pci_write_config(uint8 virtualBus, uint8 device, uint8 function,
				uint16 offset, uint8 size, uint32 value);

void		__pci_resolve_virtual_bus(uint8 virtualBus, uint8 *domain, uint8 *bus);

}

#endif	/* __PCI_H__ */
