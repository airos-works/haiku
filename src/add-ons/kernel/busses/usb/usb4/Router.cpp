/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * USB4 routers and their adapters: enumeration, link setup, router
 * operations and time management (USB4 specification chapters 8 and 11).
 */


#include "Router.h"

#include <KernelExport.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "Domain.h"
#include "usb4_debug.h"


#ifndef ROUNDUP
#	define ROUNDUP(a, b) (((a) + ((b) - 1)) / (b) * (b))
#endif


// #pragma mark - HopAllocator


HopAllocator::HopAllocator()
{
	Clear();
}


void
HopAllocator::Clear()
{
	memset(fBits, 0, sizeof(fBits));
}


int32
HopAllocator::Allocate(int32 min, int32 max)
{
	if (max >= USB4_MAX_HOPS)
		max = USB4_MAX_HOPS - 1;
	for (int32 hop = min; hop <= max; hop++) {
		if ((fBits[hop / 32] & (1u << (hop % 32))) == 0) {
			fBits[hop / 32] |= 1u << (hop % 32);
			return hop;
		}
	}
	return -1;
}


bool
HopAllocator::Reserve(int32 hop)
{
	if (hop < 0 || hop >= USB4_MAX_HOPS)
		return false;
	if ((fBits[hop / 32] & (1u << (hop % 32))) != 0)
		return false;
	fBits[hop / 32] |= 1u << (hop % 32);
	return true;
}


void
HopAllocator::Release(int32 hop)
{
	if (hop >= 0 && hop < USB4_MAX_HOPS)
		fBits[hop / 32] &= ~(1u << (hop % 32));
}


// #pragma mark - Adapter


bool
Adapter::IsUpstream() const
{
	return router->Route() != 0 && number == router->UpstreamAdapterNumber();
}


bool
Adapter::HasRemote() const
{
	if (remote == NULL || IsUpstream())
		return false;
	// the second lane of a link leads to the same router (and its remote
	// is the router's second upstream lane, which points back up)
	if (dualLink != NULL && linkNumber != 0)
		return false;
	return remote->router != router && remote->router->Parent() == router;
}


status_t
Adapter::Read(usb4_config_space space, uint16 offset, uint32* buffer,
	size_t dwords)
{
	if (router->IsUnplugged())
		return B_DEV_NOT_READY;
	return router->GetDomain()->ReadConfig(router->Route(), number, space,
		offset, buffer, dwords);
}


status_t
Adapter::Write(usb4_config_space space, uint16 offset, const uint32* buffer,
	size_t dwords)
{
	if (router->IsUnplugged())
		return B_DEV_NOT_READY;
	return router->GetDomain()->WriteConfig(router->Route(), number, space,
		offset, buffer, dwords);
}


int32
Adapter::FindCapability(uint8 id)
{
	uint32 offset = ADP_CS_1_NEXT_CAP(header[1]);
	for (int i = 0; offset != 0 && offset < 0xff && i < 32; i++) {
		uint32 value;
		status_t status = ReadAdapter(offset, value);
		if (status != B_OK)
			return status;
		if (((value >> 8) & 0xff) == id)
			return offset;
		offset = value & 0xff;
	}
	return B_ENTRY_NOT_FOUND;
}


int32
Adapter::State()
{
	if (capLane == 0)
		return B_BAD_VALUE;
	uint32 value;
	status_t status = ReadAdapter(capLane + LANE_ADP_CS_1, value);
	if (status != B_OK)
		return status;
	return LANE_ADP_CS_1_ADAPTER_STATE(value);
}


int32
Adapter::LinkSpeed()
{
	if (capLane == 0)
		return B_BAD_VALUE;
	uint32 value;
	status_t status = ReadAdapter(capLane + LANE_ADP_CS_1, value);
	if (status != B_OK)
		return status;
	switch (LANE_ADP_CS_1_CURRENT_SPEED(value)) {
		case LANE_SPEED_GEN4:
			return 40;
		case LANE_SPEED_GEN3:
			return 20;
		default:
			return 10;
	}
}


int32
Adapter::LinkWidth()
{
	if (capLane == 0)
		return B_BAD_VALUE;
	uint32 value;
	status_t status = ReadAdapter(capLane + LANE_ADP_CS_1, value);
	if (status != B_OK)
		return status;
	return LANE_ADP_CS_1_CURRENT_WIDTH(value);
}


bool
Adapter::IsEnabled()
{
	if (capAdapter == 0)
		return false;

	uint32 value;
	switch (type) {
		case ADAPTER_PCIE_DOWN:
		case ADAPTER_PCIE_UP:
			return ReadAdapter(capAdapter + ADP_PCIE_CS_0, value) == B_OK
				&& (value & ADP_PCIE_CS_0_PE) != 0;
		case ADAPTER_USB3_DOWN:
		case ADAPTER_USB3_UP:
			return ReadAdapter(capAdapter + ADP_USB3_CS_0, value) == B_OK
				&& (value & ADP_USB3_CS_0_PE) != 0;
		case ADAPTER_DP_IN:
		case ADAPTER_DP_OUT:
			return ReadAdapter(capAdapter + ADP_DP_CS_0, value) == B_OK
				&& (value & (ADP_DP_CS_0_VE | ADP_DP_CS_0_AE)) != 0;
		default:
			return false;
	}
}


status_t
Adapter::UpdateCredits()
{
	uint32 value;
	status_t status = ReadAdapter(ADP_CS_4, value);
	if (status != B_OK)
		return status;
	if (value != header[4]) {
		uint16 total = ADP_CS_4_TOTAL_BUFFERS(value);
		TRACE("%#" B_PRIx64 ":%u: total credits %u -> %u\n", router->Route(),
			number, totalCredits, total);
		header[4] = value;
		totalCredits = total;
	}
	return B_OK;
}


status_t
Adapter::AddNFCCredits(int32 credits)
{
	if (credits == 0 || router->IsUnplugged())
		return B_OK;
	// USB4 protocol adapters have their non flow controlled buffers fixed
	if (router->IsUSB4() && !IsLane())
		return B_OK;

	int32 nfc = header[4] & ADP_CS_4_NFC_BUFFERS_MASK;
	if (credits < 0 && -credits > nfc)
		credits = -nfc;
	nfc += credits;

	header[4] = (header[4] & ~ADP_CS_4_NFC_BUFFERS_MASK) | nfc;
	return WriteAdapter(ADP_CS_4, header[4]);
}


const char*
Adapter::TypeName() const
{
	switch (type) {
		case ADAPTER_INACTIVE:
			return "inactive";
		case ADAPTER_LANE:
			return "lane";
		case ADAPTER_NHI:
			return "host interface";
		case ADAPTER_DP_IN:
			return "DP IN";
		case ADAPTER_DP_OUT:
			return "DP OUT";
		case ADAPTER_PCIE_DOWN:
			return "PCIe down";
		case ADAPTER_PCIE_UP:
			return "PCIe up";
		case ADAPTER_USB3_DOWN:
			return "USB3 down";
		case ADAPTER_USB3_UP:
			return "USB3 up";
		default:
			return "unknown";
	}
}


// #pragma mark - Router


Router::Router(Domain* domain, Router* parent, uint64 route)
	:
	fDomain(domain),
	fParent(parent),
	fRoute(route),
	fDepth(route_length(route)),
	fUpstreamAdapter(0),
	fMaxAdapter(0),
	fAdapters(NULL),
	fUSB4(false),
	fLinkUSB4(false),
	fUnplugged(false),
	fLinkSpeed(0),
	fLinkWidth(0),
	fUID(0),
	fCapTMU(0),
	fCapPlugEvents(0),
	fCapLinkController(0),
	fCapTime2(0),
	fTMUMode(TMU_MODE_OFF),
	fTMUUnidirectional(false),
	fCreditAllocation(false),
	fMaxUSB3Credits(0),
	fMinDPAuxCredits(0),
	fMinDPMainCredits(0),
	fMaxPCIeCredits(0),
	fMaxDMACredits(0)
{
	memset(fConfig, 0, sizeof(fConfig));
	fVendorName[0] = '\0';
	fDeviceName[0] = '\0';
}


Router::~Router()
{
	delete[] fAdapters;
}


status_t
Router::ReadConfig(uint16 offset, uint32* buffer, size_t dwords)
{
	if (fUnplugged)
		return B_DEV_NOT_READY;
	return fDomain->ReadConfig(fRoute, 0, USB4_CFG_ROUTER, offset, buffer,
		dwords);
}


status_t
Router::WriteConfig(uint16 offset, const uint32* buffer, size_t dwords)
{
	if (fUnplugged)
		return B_DEV_NOT_READY;
	return fDomain->WriteConfig(fRoute, 0, USB4_CFG_ROUTER, offset, buffer,
		dwords);
}


status_t
Router::WaitForBit(uint16 offset, uint32 bit, uint32 value, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	do {
		uint32 current;
		status_t status = ReadConfig(offset, current);
		if (status != B_OK)
			return status;
		if ((current & bit) == value)
			return B_OK;
		snooze(50);
	} while (system_time() < deadline);
	return B_TIMED_OUT;
}


void
Router::SetUnplugged()
{
	if (fUnplugged)
		return;
	fUnplugged = true;
	for (uint8 i = 1; i <= fMaxAdapter; i++) {
		Adapter& adapter = fAdapters[i];
		if (adapter.present && adapter.HasRemote())
			adapter.remote->router->SetUnplugged();
	}
}


Adapter*
Router::DownstreamAdapter()
{
	if (fParent == NULL)
		return NULL;
	return fParent->AdapterTowards(fRoute);
}


Adapter*
Router::AdapterTowards(uint64 route)
{
	return AdapterAt(route_adapter_at(route, fDepth));
}


Adapter*
Router::FindAdapter(uint32 type)
{
	for (uint8 i = 1; i <= fMaxAdapter; i++) {
		if (fAdapters[i].present && fAdapters[i].type == type)
			return &fAdapters[i];
	}
	return NULL;
}


Adapter*
Router::FindUnusedAdapter(uint32 type)
{
	for (uint8 i = 1; i <= fMaxAdapter; i++) {
		Adapter& adapter = fAdapters[i];
		if (adapter.present && adapter.type == type && !adapter.IsEnabled())
			return &adapter;
	}
	return NULL;
}


/*!	USB4 routers pair their protocol adapters with their USB4 ports in
	order: the n-th downstream USB4 port uses the n-th PCIe or USB3 down
	adapter. Keeping to that puts devices in the same place in the PCIe
	and USB hierarchies whenever they are plugged in.
*/
Adapter*
Router::MapProtocolDown(Adapter* lane, uint32 type)
{
	int32 index = 0;
	if (fUSB4) {
		for (uint8 i = 1; i <= fMaxAdapter; i++) {
			Adapter& adapter = fAdapters[i];
			if (!adapter.present || !adapter.IsLane() || adapter.IsUpstream())
				continue;
			if (adapter.linkNumber != 0)
				continue;
			if (&adapter == lane)
				break;
			index++;
		}

		int32 current = 0;
		for (uint8 i = 1; i <= fMaxAdapter; i++) {
			Adapter& adapter = fAdapters[i];
			if (!adapter.present || adapter.type != type)
				continue;
			if (current == index) {
				if (!adapter.IsEnabled())
					return &adapter;
				break;
			}
			current++;
		}
	}

	return FindUnusedAdapter(type);
}


status_t
Router::UnlockAdapter(Adapter* adapter)
{
	if (!fUSB4 || adapter == NULL || !adapter->IsLane())
		return B_OK;

	uint32 value;
	status_t status = adapter->ReadAdapter(ADP_CS_4, value);
	if (status != B_OK)
		return status;
	return adapter->WriteAdapter(ADP_CS_4, value & ~ADP_CS_4_LCK);
}


status_t
Router::Probe()
{
	// the response to any read tells which adapter we came in through
	uint32 value;
	ControlResult result = fDomain->RawRead(fRoute, 0, USB4_CFG_ROUTER, 0,
		&value, 1);
	if (result.status != B_OK)
		return result.status;
	uint8 upstream = result.port;

	status_t status = ReadConfig(ROUTER_CS_0, fConfig, 5);
	if (status != B_OK)
		return status;

	fUSB4 = USB4Version() > 0;
	fMaxAdapter = ROUTER_CS_1_MAX_ADAPTER(fConfig[1]);
	fUpstreamAdapter = upstream;

	if (fDepth > 5) {
		TRACE_ERROR("%#" B_PRIx64 ": topology too deep\n", fRoute);
		return B_BAD_VALUE;
	}

	// what this router is going to be told: upstream adapter, depth and
	// route, but not yet valid
	fConfig[1] = (fConfig[1] & ~((0x3fu << 8) | (0x7u << 20)))
		| ((uint32)upstream << 8) | ((uint32)fDepth << 20);
	fConfig[2] = (uint32)fRoute;
	fConfig[3] = (uint32)(fRoute >> 32) & 0x7fffffff;

	fAdapters = new(std::nothrow) Adapter[fMaxAdapter + 1];
	if (fAdapters == NULL)
		return B_NO_MEMORY;
	for (uint8 i = 0; i <= fMaxAdapter; i++) {
		Adapter& adapter = fAdapters[i];
		adapter.router = this;
		adapter.number = i;
		adapter.present = false;
		adapter.type = ADAPTER_INACTIVE;
		memset(adapter.header, 0, sizeof(adapter.header));
		adapter.capLane = adapter.capUSB4 = adapter.capAdapter = 0;
		adapter.capTMU = 0;
		adapter.maxInHop = adapter.maxOutHop = 0;
		adapter.totalCredits = adapter.ctlCredits = 0;
		adapter.remote = NULL;
		adapter.dualLink = NULL;
		adapter.linkNumber = 0;
		adapter.bonded = false;
		adapter.dpResource = false;
	}

	int32 cap = _FindVSECapability(USB4_VSE_CAP_PLUG_EVENTS);
	if (cap > 0)
		fCapPlugEvents = cap;
	cap = _FindVSECapability(USB4_VSE_CAP_LINK_CONTROLLER);
	if (cap > 0)
		fCapLinkController = cap;
	cap = _FindVSECapability(USB4_VSE_CAP_TIME2);
	if (cap > 0)
		fCapTime2 = cap;

	TRACE("%#" B_PRIx64 ": router %04x:%04x, %s, %u adapters, upstream %u\n",
		fRoute, VendorID(), DeviceID(),
		fUSB4 ? "USB4" : "Thunderbolt", fMaxAdapter, upstream);
	return B_OK;
}


status_t
Router::Configure()
{
	fConfig[3] |= ROUTER_CS_3_V;

	status_t status;
	if (fUSB4) {
		// we are a USB4 version 1 connection manager; ask for a
		// notification within 10 ms
		uint32 cmuv = USB4Version() < 2 ? ROUTER_CS_4_CMUV_V1
			: ROUTER_CS_4_CMUV_V2;
		fConfig[4] = (fConfig[4] & ~0xffffu) | (cmuv << 8) | 0xa;
		status = WriteConfig(ROUTER_CS_1, fConfig + 1, 4);
		if (status != B_OK)
			return status;

		if (IsHost())
			return B_OK;

		uint32 cs6;
		status = ReadConfig(ROUTER_CS_6, cs6);
		if (status != B_OK)
			return status;

		Adapter* down = DownstreamAdapter();
		fLinkUSB4 = false;
		if (down != NULL && down->capUSB4 != 0) {
			uint32 value;
			if (down->ReadAdapter(down->capUSB4 + PORT_CS_18, value) == B_OK)
				fLinkUSB4 = (value & PORT_CS_18_TCM) == 0;
		}
		bool xhci = (cs6 & ROUTER_CS_6_HCI) != 0;
		bool tbt3 = (cs6 & ROUTER_CS_6_TNS) == 0;
		TRACE("%#" B_PRIx64 ": link %s, Thunderbolt 3 %s, xHCI %s\n", fRoute,
			fLinkUSB4 ? "USB4" : "Thunderbolt", tbt3 ? "yes" : "no",
			xhci ? "yes" : "no");

		uint32 cs5;
		status = ReadConfig(ROUTER_CS_5, cs5);
		if (status != B_OK)
			return status;

		if (fDomain->MayTunnelUSB3() && fLinkUSB4
			&& fParent->FindAdapter(ADAPTER_USB3_DOWN) != NULL) {
			cs5 |= ROUTER_CS_5_UTO;
			xhci = false;
		}
		if (fDomain->MayTunnelPCIe()
			&& fParent->FindAdapter(ADAPTER_PCIE_DOWN) != NULL) {
			cs5 |= ROUTER_CS_5_PTO;
			if (xhci)
				cs5 |= ROUTER_CS_5_HCO;
		}
		// we handle Thunderbolt 3 routers too
		cs5 &= ~ROUTER_CS_5_CNS;
		return WriteConfig(ROUTER_CS_5, cs5);
	}

	// Thunderbolt 3 and older routers
	status = WriteConfig(ROUTER_CS_1, fConfig + 1, 3);
	if (status != B_OK)
		return status;

	if (fCapPlugEvents != 0) {
		// plug events on: clear the disable bits
		fConfig[4] = (fConfig[4] & ~0xffu) | 0xff;
		WriteConfig(ROUTER_CS_4, fConfig[4]);
		uint32 value;
		if (ReadConfig(fCapPlugEvents + 1, value) == B_OK) {
			value &= 0xffffff83;
			WriteConfig(fCapPlugEvents + 1, value);
		}
	}
	return B_OK;
}


status_t
Router::_InitAdapter(Adapter& adapter)
{
	if (adapter.number == 0) {
		// the control adapter has no configuration space
		adapter.present = true;
		return B_OK;
	}

	status_t status = adapter.Read(USB4_CFG_ADAPTER, ADP_CS_0, adapter.header,
		8);
	if (status == B_ENTRY_NOT_FOUND) {
		adapter.present = false;
		return B_OK;
	}
	if (status != B_OK)
		return status;

	adapter.present = true;
	adapter.type = ADP_CS_2_TYPE(adapter.header[2]);
	adapter.maxInHop = ADP_CS_5_MAX_IN_HOP(adapter.header[5]);
	adapter.maxOutHop = ADP_CS_5_MAX_OUT_HOP(adapter.header[5]);
	adapter.totalCredits = ADP_CS_4_TOTAL_BUFFERS(adapter.header[4]);

	if (adapter.IsLane()) {
		int32 cap = adapter.FindCapability(USB4_ADP_CAP_LANE);
		if (cap > 0)
			adapter.capLane = cap;
		else
			TRACE_ERROR("%#" B_PRIx64 ":%u: lane adapter without lane "
				"capability\n", fRoute, adapter.number);
		cap = adapter.FindCapability(USB4_ADP_CAP_USB4);
		if (cap > 0)
			adapter.capUSB4 = cap;

		// the control path keeps its buffers
		if (adapter.capUSB4 != 0) {
			uint32 hop[2];
			if (adapter.Read(USB4_CFG_HOPS, 0, hop, 2) == B_OK)
				adapter.ctlCredits = (hop[0] >> 17) & 0x7f;
		}
		if (adapter.ctlCredits == 0)
			adapter.ctlCredits = 2;
	} else {
		int32 cap = adapter.FindCapability(USB4_ADP_CAP_ADAPTER);
		if (cap > 0)
			adapter.capAdapter = cap;
	}

	int32 cap = adapter.FindCapability(USB4_ADP_CAP_TMU);
	if (cap > 0)
		adapter.capTMU = cap;

	TRACE_VERBOSE("%#" B_PRIx64 ":%u: %s, credits %u, hops %u/%u\n", fRoute,
		adapter.number, adapter.TypeName(), adapter.totalCredits,
		adapter.maxInHop, adapter.maxOutHop);
	return B_OK;
}


int32
Router::_NextCapability(uint16 offset)
{
	if (offset == 0)
		return ROUTER_CS_1_NEXT_CAP(fConfig[1]);

	uint32 header[2];
	status_t status = ReadConfig(offset, header, 2);
	if (status != B_OK)
		return status;

	uint8 id = (header[0] >> 8) & 0xff;
	int32 next;
	if (id == USB4_ROUTER_CAP_TMU)
		next = header[0] & 0xff;
	else if (id == USB4_ROUTER_CAP_VSE) {
		if (((header[0] >> 24) & 0xff) == 0) {
			// long vendor specific capability
			next = header[1] & 0xffff;
		} else
			next = header[0] & 0xff;
	} else
		return B_BAD_DATA;

	return next >= 0xffff ? 0 : next;
}


int32
Router::_FindCapability(uint8 id)
{
	int32 offset = 0;
	for (int i = 0; i < 32; i++) {
		offset = _NextCapability(offset);
		if (offset <= 0)
			return offset < 0 ? offset : B_ENTRY_NOT_FOUND;
		uint32 header;
		status_t status = ReadConfig(offset, header);
		if (status != B_OK)
			return status;
		if (((header >> 8) & 0xff) == id)
			return offset;
	}
	return B_ENTRY_NOT_FOUND;
}


int32
Router::_FindVSECapability(uint8 vsec)
{
	int32 offset = 0;
	for (int i = 0; i < 32; i++) {
		offset = _NextCapability(offset);
		if (offset <= 0)
			return offset < 0 ? offset : B_ENTRY_NOT_FOUND;
		uint32 header;
		status_t status = ReadConfig(offset, header);
		if (status != B_OK)
			return status;
		if (((header >> 8) & 0xff) == USB4_ROUTER_CAP_VSE
			&& ((header >> 16) & 0xff) == vsec)
			return offset;
	}
	return B_ENTRY_NOT_FOUND;
}


status_t
Router::Operation(uint16 opcode, uint32* metadata, uint8* opStatus,
	const uint32* txData, size_t txDwords, uint32* rxData, size_t rxDwords)
{
	if (txDwords > 16 || rxDwords > 16)
		return B_BAD_VALUE;

	status_t status;
	if (metadata != NULL) {
		status = WriteConfig(ROUTER_CS_25, *metadata);
		if (status != B_OK)
			return status;
	}
	if (txDwords > 0) {
		status = WriteConfig(ROUTER_CS_9, txData, txDwords);
		if (status != B_OK)
			return status;
	}

	status = WriteConfig(ROUTER_CS_26, (uint32)opcode | ROUTER_CS_26_OV);
	if (status != B_OK)
		return status;

	status = WaitForBit(ROUTER_CS_26, ROUTER_CS_26_OV, 0, 500000);
	if (status != B_OK)
		return status;

	uint32 value;
	status = ReadConfig(ROUTER_CS_26, value);
	if (status != B_OK)
		return status;
	if ((value & ROUTER_CS_26_ONS) != 0)
		return B_NOT_SUPPORTED;
	if (opStatus != NULL)
		*opStatus = ROUTER_CS_26_STATUS(value);

	if (metadata != NULL) {
		status = ReadConfig(ROUTER_CS_25, *metadata);
		if (status != B_OK)
			return status;
	}
	if (rxDwords > 0)
		return ReadConfig(ROUTER_CS_9, rxData, rxDwords);
	return B_OK;
}


status_t
Router::_InitCredits()
{
	if (!fUSB4)
		return B_OK;

	uint32 data[16];
	memset(data, 0, sizeof(data));
	uint32 metadata = 0;
	uint8 opStatus = 0;
	status_t status = Operation(USB4_ROUTER_OP_BUFFER_ALLOC, &metadata,
		&opStatus, NULL, 0, data, 16);
	if (status != B_OK)
		return status;
	if (opStatus != 0)
		return B_IO_ERROR;

	uint32 length = metadata & USB4_BA_LENGTH_MASK;
	if (length > 16)
		return B_BAD_DATA;

	int32 maxUSB3 = -1, minDPAux = -1, minDPMain = -1, maxPCIe = -1,
		maxDMA = -1;
	for (uint32 i = 0; i < length; i++) {
		uint16 value = USB4_BA_VALUE(data[i]);
		switch (USB4_BA_INDEX(data[i])) {
			case USB4_BA_MAX_USB3:
				maxUSB3 = value;
				break;
			case USB4_BA_MIN_DP_AUX:
				minDPAux = value;
				break;
			case USB4_BA_MIN_DP_MAIN:
				minDPMain = value;
				break;
			case USB4_BA_MAX_PCIE:
				maxPCIe = value;
				break;
			case USB4_BA_MAX_HI:
				maxDMA = value;
				break;
		}
	}

	TRACE("%#" B_PRIx64 ": credits USB3 %" B_PRId32 ", DP aux %" B_PRId32
		", DP main %" B_PRId32 ", PCIe %" B_PRId32 ", DMA %" B_PRId32 "\n",
		fRoute, maxUSB3, minDPAux, minDPMain, maxPCIe, maxDMA);

	// the parameters must cover every adapter the router has, otherwise we
	// use the defaults
	if (IsHost() && maxDMA < 0)
		return B_BAD_DATA;
	int32 lanes = 0;
	for (uint8 i = 1; i <= fMaxAdapter; i++) {
		Adapter& adapter = fAdapters[i];
		if (!adapter.present)
			continue;
		if (adapter.IsLane())
			lanes++;
		if (adapter.IsDPOut() && minDPMain < 0)
			return B_BAD_DATA;
		if ((adapter.IsDPIn() || adapter.IsDPOut()) && minDPAux < 0)
			return B_BAD_DATA;
		if ((adapter.IsUSB3Down() || adapter.IsUSB3Up()) && maxUSB3 < 0)
			return B_BAD_DATA;
		if ((adapter.IsPCIeDown() || adapter.IsPCIeUp()) && maxPCIe < 0)
			return B_BAD_DATA;
	}
	if (lanes > 2 && (minDPAux < 0 || minDPMain < 0))
		return B_BAD_DATA;

	fCreditAllocation = true;
	if (maxUSB3 > 0)
		fMaxUSB3Credits = maxUSB3;
	if (minDPAux > 0)
		fMinDPAuxCredits = minDPAux;
	if (minDPMain > 0)
		fMinDPMainCredits = minDPMain;
	if (maxPCIe > 0)
		fMaxPCIeCredits = maxPCIe;
	if (maxDMA > 0)
		fMaxDMACredits = maxDMA;
	return B_OK;
}


void
Router::_LinkDualPorts()
{
	for (uint8 i = 1; i < fMaxAdapter; i++) {
		Adapter& first = fAdapters[i];
		Adapter& second = fAdapters[i + 1];
		if (!first.present || !first.IsLane() || !second.present
			|| !second.IsLane())
			continue;
		if (first.dualLink != NULL || second.dualLink != NULL)
			continue;
		first.linkNumber = 0;
		first.dualLink = &second;
		second.linkNumber = 1;
		second.dualLink = &first;
		i++;
	}
}


void
Router::_ReadDROM()
{
	if (!fUSB4)
		return;

	// the size is at byte 14 of the DROM
	uint32 metadata = USB4_DROM_SIZE(1) | USB4_DROM_ADDRESS(3);
	uint8 opStatus = 0;
	uint32 data[16];
	if (Operation(USB4_ROUTER_OP_DROM_READ, &metadata, &opStatus, NULL, 0,
			data, 1) != B_OK || opStatus != 0)
		return;
	uint32 size = ((data[0] >> 16) & 0xfff) + 13;
	if (size < 16 || size > 4096)
		return;

	uint8* drom = (uint8*)malloc(ROUNDUP(size, 64));
	if (drom == NULL)
		return;

	for (uint32 offset = 0; offset < size; offset += 64) {
		uint32 dwords = min_c(16, (ROUNDUP(size, 4) - offset) / 4);
		metadata = USB4_DROM_SIZE(dwords) | USB4_DROM_ADDRESS(offset / 4);
		if (Operation(USB4_ROUTER_OP_DROM_READ, &metadata, &opStatus, NULL,
				0, data, dwords) != B_OK || opStatus != 0) {
			free(drom);
			return;
		}
		memcpy(drom + offset, data, dwords * 4);
	}

	uint8 revision = drom[13];
	uint32 position = revision >= 3 ? 16 : 22;
	while (position + 2 <= size) {
		uint8 length = drom[position];
		uint8 info = drom[position + 1];
		if (length < 2 || position + length > size)
			break;
		bool port = (info & 0x80) != 0;
		uint8 index = info & 0x3f;
		if (!port && (index == 1 || index == 2)) {
			char* target = index == 1 ? fVendorName : fDeviceName;
			size_t targetSize = index == 1 ? sizeof(fVendorName)
				: sizeof(fDeviceName);
			size_t textLength = min_c((size_t)length - 2, targetSize - 1);
			memcpy(target, drom + position + 2, textLength);
			target[textLength] = '\0';
		}
		position += length;
	}

	free(drom);
}


status_t
Router::Setup()
{
	if (_InitCredits() != B_OK && fUSB4) {
		TRACE("%#" B_PRIx64 ": no usable buffer allocation, using defaults\n",
			fRoute);
	}

	for (uint8 i = 0; i <= fMaxAdapter; i++) {
		status_t status = _InitAdapter(fAdapters[i]);
		if (status != B_OK) {
			TRACE_ERROR("%#" B_PRIx64 ":%u: init failed: %s\n", fRoute, i,
				strerror(status));
			return status;
		}
	}

	if (fUSB4) {
		uint32 uid[2];
		if (ReadConfig(ROUTER_CS_7, uid, 2) == B_OK)
			fUID = ((uint64)uid[1] << 32) | uid[0];
		_ReadDROM();
	}

	_LinkDualPorts();

	if (!IsHost()) {
		Adapter* up = UpstreamAdapter();
		Adapter* down = DownstreamAdapter();
		if (up == NULL || down == NULL)
			return B_BAD_DATA;
		fLinkSpeed = up->LinkSpeed();
		fLinkWidth = up->LinkWidth();

		bool bonded = fLinkWidth >= LINK_WIDTH_DUAL;
		up->bonded = bonded;
		if (up->dualLink != NULL)
			up->dualLink->bonded = bonded;
		up->UpdateCredits();
		down->bonded = bonded;
		if (down->dualLink != NULL)
			down->dualLink->bonded = bonded;
		down->UpdateCredits();
	}

	// time management unit
	int32 cap = _FindCapability(USB4_ROUTER_CAP_TMU);
	if (cap > 0) {
		fCapTMU = cap;
		uint32 cs0, cs3;
		if (ReadConfig(fCapTMU + TMU_RTR_CS_0, cs0) == B_OK
			&& ReadConfig(fCapTMU + TMU_RTR_CS_3, cs3) == B_OK) {
			fTMUUnidirectional = (cs0 & TMU_RTR_CS_0_UCAP) != 0;
			uint32 rate = cs3 >> TMU_RTR_CS_3_TS_PACKET_INTERVAL_SHIFT;
			fTMUMode = rate != 0 ? TMU_MODE_HIFI_BI : TMU_MODE_OFF;
			Adapter* up = UpstreamAdapter();
			if (!IsHost() && up != NULL && up->capTMU != 0
				&& fTMUUnidirectional) {
				uint32 value;
				if (up->ReadAdapter(up->capTMU + TMU_ADP_CS_3, value) == B_OK
					&& (value & TMU_ADP_CS_3_UDM) != 0) {
					fTMUMode = rate == TMU_RATE_LOWRES ? TMU_MODE_LOWRES
						: TMU_MODE_HIFI_UNI;
				}
			}
		}
	}

	// hot plug events from the USB4 ports
	for (uint8 i = 1; i <= fMaxAdapter; i++) {
		Adapter& adapter = fAdapters[i];
		if (!adapter.present || adapter.capUSB4 == 0)
			continue;
		uint32 value;
		if (adapter.ReadAdapter(ADP_CS_5, value) == B_OK)
			adapter.WriteAdapter(ADP_CS_5, value & ~ADP_CS_5_DHP);
	}

	return B_OK;
}


status_t
Router::SetConfigurationValid()
{
	if (IsHost() || !fUSB4)
		return B_OK;

	uint32 value;
	status_t status = ReadConfig(ROUTER_CS_5, value);
	if (status != B_OK)
		return status;
	status = WriteConfig(ROUTER_CS_5, value | ROUTER_CS_5_CV);
	if (status != B_OK)
		return status;
	return WaitForBit(ROUTER_CS_6, ROUTER_CS_6_CR, ROUTER_CS_6_CR, 50000);
}


// Thunderbolt 3 link controller registers
#define TB_LC_DESC				0x02
#define TB_LC_PORT_MODE			0x26
#define TB_LC_PORT_MODE_DPR		(1u << 0)
#define TB_LC_PORT_ATTR			0x8d
#define TB_LC_PORT_ATTR_BE		(1u << 12)
#define TB_LC_SX_CTRL			0x96
#define TB_LC_SX_CTRL_L1C		(1u << 16)
#define TB_LC_SX_CTRL_L2C		(1u << 20)
#define TB_LC_SX_CTRL_UPSTREAM	(1u << 30)


//! Where the link controller registers of the adapter's port start.
int32
Router::LinkControllerOffset(Adapter* adapter)
{
	if (fCapLinkController == 0 || adapter->number == 0)
		return B_NOT_SUPPORTED;
	uint32 desc;
	status_t status = ReadConfig(fCapLinkController + TB_LC_DESC, desc);
	if (status != B_OK)
		return status;
	uint32 start = (desc >> 8) & 0xff;
	uint32 size = (desc >> 16) & 0xfff;
	uint32 physical = (adapter->number - 1) / 2;
	return fCapLinkController + start + physical * size;
}


static status_t
set_port_configured(Adapter* adapter, bool configured)
{
	if (adapter == NULL)
		return B_OK;

	Router* router = adapter->router;
	if (adapter->capUSB4 == 0 && !router->IsUSB4()) {
		// Thunderbolt 3: through the link controller
		int32 offset = router->LinkControllerOffset(adapter);
		if (offset < 0)
			return B_OK;
		uint32 control;
		status_t status = router->ReadConfig(offset + TB_LC_SX_CTRL, control);
		if (status != B_OK)
			return status;
		uint32 lane = adapter->number % 2 != 0 ? TB_LC_SX_CTRL_L1C
			: TB_LC_SX_CTRL_L2C;
		if (configured) {
			control |= lane;
			if (adapter->IsUpstream())
				control |= TB_LC_SX_CTRL_UPSTREAM;
		} else {
			control &= ~lane;
			if (adapter->IsUpstream())
				control &= ~TB_LC_SX_CTRL_UPSTREAM;
		}
		return router->WriteConfig(offset + TB_LC_SX_CTRL, control);
	}

	if (adapter->capUSB4 == 0)
		return B_OK;
	uint32 value;
	status_t status = adapter->ReadAdapter(adapter->capUSB4 + PORT_CS_19,
		value);
	if (status != B_OK)
		return status;
	if (configured)
		value |= PORT_CS_19_PC;
	else
		value &= ~PORT_CS_19_PC;
	return adapter->WriteAdapter(adapter->capUSB4 + PORT_CS_19, value);
}


static int32
wait_for_adapter(Adapter* adapter)
{
	for (int retry = 0; retry < 10; retry++) {
		int32 state = adapter->State();
		if (state < 0)
			return state;
		switch (state) {
			case ADAPTER_STATE_DISABLED:
			case ADAPTER_STATE_CLD:
				return 0;
			case ADAPTER_STATE_CL0:
			case ADAPTER_STATE_CL0S_TX:
			case ADAPTER_STATE_CL0S_RX:
			case ADAPTER_STATE_CL1:
			case ADAPTER_STATE_CL2:
				return 1;
			default:
				snooze(100000);
				break;
		}
	}
	return 0;
}


static status_t
set_link_width(Adapter* adapter, uint32 width)
{
	uint32 value;
	status_t status = adapter->ReadAdapter(adapter->capLane + LANE_ADP_CS_1,
		value);
	if (status != B_OK)
		return status;
	value = (value & ~LANE_ADP_CS_1_TARGET_WIDTH_MASK) | width;
	return adapter->WriteAdapter(adapter->capLane + LANE_ADP_CS_1, value);
}


static status_t
set_lane_bonding(Adapter* adapter, bool bonding)
{
	uint32 value;
	status_t status = adapter->ReadAdapter(adapter->capLane + LANE_ADP_CS_1,
		value);
	if (status != B_OK)
		return status;
	if (bonding)
		value |= LANE_ADP_CS_1_LB;
	else
		value &= ~LANE_ADP_CS_1_LB;
	return adapter->WriteAdapter(adapter->capLane + LANE_ADP_CS_1, value);
}


static status_t
bond_lanes(Adapter* adapter)
{
	if (adapter->LinkWidth() == LINK_WIDTH_SINGLE) {
		status_t status = set_link_width(adapter,
			LANE_ADP_CS_1_TARGET_WIDTH_DUAL);
		if (status != B_OK)
			return status;
	}
	int32 width = adapter->dualLink->LinkWidth();
	if (width == LINK_WIDTH_SINGLE) {
		status_t status = set_link_width(adapter->dualLink,
			LANE_ADP_CS_1_TARGET_WIDTH_DUAL);
		if (status != B_OK)
			return status;
	}
	// only the downstream side starts the bonding
	if (width == LINK_WIDTH_SINGLE && !adapter->IsUpstream()) {
		status_t status = set_lane_bonding(adapter, true);
		if (status != B_OK)
			return status;
	}
	adapter->bonded = true;
	adapter->dualLink->bonded = true;
	return B_OK;
}


static void
unbond_lanes(Adapter* adapter)
{
	set_lane_bonding(adapter, false);
	set_link_width(adapter->dualLink, LANE_ADP_CS_1_TARGET_WIDTH_SINGLE);
	set_link_width(adapter, LANE_ADP_CS_1_TARGET_WIDTH_SINGLE);
	adapter->bonded = false;
	adapter->dualLink->bonded = false;
}


status_t
Router::EnableLaneBonding()
{
	Adapter* up = UpstreamAdapter();
	Adapter* down = DownstreamAdapter();
	if (up == NULL || down == NULL || up->dualLink == NULL
		|| down->dualLink == NULL || up->dualLink->remote == NULL)
		return B_OK;

	if (fUSB4) {
		uint32 value;
		if (up->ReadAdapter(up->capUSB4 + PORT_CS_18, value) != B_OK
			|| (value & PORT_CS_18_BE) == 0)
			return B_OK;
	} else {
		int32 offset = LinkControllerOffset(up);
		uint32 value;
		if (offset < 0 || ReadConfig(offset + TB_LC_PORT_ATTR, value) != B_OK
			|| (value & TB_LC_PORT_ATTR_BE) == 0)
			return B_OK;
	}

	// Gen 4 links are always bonded
	if (up->LinkSpeed() >= 40)
		return B_OK;

	uint32 value;
	if (up->ReadAdapter(up->capLane + LANE_ADP_CS_0, value) != B_OK
		|| (LANE_ADP_CS_0_SUPPORTED_WIDTH(value) & LINK_WIDTH_DUAL) == 0)
		return B_OK;
	if (down->ReadAdapter(down->capLane + LANE_ADP_CS_0, value) != B_OK
		|| (LANE_ADP_CS_0_SUPPORTED_WIDTH(value) & LINK_WIDTH_DUAL) == 0)
		return B_OK;

	if (wait_for_adapter(down->dualLink) <= 0)
		return B_DEV_NOT_READY;

	status_t status = bond_lanes(up);
	if (status != B_OK) {
		TRACE_ERROR("%#" B_PRIx64 ": lane bonding failed upstream\n", fRoute);
		return status;
	}
	status = bond_lanes(down);
	if (status != B_OK) {
		TRACE_ERROR("%#" B_PRIx64 ": lane bonding failed downstream\n",
			fRoute);
		unbond_lanes(up);
		return status;
	}

	bigtime_t deadline = system_time() + 100000;
	while (system_time() < deadline) {
		int32 width = down->LinkWidth();
		if (width > 0 && (width & (LINK_WIDTH_DUAL | LINK_WIDTH_ASYM_TX
				| LINK_WIDTH_ASYM_RX)) != 0)
			break;
		snooze(1000);
	}

	down->UpdateCredits();
	if (down->dualLink != NULL)
		down->dualLink->UpdateCredits();
	up->UpdateCredits();
	if (up->dualLink != NULL)
		up->dualLink->UpdateCredits();
	fLinkSpeed = up->LinkSpeed();
	fLinkWidth = up->LinkWidth();
	TRACE("%#" B_PRIx64 ": lanes bonded, %" B_PRId32 " Gb/s x %" B_PRId32
		"\n", fRoute, fLinkSpeed, fLinkWidth);
	return B_OK;
}


status_t
Router::ConfigureLink()
{
	Adapter* up = UpstreamAdapter();
	Adapter* down = DownstreamAdapter();
	if (up == NULL || down == NULL)
		return B_BAD_VALUE;

	down->remote = up;
	up->remote = down;
	if (down->dualLink != NULL && up->dualLink != NULL) {
		down->dualLink->remote = up->dualLink;
		up->dualLink->remote = down->dualLink;
	}

	if (fLinkWidth < LINK_WIDTH_DUAL)
		EnableLaneBonding();

	status_t status = set_port_configured(up, true);
	if (status != B_OK)
		return status;
	return set_port_configured(down, true);
}


void
Router::UnconfigureLink()
{
	Adapter* down = DownstreamAdapter();
	set_port_configured(down, false);
	if (!fUnplugged)
		set_port_configured(UpstreamAdapter(), false);
}


status_t
Router::ResetPort(Adapter* lane)
{
	if (lane == NULL)
		return B_OK;

	if (lane->capUSB4 == 0 && !fUSB4) {
		int32 offset = LinkControllerOffset(lane);
		if (offset < 0)
			return B_OK;
		uint32 mode;
		status_t status = ReadConfig(offset + TB_LC_PORT_MODE, mode);
		if (status != B_OK)
			return status;
		status = WriteConfig(offset + TB_LC_PORT_MODE,
			mode | TB_LC_PORT_MODE_DPR);
		if (status != B_OK)
			return status;
		snooze(10000);
		return WriteConfig(offset + TB_LC_PORT_MODE,
			mode & ~TB_LC_PORT_MODE_DPR);
	}

	if (lane->capUSB4 == 0)
		return B_OK;

	uint32 value;
	status_t status = lane->ReadAdapter(lane->capUSB4 + PORT_CS_19, value);
	if (status != B_OK)
		return status;
	status = lane->WriteAdapter(lane->capUSB4 + PORT_CS_19,
		value | PORT_CS_19_DPR);
	if (status != B_OK)
		return status;
	snooze(10000);
	status = lane->ReadAdapter(lane->capUSB4 + PORT_CS_19, value);
	if (status != B_OK)
		return status;
	return lane->WriteAdapter(lane->capUSB4 + PORT_CS_19,
		value & ~PORT_CS_19_DPR);
}


/*!	Takes down everything the boot firmware set up below the host router:
	the links of its USB4 ports are reset and its protocol adapters
	disabled, so the devices come back as hot plugs.
*/
status_t
Router::ResetHost()
{
	uint32 cs3;
	if (ReadConfig(ROUTER_CS_3, cs3) != B_OK || (cs3 & ROUTER_CS_3_V) == 0)
		return B_OK;

	for (uint8 i = 1; i <= fMaxAdapter; i++) {
		Adapter& adapter = fAdapters[i];
		if (!adapter.present)
			continue;

		uint32 value;
		if (adapter.IsLane()) {
			if (adapter.IsUpstream())
				continue;
			status_t status = ResetPort(&adapter);
			if (status != B_OK)
				return status;
		} else if (adapter.IsUSB3Down() || adapter.IsUSB3Up()) {
			if (adapter.capAdapter != 0)
				adapter.WriteAdapter(adapter.capAdapter + ADP_USB3_CS_0,
					ADP_USB3_CS_0_V);
		} else if (adapter.IsDPIn() || adapter.IsDPOut()) {
			if (adapter.capAdapter != 0
				&& adapter.ReadAdapter(adapter.capAdapter + ADP_DP_CS_0,
					value) == B_OK) {
				adapter.WriteAdapter(adapter.capAdapter + ADP_DP_CS_0,
					value & ~(ADP_DP_CS_0_VE | ADP_DP_CS_0_AE));
			}
		} else if (adapter.IsPCIeDown() || adapter.IsPCIeUp()) {
			if (adapter.capAdapter != 0)
				adapter.WriteAdapter(adapter.capAdapter + ADP_PCIE_CS_0, 0);
		} else
			continue;

		// clean the path configuration space
		for (uint32 hop = USB4_FIRST_PATH_HOP; hop <= adapter.maxInHop; hop++)
			fDomain->DeactivateHop(&adapter, hop, true);
	}
	return B_OK;
}


bool
Router::QueryDPResource(Adapter* in)
{
	if (!fUSB4)
		return true;
	uint32 metadata = in->number;
	uint8 opStatus = 0;
	status_t status = Operation(USB4_ROUTER_OP_QUERY_DP_RESOURCE, &metadata,
		&opStatus, NULL, 0, NULL, 0);
	if (status == B_NOT_SUPPORTED)
		return true;
	return status == B_OK && opStatus == 0;
}


status_t
Router::AllocateDPResource(Adapter* in)
{
	if (!fUSB4)
		return B_OK;
	uint32 metadata = in->number;
	uint8 opStatus = 0;
	status_t status = Operation(USB4_ROUTER_OP_ALLOC_DP_RESOURCE, &metadata,
		&opStatus, NULL, 0, NULL, 0);
	if (status == B_NOT_SUPPORTED)
		return B_OK;
	if (status != B_OK)
		return status;
	return opStatus == 0 ? B_OK : B_BUSY;
}


status_t
Router::DeallocateDPResource(Adapter* in)
{
	if (!fUSB4)
		return B_OK;
	uint32 metadata = in->number;
	uint8 opStatus = 0;
	status_t status = Operation(USB4_ROUTER_OP_DEALLOC_DP_RESOURCE, &metadata,
		&opStatus, NULL, 0, NULL, 0);
	if (status == B_NOT_SUPPORTED)
		return B_OK;
	if (status != B_OK)
		return status;
	return opStatus == 0 ? B_OK : B_IO_ERROR;
}


// #pragma mark - time management


status_t
Router::_SetTimeDisruption(bool set)
{
	uint16 offset;
	uint32 bit;
	if (fUSB4) {
		if (fCapTMU == 0)
			return B_OK;
		offset = fCapTMU + TMU_RTR_CS_0;
		bit = TMU_RTR_CS_0_TD;
	} else {
		// Thunderbolt 3 keeps it in a vendor specific capability
		if (fCapTime2 == 0)
			return B_OK;
		offset = fCapTime2 + 0x1a;
		bit = 1u << 22;
	}

	uint32 value;
	status_t status = ReadConfig(offset, value);
	if (status != B_OK)
		return status;
	if (set)
		value |= bit;
	else
		value &= ~bit;
	return WriteConfig(offset, value);
}


status_t
Router::_WriteTMURate(uint32 rate)
{
	uint32 value;
	status_t status = ReadConfig(fCapTMU + TMU_RTR_CS_3, value);
	if (status != B_OK)
		return status;
	value = (value & ~TMU_RTR_CS_3_TS_PACKET_INTERVAL_MASK)
		| (rate << TMU_RTR_CS_3_TS_PACKET_INTERVAL_SHIFT);
	return WriteConfig(fCapTMU + TMU_RTR_CS_3, value);
}


status_t
Router::_SetAdapterTMU(Adapter* adapter, uint16 offset, uint32 mask,
	uint32 value)
{
	if (adapter == NULL || adapter->capTMU == 0)
		return B_OK;
	uint32 current;
	status_t status = adapter->ReadAdapter(adapter->capTMU + offset, current);
	if (status != B_OK)
		return status;
	current = (current & ~mask) | value;
	return adapter->WriteAdapter(adapter->capTMU + offset, current);
}


status_t
Router::_PostTime()
{
	Router* host = fDomain->HostRouter();
	if (IsHost() || !fUSB4 || host == NULL || host->fCapTMU == 0
		|| fCapTMU == 0)
		return B_OK;

	uint32 gmTime[3];
	status_t status = host->ReadConfig(host->fCapTMU + TMU_RTR_CS_1, gmTime,
		3);
	if (status != B_OK)
		return status;

	uint64 hi = TMU_RTR_CS_3_LOCAL_TIME_NS(gmTime[2]);
	uint64 mid = gmTime[1];
	uint64 lo = TMU_RTR_CS_1_LOCAL_TIME_NS(gmTime[0]);
	uint64 localTime = hi << 48 | mid << 16 | lo;

	status = _SetTimeDisruption(true);
	if (status != B_OK)
		return status;

	uint32 local[2] = { (uint32)localTime, (uint32)(localTime >> 32) };
	status = WriteConfig(fCapTMU + TMU_RTR_CS_22, local, 2);
	if (status == B_OK) {
		// the post time; writing its upper half starts the update
		uint32 post[2] = { 0x00000001, 0xffffffff };
		status = WriteConfig(fCapTMU + TMU_RTR_CS_24, post, 2);
	}
	if (status == B_OK)
		status = WriteConfig(fCapTMU + TMU_RTR_CS_25, (uint32)0);
	if (status == B_OK) {
		status = B_TIMED_OUT;
		for (int retry = 0; retry < 100; retry++) {
			uint32 post[2];
			if (ReadConfig(fCapTMU + TMU_RTR_CS_24, post, 2) != B_OK)
				break;
			if (post[0] == 0 && post[1] == 0) {
				status = B_OK;
				break;
			}
			snooze(10);
		}
	}

	_SetTimeDisruption(false);
	return status;
}


status_t
Router::DisableTMU()
{
	if (fTMUMode == TMU_MODE_OFF || fCapTMU == 0)
		return B_OK;

	if (!IsHost()) {
		Adapter* up = UpstreamAdapter();
		Adapter* down = DownstreamAdapter();
		_WriteTMURate(TMU_RATE_OFF);
		_SetAdapterTMU(up, TMU_ADP_CS_6, TMU_ADP_CS_6_DTS, TMU_ADP_CS_6_DTS);
		_SetAdapterTMU(down, TMU_ADP_CS_6, TMU_ADP_CS_6_DTS, TMU_ADP_CS_6_DTS);
		if ((fTMUMode == TMU_MODE_LOWRES || fTMUMode == TMU_MODE_HIFI_UNI)
			&& fTMUUnidirectional) {
			_SetAdapterTMU(up, TMU_ADP_CS_3, TMU_ADP_CS_3_UDM, 0);
			_SetAdapterTMU(down, TMU_ADP_CS_3, TMU_ADP_CS_3_UDM, 0);
		}
	} else
		_WriteTMURate(TMU_RATE_OFF);

	fTMUMode = TMU_MODE_OFF;
	return B_OK;
}


/*!	The host router runs its time management unit in the low resolution
	mode; device routers synchronize with their parent in the bidirectional
	high fidelity mode, since we do not use the link low power states.
*/
status_t
Router::EnableTMU()
{
	if (fCapTMU == 0)
		return B_OK;

	if (IsHost()) {
		// without the unidirectional mode the host router keeps whatever
		// the firmware set up
		if (!fTMUUnidirectional || fTMUMode == TMU_MODE_LOWRES)
			return B_OK;
		_SetTimeDisruption(true);
		status_t status = _WriteTMURate(TMU_RATE_LOWRES);
		_SetTimeDisruption(false);
		if (status == B_OK)
			fTMUMode = TMU_MODE_LOWRES;
		return status;
	}

	if (fTMUMode == TMU_MODE_HIFI_BI)
		return B_OK;

	status_t status = DisableTMU();
	if (status != B_OK)
		return status;
	_PostTime();

	Adapter* up = UpstreamAdapter();
	Adapter* down = DownstreamAdapter();

	_SetTimeDisruption(true);
	if (fTMUUnidirectional) {
		_SetAdapterTMU(up, TMU_ADP_CS_3, TMU_ADP_CS_3_UDM, 0);
		_SetAdapterTMU(down, TMU_ADP_CS_3, TMU_ADP_CS_3_UDM, 0);
	}
	status = _WriteTMURate(TMU_RATE_HIFI);
	if (status == B_OK)
		status = _SetAdapterTMU(up, TMU_ADP_CS_6, TMU_ADP_CS_6_DTS, 0);
	if (status == B_OK)
		status = _SetAdapterTMU(down, TMU_ADP_CS_6, TMU_ADP_CS_6_DTS, 0);
	_SetTimeDisruption(false);

	if (status == B_OK)
		fTMUMode = TMU_MODE_HIFI_BI;
	else {
		TRACE_ERROR("%#" B_PRIx64 ": enabling time sync failed: %s\n", fRoute,
			strerror(status));
		_WriteTMURate(TMU_RATE_OFF);
	}
	return status;
}
