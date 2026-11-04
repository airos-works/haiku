/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * The software connection manager of a USB4 domain.
 */


#include "Domain.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <util/AutoLock.h>

#include "usb4_debug.h"


#define DPRX_TIMEOUT		12000000
	// how long a DisplayPort tunnel may take for the graphics driver to
	// read the capabilities of the monitor


Domain::Domain(Nhi* nhi, int32 index, uint32 control)
	:
	fNhi(nhi),
	fIndex(index),
	fControl(control),
	fPCIeListener(NULL),
	fHost(NULL),
	fDPResourceCount(0),
	fPendingDPCount(0),
	fEventSem(-1),
	fThread(-1),
	fRunning(false),
	fStarted(false),
	fStartStatus(B_OK)
{
	mutex_init(&fLock, "usb4 domain");
	B_INITIALIZE_SPINLOCK(&fEventLock);
	memset(fRouters, 0, sizeof(fRouters));
	memset(fDPResources, 0, sizeof(fDPResources));
}


Domain::~Domain()
{
	Stop();
	mutex_destroy(&fLock);
}


status_t
Domain::Start()
{
	if (fRunning)
		return B_OK;

	fEventSem = create_sem(0, "usb4 domain events");
	if (fEventSem < 0)
		return fEventSem;

	fRunning = true;
	fThread = spawn_kernel_thread(_Thread, "usb4 connection manager",
		B_NORMAL_PRIORITY + 2, this);
	if (fThread < 0) {
		fRunning = false;
		delete_sem(fEventSem);
		fEventSem = -1;
		return fThread;
	}
	resume_thread(fThread);
	return B_OK;
}


void
Domain::Stop()
{
	if (fThread >= 0) {
		fRunning = false;
		release_sem(fEventSem);
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}

	if (fEventSem >= 0) {
		delete_sem(fEventSem);
		fEventSem = -1;
	}

	InterruptsSpinLocker locker(fEventLock);
	while (DomainEvent* event = fEvents.RemoveHead())
		free(event);
}


/*static*/ status_t
Domain::_Thread(void* data)
{
	((Domain*)data)->_Loop();
	return B_OK;
}


void
Domain::_Loop()
{
	{
		MutexLocker locker(fLock);
		fStartStatus = _StartDomain();
		fStarted = fStartStatus == B_OK;
		if (!fStarted) {
			TRACE_ERROR("domain %" B_PRId32 ": start failed: %s\n", fIndex,
				strerror(fStartStatus));
		}
	}

	while (fRunning) {
		bigtime_t timeout = fPendingDPCount > 0 ? 50000 : 1000000;
		acquire_sem_etc(fEventSem, 1, B_RELATIVE_TIMEOUT, timeout);
		if (!fRunning)
			break;

		while (true) {
			InterruptsSpinLocker eventLocker(fEventLock);
			DomainEvent* event = fEvents.RemoveHead();
			eventLocker.Unlock();
			if (event == NULL)
				break;

			if (fStarted) {
				MutexLocker locker(fLock);
				_HandleEvent(event);
			}
			free(event);
		}

		if (fStarted && fPendingDPCount > 0) {
			MutexLocker locker(fLock);
			_CheckPendingDP();
		}
	}

	MutexLocker locker(fLock);
	if (fStarted)
		_StopDomain();
	fStarted = false;
}


status_t
Domain::_StartDomain()
{
	if (fNhi->Version() >= NHI_CAPS_VERSION_2)
		fNhi->ResetHostRouter();

	status_t status = fNhi->StartControl();
	if (status != B_OK)
		return status;

	Router* host = new(std::nothrow) Router(this, NULL, 0);
	if (host == NULL)
		return B_NO_MEMORY;

	status = host->Probe();
	if (status == B_OK)
		status = host->Configure();
	if (status == B_OK)
		status = host->Setup();
	if (status != B_OK) {
		delete host;
		return status;
	}
	fHost = host;
	_AddRouter(host);

	TRACE_ALWAYS("domain %" B_PRId32 ": host router %04x:%04x, %s %"
		B_PRIu32 ", %u adapters, interrupts: %s\n", fIndex, host->VendorID(),
		host->DeviceID(), host->IsUSB4() ? "USB4 version" : "Thunderbolt",
		host->USB4Version(), host->MaxAdapter(), fNhi->InterruptMode());
	TRACE_ALWAYS("domain %" B_PRId32 ": tunneling USB3 %s, DisplayPort %s, "
		"PCIe %s\n", fIndex, MayTunnelUSB3() ? "yes" : "no",
		MayTunnelDP() ? "yes" : "no", MayTunnelPCIe() ? "yes" : "no");

	host->EnableTMU();

	// The boot firmware may have tunnels of its own. We cannot know whether
	// they suit us, so take everything down and let the devices come back
	// as hot plugs (USB4 version 2 host routers were reset already).
	if (host->IsUSB4() && host->USB4Version() == 1) {
		status = host->ResetHost();
		if (status != B_OK) {
			TRACE_ERROR("domain %" B_PRId32 ": resetting the host router "
				"failed: %s\n", fIndex, strerror(status));
		}
	}

	_AddDPResources(host);

	// pick up links that came up before hot plug events were on
	QueueRescan();
	return B_OK;
}


void
Domain::_StopDomain()
{
	// DisplayPort tunnels would keep the monitors lit with nobody to look
	// after them; the others can stay for the boot firmware or the next
	// connection manager to find
	while (Tunnel* tunnel = fTunnels.Head())
		_FreeTunnel(tunnel);

	for (int32 i = USB4_MAX_ROUTERS - 1; i >= 0; i--) {
		if (fRouters[i] != NULL && fRouters[i] != fHost) {
			delete fRouters[i];
			fRouters[i] = NULL;
		}
	}
	for (int32 i = 0; i < USB4_MAX_ROUTERS; i++) {
		if (fRouters[i] == fHost)
			fRouters[i] = NULL;
	}
	delete fHost;
	fHost = NULL;
	fDPResourceCount = 0;
	fPendingDPCount = 0;

	fNhi->StopControl();
}


// #pragma mark - configuration access


ControlResult
Domain::RawRead(uint64 route, uint8 adapter, usb4_config_space space,
	uint16 offset, uint32* buffer, size_t dwords)
{
	return fNhi->Read(route, adapter, space, offset, buffer, dwords);
}


static status_t
control_error(const ControlResult& result, usb4_config_space space,
	uint64 route, uint8 adapter, uint16 offset, const char* what)
{
	if (result.status == B_OK)
		return B_OK;

	if (result.status == B_DEV_NOT_READY) {
		switch (result.error) {
			case USB4_ERROR_INVALID_CONFIG_SPACE:
				if (space == USB4_CFG_ADAPTER)
					return B_ENTRY_NOT_FOUND;
				TRACE("%#" B_PRIx64 ":%u: invalid config space %d offset %#x"
					"\n", route, adapter, space, offset);
				return B_BAD_ADDRESS;
			case USB4_ERROR_PORT_NOT_CONNECTED:
				return B_DEV_NOT_READY;
			case USB4_ERROR_LOCK:
				TRACE("%#" B_PRIx64 ":%u: adapter is locked\n", route, adapter);
				return B_PERMISSION_DENIED;
			default:
				TRACE_ERROR("%s %#" B_PRIx64 ":%u space %d offset %#x: USB4 "
					"error %u (from %#" B_PRIx64 ":%u)\n", what, route, adapter,
					space, offset, result.error, result.route, result.port);
				return B_IO_ERROR;
		}
	}

	if (result.status == B_TIMED_OUT) {
		TRACE_ERROR("%s %#" B_PRIx64 ":%u space %d offset %#x timed out\n",
			what, route, adapter, space, offset);
	}
	return result.status;
}


status_t
Domain::ReadConfig(uint64 route, uint8 adapter, usb4_config_space space,
	uint16 offset, uint32* buffer, size_t dwords)
{
	ControlResult result = fNhi->Read(route, adapter, space, offset, buffer,
		dwords);
	return control_error(result, space, route, adapter, offset, "read");
}


status_t
Domain::WriteConfig(uint64 route, uint8 adapter, usb4_config_space space,
	uint16 offset, const uint32* buffer, size_t dwords)
{
	ControlResult result = fNhi->Write(route, adapter, space, offset, buffer,
		dwords);
	return control_error(result, space, route, adapter, offset, "write");
}


status_t
Domain::DeactivateHop(Adapter* adapter, uint32 hop, bool clearFlowControl)
{
	uint32 entry[2];
	status_t status = adapter->Read(USB4_CFG_HOPS, 2 * hop, entry, 2);
	if (status != B_OK)
		return status;
	if ((entry[0] & HOP_0_VALID) == 0)
		return B_OK;

	entry[0] &= ~HOP_0_VALID;
	status = adapter->Write(USB4_CFG_HOPS, 2 * hop, entry, 2);
	if (status != B_OK)
		return status;

	// wait until it has drained
	bigtime_t deadline = system_time() + 500000;
	do {
		status = adapter->Read(USB4_CFG_HOPS, 2 * hop, entry, 2);
		if (status != B_OK)
			return status;
		if ((entry[1] & HOP_1_PENDING) == 0) {
			if (!clearFlowControl)
				return B_OK;
			if (!adapter->router->IsUSB4())
				entry[1] &= ~(HOP_1_INGRESS_FC | HOP_1_INGRESS_SHARED);
			entry[1] &= ~(HOP_1_EGRESS_FC | HOP_1_EGRESS_SHARED);
			return adapter->Write(USB4_CFG_HOPS, 2 * hop, entry, 2);
		}
		snooze(20);
	} while (system_time() < deadline);

	return B_TIMED_OUT;
}


/*!	The buffer credits of a lane adapter that are left for PCIe (and DMA)
	paths once USB3 and as many DisplayPort streams as fit have theirs.
*/
uint32
Domain::AvailableCredits(Adapter* lane, size_t* maxDPStreams)
{
	Router* router = lane->router;
	int32 usb3 = MayTunnelUSB3() ? router->MaxUSB3Credits() : 0;
	int32 pcie = MayTunnelPCIe() ? router->MaxPCIeCredits() : 0;
	int32 spare = 0;
	if ((fControl & USB4_CONTROL_XDOMAIN) != 0) {
		spare = router->MaxDMACredits() != 0
			? min_c((int32)router->MaxDMACredits(), 14) : 14;
		spare += 1;
	}

	int32 credits = (int32)lane->totalCredits - (int32)lane->ctlCredits;
	int32 perStream = router->MinDPAuxCredits() + router->MinDPMainCredits();
	int32 streams = 0;
	if (MayTunnelDP() && perStream > 0)
		streams = max_c(0, (credits - (usb3 + pcie + spare)) / perStream);
	credits -= streams * perStream;
	credits -= usb3;

	if (maxDPStreams != NULL)
		*maxDPStreams = streams;
	return credits > 0 ? credits : 0;
}


// #pragma mark - events


void
Domain::QueueHotplug(uint64 route, uint8 adapter, bool unplug)
{
	DomainEvent* event = (DomainEvent*)malloc(sizeof(DomainEvent));
	if (event == NULL)
		return;
	new(event) DomainEvent;
	event->type = DomainEvent::HOTPLUG;
	event->route = route;
	event->adapter = adapter;
	event->unplug = unplug;
	event->code = 0;

	InterruptsSpinLocker locker(fEventLock);
	fEvents.Add(event);
	locker.Unlock();
	if (fEventSem >= 0)
		release_sem_etc(fEventSem, 1, B_DO_NOT_RESCHEDULE);
}


void
Domain::QueueNotification(uint64 route, uint8 adapter, uint8 code)
{
	DomainEvent* event = (DomainEvent*)malloc(sizeof(DomainEvent));
	if (event == NULL)
		return;
	new(event) DomainEvent;
	event->type = DomainEvent::NOTIFICATION;
	event->route = route;
	event->adapter = adapter;
	event->unplug = false;
	event->code = code;

	InterruptsSpinLocker locker(fEventLock);
	fEvents.Add(event);
	locker.Unlock();
	if (fEventSem >= 0)
		release_sem_etc(fEventSem, 1, B_DO_NOT_RESCHEDULE);
}


void
Domain::SetControl(uint32 control)
{
	fControl = control;
	QueueRescan();
}


void
Domain::QueueRescan()
{
	DomainEvent* event = (DomainEvent*)malloc(sizeof(DomainEvent));
	if (event == NULL)
		return;
	new(event) DomainEvent;
	event->type = DomainEvent::RESCAN;
	event->route = 0;
	event->adapter = 0;
	event->unplug = false;
	event->code = 0;

	InterruptsSpinLocker locker(fEventLock);
	fEvents.Add(event);
	locker.Unlock();
	if (fEventSem >= 0)
		release_sem_etc(fEventSem, 1, B_DO_NOT_RESCHEDULE);
}


void
Domain::_HandleEvent(DomainEvent* event)
{
	switch (event->type) {
		case DomainEvent::HOTPLUG:
			TRACE_ALWAYS("domain %" B_PRId32 ": %s on %#" B_PRIx64 ":%u\n",
				fIndex, event->unplug ? "unplug" : "plug", event->route,
				event->adapter);
			_HandleHotplug(event->route, event->adapter, event->unplug);
			break;

		case DomainEvent::NOTIFICATION:
			TRACE("domain %" B_PRId32 ": notification %u from %#" B_PRIx64
				":%u\n", fIndex, event->code, event->route, event->adapter);
			break;

		case DomainEvent::RESCAN:
		{
			// scan every router, and set up what tunnels are missing (the
			// allowed kinds may have changed)
			Router* routers[USB4_MAX_ROUTERS];
			int32 count = 0;
			for (int32 i = 0; i < USB4_MAX_ROUTERS; i++) {
				if (fRouters[i] != NULL && !fRouters[i]->IsUnplugged())
					routers[count++] = fRouters[i];
			}
			for (int32 i = 0; i < count; i++) {
				if (!routers[i]->IsHost()) {
					_TunnelUSB3(routers[i]);
					_TunnelPCIe(routers[i]);
				}
			}
			for (int32 i = 0; i < count; i++)
				_ScanRouter(routers[i]);
			_TunnelDP();
			break;
		}
	}
}


void
Domain::_HandleHotplug(uint64 route, uint8 number, bool unplug)
{
	Router* router = _FindRouter(route);
	if (router == NULL) {
		TRACE("hot plug event from unknown router %#" B_PRIx64 "\n", route);
		return;
	}
	Adapter* adapter = router->AdapterAt(number);
	if (adapter == NULL || !adapter->present) {
		TRACE("hot plug event from unknown adapter %#" B_PRIx64 ":%u\n", route,
			number);
		return;
	}
	if (adapter->IsUpstream())
		return;

	if (unplug) {
		if (adapter->remote != NULL && adapter->IsLane())
			_Unplug(adapter);
		else if (adapter->IsDPIn() || adapter->IsDPOut())
			_DPResourceUnavailable(adapter, "unplugged");
		else if (number == 0)
			TRACE("%#" B_PRIx64 ": xHCI disconnect request\n", route);
		return;
	}

	if (adapter->remote != NULL)
		return;
	if (number == 0) {
		TRACE("%#" B_PRIx64 ": xHCI connect request\n", route);
		return;
	}
	if (adapter->IsLane())
		_ScanAdapter(adapter);
	else if (adapter->IsDPIn() || adapter->IsDPOut())
		_DPResourceAvailable(adapter);
}


// #pragma mark - routers


Router*
Domain::_FindRouter(uint64 route)
{
	for (int32 i = 0; i < USB4_MAX_ROUTERS; i++) {
		Router* router = fRouters[i];
		if (router != NULL && router->Route() == route
			&& !router->IsUnplugged())
			return router;
	}
	return NULL;
}


status_t
Domain::_AddRouter(Router* router)
{
	for (int32 i = 0; i < USB4_MAX_ROUTERS; i++) {
		if (fRouters[i] == NULL) {
			fRouters[i] = router;
			return B_OK;
		}
	}
	return B_NO_MEMORY;
}


//! Removes and deletes the router and everything below it.
void
Domain::_RemoveRouter(Router* router)
{
	for (int32 i = 0; i < USB4_MAX_ROUTERS; i++) {
		if (fRouters[i] != NULL && fRouters[i]->Parent() == router)
			_RemoveRouter(fRouters[i]);
	}
	for (int32 i = 0; i < USB4_MAX_ROUTERS; i++) {
		if (fRouters[i] == router)
			fRouters[i] = NULL;
	}

	// whatever pointed at it from the parent
	for (uint8 i = 1; router->Parent() != NULL
			&& i <= router->Parent()->MaxAdapter(); i++) {
		Adapter* adapter = router->Parent()->AdapterAt(i);
		if (adapter->remote != NULL && adapter->remote->router == router)
			adapter->remote = NULL;
	}
	delete router;
}


void
Domain::_ScanRouter(Router* router)
{
	for (uint8 i = 1; i <= router->MaxAdapter(); i++) {
		Adapter* adapter = router->AdapterAt(i);
		if (adapter->present)
			_ScanAdapter(adapter);
	}
}


void
Domain::_ScanAdapter(Adapter* adapter)
{
	if (adapter->IsUpstream())
		return;

	if (adapter->IsDPOut()) {
		uint32 value;
		if (adapter->capAdapter != 0
			&& adapter->ReadAdapter(adapter->capAdapter + ADP_DP_CS_2, value)
				== B_OK
			&& (value & ADP_DP_CS_2_HPD) != 0 && !adapter->IsEnabled()) {
			TRACE("%#" B_PRIx64 ":%u: DP OUT has a monitor\n",
				adapter->router->Route(), adapter->number);
			_DPResourceAvailable(adapter);
		}
		return;
	}

	if (!adapter->IsLane())
		return;
	// routers with two links are scanned through the first
	if (adapter->dualLink != NULL && adapter->linkNumber != 0)
		return;

	int32 state = adapter->State();
	for (int retry = 0; retry < 10; retry++) {
		if (state < 0 || state == ADAPTER_STATE_DISABLED
			|| state == ADAPTER_STATE_CLD)
			return;
		if (state >= ADAPTER_STATE_CL0 && state <= ADAPTER_STATE_CL2)
			break;
		snooze(100000);
		state = adapter->State();
	}
	if (state < ADAPTER_STATE_CL0 || state > ADAPTER_STATE_CL2)
		return;
	if (adapter->remote != NULL)
		return;

	Router* parent = adapter->router;
	uint64 route = parent->Route()
		| ((uint64)adapter->number << (parent->Depth() * 8));

	parent->UnlockAdapter(adapter);

	Router* router = new(std::nothrow) Router(this, parent, route);
	if (router == NULL)
		return;

	status_t status = router->Probe();
	if (status == B_OK)
		status = router->Configure();
	if (status == B_OK)
		status = router->Setup();
	if (status == B_OK)
		status = _AddRouter(router);
	if (status != B_OK) {
		TRACE_ERROR("%#" B_PRIx64 ": router setup failed: %s\n", route,
			strerror(status));
		delete router;
		return;
	}

	status = router->ConfigureLink();
	if (status != B_OK) {
		TRACE_ERROR("%#" B_PRIx64 ": link setup failed: %s\n", route,
			strerror(status));
	}

	TRACE_ALWAYS("domain %" B_PRId32 ": %#" B_PRIx64 ": %s %s (%04x:%04x, "
		"%s), %" B_PRId32 " Gb/s x %" B_PRId32 ", %s link\n", fIndex, route,
		router->VendorName()[0] != '\0' ? router->VendorName() : "router",
		router->DeviceName(), router->VendorID(), router->DeviceID(),
		router->IsUSB4() ? "USB4" : "Thunderbolt", router->LinkSpeed(),
		router->LinkWidth() >= LINK_WIDTH_DUAL ? 2 : 1,
		router->LinkUSB4() ? "USB4" : "Thunderbolt");

	if (router->EnableTMU() != B_OK)
		TRACE_ERROR("%#" B_PRIx64 ": time sync failed\n", route);

	status = router->SetConfigurationValid();
	if (status != B_OK) {
		TRACE_ERROR("%#" B_PRIx64 ": configuration not accepted: %s\n", route,
			strerror(status));
	}

	_TunnelUSB3(router);
	_TunnelPCIe(router);
	_AddDPResources(router);
	_ScanRouter(router);
}


void
Domain::_Unplug(Adapter* adapter)
{
	Router* router = adapter->remote->router;
	TRACE_ALWAYS("domain %" B_PRId32 ": %#" B_PRIx64 " unplugged\n", fIndex,
		router->Route());

	router->SetUnplugged();
	bool hadPCIe = _FindTunnel(TUNNEL_PCIE, NULL, NULL) != NULL;
	_FreeInvalidTunnels();
	_RemoveDPResources(router);
	router->UnconfigureLink();
	_RemoveRouter(router);

	adapter->remote = NULL;
	if (adapter->dualLink != NULL)
		adapter->dualLink->remote = NULL;

	if (hadPCIe && fPCIeListener != NULL)
		fPCIeListener->PCIeTunnelChanged(this, false);

	// a DP IN might be free for another monitor now
	_TunnelDP();
}


// #pragma mark - USB3 and PCIe


void
Domain::_TunnelUSB3(Router* router)
{
	if (!MayTunnelUSB3())
		return;

	Adapter* up = router->FindAdapter(ADAPTER_USB3_UP);
	if (up == NULL || !router->LinkUSB4()
		|| _FindTunnel(TUNNEL_USB3, NULL, up) != NULL)
		return;

	Router* parent = router->Parent();
	Adapter* down = parent->MapProtocolDown(router->DownstreamAdapter(),
		ADAPTER_USB3_DOWN);
	if (down == NULL)
		return;

	if (!parent->IsHost()) {
		// the chain above must be complete
		Adapter* parentUp = parent->FindAdapter(ADAPTER_USB3_UP);
		if (parentUp == NULL || !parentUp->IsEnabled())
			return;
		_ReleaseUSB3Bandwidth(down, up);
	}

	int32 availableUp, availableDown;
	_AvailableBandwidth(down, up, availableUp, availableDown);
	TRACE("%#" B_PRIx64 ": %" B_PRId32 "/%" B_PRId32 " Mb/s for USB3\n",
		router->Route(), availableUp, availableDown);

	Tunnel* tunnel = Tunnel::CreateUSB3(this, up, down, availableUp,
		availableDown);
	if (tunnel == NULL) {
		TRACE_ERROR("%#" B_PRIx64 ": no USB3 tunnel\n", router->Route());
		if (!parent->IsHost())
			_ReclaimUSB3Bandwidth(down, up);
		return;
	}
	if (tunnel->Activate() != B_OK) {
		delete tunnel;
		if (!parent->IsHost())
			_ReclaimUSB3Bandwidth(down, up);
		return;
	}
	fTunnels.Add(tunnel);
	TRACE_ALWAYS("domain %" B_PRId32 ": USB3 tunnel %#" B_PRIx64 ":%u -> %#"
		B_PRIx64 ":%u\n", fIndex, down->router->Route(), down->number,
		up->router->Route(), up->number);

	if (!parent->IsHost())
		_ReclaimUSB3Bandwidth(down, up);
}


void
Domain::_TunnelPCIe(Router* router)
{
	if (!MayTunnelPCIe())
		return;

	Adapter* up = router->FindAdapter(ADAPTER_PCIE_UP);
	if (up == NULL || _FindTunnel(TUNNEL_PCIE, NULL, up) != NULL)
		return;

	Router* parent = router->Parent();
	if (!parent->IsHost()) {
		Adapter* parentUp = parent->FindAdapter(ADAPTER_PCIE_UP);
		if (parentUp == NULL || !parentUp->IsEnabled())
			return;
	}
	Adapter* down = parent->MapProtocolDown(router->DownstreamAdapter(),
		ADAPTER_PCIE_DOWN);
	if (down == NULL)
		return;

	Tunnel* tunnel = Tunnel::CreatePCIe(this, up, down);
	if (tunnel == NULL) {
		TRACE_ERROR("%#" B_PRIx64 ": no PCIe tunnel\n", router->Route());
		return;
	}
	if (tunnel->Activate() != B_OK) {
		delete tunnel;
		return;
	}
	fTunnels.Add(tunnel);
	TRACE_ALWAYS("domain %" B_PRId32 ": PCIe tunnel %#" B_PRIx64 ":%u -> %#"
		B_PRIx64 ":%u\n", fIndex, down->router->Route(), down->number,
		up->router->Route(), up->number);

	if (fPCIeListener != NULL)
		fPCIeListener->PCIeTunnelChanged(this, true);
}


// #pragma mark - DisplayPort


void
Domain::_AddDPResources(Router* router)
{
	for (uint8 i = 1; i <= router->MaxAdapter(); i++) {
		Adapter* adapter = router->AdapterAt(i);
		if (!adapter->present || !adapter->IsDPIn() || adapter->dpResource)
			continue;
		if (!router->QueryDPResource(adapter))
			continue;
		if (fDPResourceCount >= USB4_MAX_DP_RESOURCES)
			return;

		// DP IN adapters of device routers (external graphics) come first
		if (!router->IsHost()) {
			memmove(fDPResources + 1, fDPResources,
				fDPResourceCount * sizeof(Adapter*));
			fDPResources[0] = adapter;
		} else
			fDPResources[fDPResourceCount] = adapter;
		fDPResourceCount++;
		adapter->dpResource = true;
		TRACE("%#" B_PRIx64 ":%u: DP IN available\n", router->Route(), i);
	}
}


static bool
router_below(Router* router, Router* top)
{
	while (router != NULL) {
		if (router == top)
			return true;
		router = router->Parent();
	}
	return false;
}


void
Domain::_RemoveDPResources(Router* router)
{
	for (int32 i = 0; i < fDPResourceCount; i++) {
		if (router_below(fDPResources[i]->router, router)) {
			fDPResources[i]->dpResource = false;
			memmove(fDPResources + i, fDPResources + i + 1,
				(fDPResourceCount - i - 1) * sizeof(Adapter*));
			fDPResourceCount--;
			i--;
		}
	}
}


void
Domain::_DPResourceAvailable(Adapter* adapter)
{
	if (adapter->dpResource || adapter->IsEnabled())
		return;
	if (fDPResourceCount >= USB4_MAX_DP_RESOURCES)
		return;

	fDPResources[fDPResourceCount++] = adapter;
	adapter->dpResource = true;
	TRACE("%#" B_PRIx64 ":%u: %s available\n", adapter->router->Route(),
		adapter->number, adapter->TypeName());
	_TunnelDP();
}


void
Domain::_DPResourceUnavailable(Adapter* adapter, const char* reason)
{
	TRACE("%#" B_PRIx64 ":%u: %s unavailable: %s\n", adapter->router->Route(),
		adapter->number, adapter->TypeName(), reason);

	Tunnel* tunnel = adapter->IsDPIn()
		? _FindTunnel(TUNNEL_DP, adapter, NULL)
		: _FindTunnel(TUNNEL_DP, NULL, adapter);
	if (tunnel != NULL)
		_FreeTunnel(tunnel);

	for (int32 i = 0; i < fDPResourceCount; i++) {
		if (fDPResources[i] == adapter) {
			memmove(fDPResources + i, fDPResources + i + 1,
				(fDPResourceCount - i - 1) * sizeof(Adapter*));
			fDPResourceCount--;
			break;
		}
	}
	adapter->dpResource = false;

	_TunnelDP();
}


Adapter*
Domain::_FindDPOut(Adapter* in)
{
	Adapter* hostLane = in->router->IsHost() ? NULL
		: fHost->AdapterTowards(in->router->Route());

	for (int32 i = 0; i < fDPResourceCount; i++) {
		Adapter* out = fDPResources[i];
		if (!out->IsDPOut())
			continue;
		if (_FindTunnel(TUNNEL_DP, NULL, out) != NULL || out->IsEnabled())
			continue;
		if (out->router == in->router)
			continue;
		// keep a tunnel below one USB4 port of the host
		if (hostLane != NULL && !out->router->IsHost()
			&& fHost->AdapterTowards(out->router->Route()) != hostLane)
			continue;
		return out;
	}
	return NULL;
}


void
Domain::_TunnelDP()
{
	if (!MayTunnelDP())
		return;

	for (int32 i = 0; i < fDPResourceCount; i++) {
		Adapter* in = fDPResources[i];
		if (!in->IsDPIn())
			continue;
		if (_FindTunnel(TUNNEL_DP, in, NULL) != NULL || in->IsEnabled())
			continue;

		Adapter* out = _FindDPOut(in);
		if (out == NULL)
			continue;
		_TunnelOneDP(in, out);
	}
}


void
Domain::_TunnelOneDP(Adapter* in, Adapter* out)
{
	if (fPendingDPCount >= USB4_MAX_DP_RESOURCES)
		return;

	if (in->router->AllocateDPResource(in) != B_OK) {
		TRACE("%#" B_PRIx64 ":%u: DP IN is busy\n", in->router->Route(),
			in->number);
		return;
	}

	_ReleaseUSB3Bandwidth(in, out);

	int32 up, down;
	_AvailableBandwidth(in, out, up, down);
	TRACE("%" B_PRId32 "/%" B_PRId32 " Mb/s for DisplayPort\n", up, down);

	Tunnel* tunnel = Tunnel::CreateDP(this, in, out, up, down);
	status_t status = tunnel != NULL ? tunnel->Activate() : B_NO_MEMORY;
	if (status != B_OK) {
		TRACE_ERROR("DisplayPort tunnel %#" B_PRIx64 ":%u -> %#" B_PRIx64
			":%u failed: %s\n", in->router->Route(), in->number,
			out->router->Route(), out->number, strerror(status));
		delete tunnel;
		_ReclaimUSB3Bandwidth(in, out);
		in->router->DeallocateDPResource(in);
		return;
	}

	fTunnels.Add(tunnel);
	fPendingDP[fPendingDPCount].tunnel = tunnel;
	fPendingDP[fPendingDPCount].deadline = system_time() + DPRX_TIMEOUT;
	fPendingDPCount++;

	TRACE_ALWAYS("domain %" B_PRId32 ": DisplayPort tunnel %#" B_PRIx64
		":%u -> %#" B_PRIx64 ":%u, %" B_PRIu32 " Mb/s x%" B_PRIu32 "\n",
		fIndex, in->router->Route(), in->number, out->router->Route(),
		out->number, tunnel->DPRate(), tunnel->DPLanes());
}


/*!	A DisplayPort tunnel is complete once the graphics driver has read the
	capabilities of the monitor through it. Until then it holds as much
	bandwidth as it might need; afterwards the rest goes back to USB3.
*/
void
Domain::_CheckPendingDP()
{
	for (int32 i = 0; i < fPendingDPCount; i++) {
		Tunnel* tunnel = fPendingDP[i].tunnel;
		bool done = tunnel->DPRXDone();
		bool expired = !done && system_time() > fPendingDP[i].deadline;
		if (!done && !expired)
			continue;

		memmove(fPendingDP + i, fPendingDP + i + 1,
			(fPendingDPCount - i - 1) * sizeof(PendingDP));
		fPendingDPCount--;
		i--;

		if (done) {
			int32 up, down;
			tunnel->ConsumedBandwidth(up, down);
			TRACE_ALWAYS("domain %" B_PRId32 ": DisplayPort tunnel to %#"
				B_PRIx64 ":%u is up, %" B_PRId32 " Mb/s\n", fIndex,
				tunnel->Destination()->router->Route(),
				tunnel->Destination()->number, max_c(up, down));
			_ReclaimUSB3Bandwidth(tunnel->Source(), tunnel->Destination());
		} else {
			// no graphics driver took it: leave the DP IN alone until it
			// is plugged again
			TRACE_ALWAYS("domain %" B_PRId32 ": DisplayPort tunnel to %#"
				B_PRIx64 ":%u: the monitor was not read, giving up\n", fIndex,
				tunnel->Destination()->router->Route(),
				tunnel->Destination()->number);
			_DPResourceUnavailable(tunnel->Source(), "DPRX not done");
		}
	}
}


// #pragma mark - tunnels


Tunnel*
Domain::_FindTunnel(tunnel_type type, Adapter* source, Adapter* destination)
{
	for (TunnelList::Iterator it = fTunnels.GetIterator();
			Tunnel* tunnel = it.Next();) {
		if (tunnel->Type() != type)
			continue;
		if ((source == NULL || tunnel->Source() == source)
			&& (destination == NULL || tunnel->Destination() == destination))
			return tunnel;
	}
	return NULL;
}


void
Domain::_FreeTunnel(Tunnel* tunnel)
{
	for (int32 i = 0; i < fPendingDPCount; i++) {
		if (fPendingDP[i].tunnel == tunnel) {
			memmove(fPendingDP + i, fPendingDP + i + 1,
				(fPendingDPCount - i - 1) * sizeof(PendingDP));
			fPendingDPCount--;
			break;
		}
	}

	fTunnels.Remove(tunnel);
	Adapter* source = tunnel->Source();
	Adapter* destination = tunnel->Destination();
	tunnel_type type = tunnel->Type();
	bool invalid = tunnel->IsInvalid();

	tunnel->Deactivate();
	delete tunnel;

	if (type == TUNNEL_DP) {
		if (!source->router->IsUnplugged())
			source->router->DeallocateDPResource(source);
		if (!invalid)
			_ReclaimUSB3Bandwidth(source, destination);
	}
}


void
Domain::_FreeInvalidTunnels()
{
	TunnelList::Iterator it = fTunnels.GetIterator();
	while (Tunnel* tunnel = it.Next()) {
		if (tunnel->IsInvalid()) {
			// the iterator would point into freed memory
			_FreeTunnel(tunnel);
			it = fTunnels.GetIterator();
		}
	}
}


// #pragma mark - bandwidth


/*!	Collects the downstream facing lane adapter of every link between the
	two adapters.
*/
int32
Domain::_CollectLinks(Adapter* source, Adapter* destination, Adapter** links,
	int32 maxLinks)
{
	Router* a = source->router;
	Router* b = destination->router;

	// the first router both hang below
	Router* top = b;
	while (top != NULL && !router_below(a, top))
		top = top->Parent();

	// every router below it on either side hangs off one link
	int32 count = 0;
	for (Router* router = a; router != top && router != NULL
			&& count < maxLinks; router = router->Parent()) {
		if (Adapter* lane = router->DownstreamAdapter())
			links[count++] = lane;
	}
	for (Router* router = b; router != top && router != NULL
			&& count < maxLinks; router = router->Parent()) {
		if (Adapter* lane = router->DownstreamAdapter())
			links[count++] = lane;
	}
	return count;
}


status_t
Domain::_AvailableBandwidth(Adapter* source, Adapter* destination,
	int32& availableUp, int32& availableDown)
{
	availableUp = availableDown = 120000;

	Adapter* links[16];
	int32 count = _CollectLinks(source, destination, links, 16);
	bool forUSB3 = source->IsUSB3Down() || destination->IsUSB3Up();

	for (int32 i = 0; i < count; i++) {
		Adapter* lane = links[i];
		Router* child = lane->remote != NULL ? lane->remote->router : NULL;
		if (child == NULL)
			continue;

		int32 width = child->LinkWidth() >= LINK_WIDTH_DUAL ? 2 : 1;
		int32 max = child->LinkSpeed() * width * 1000;
		// leave a 10% guard band
		max -= max / 10;
		int32 maxUp = max, maxDown = max;

		for (TunnelList::Iterator it = fTunnels.GetIterator();
				Tunnel* tunnel = it.Next();) {
			if (tunnel->IsInvalid() || !tunnel->Crosses(lane))
				continue;
			if (tunnel->Source() == source
				&& tunnel->Destination() == destination)
				continue;
			if (tunnel->Type() == TUNNEL_USB3 && forUSB3)
				continue;

			int32 up, down;
			if (tunnel->ConsumedBandwidth(up, down) != B_OK)
				continue;
			maxUp -= up;
			maxDown -= down;
		}

		availableUp = min_c(availableUp, maxUp);
		availableDown = min_c(availableDown, maxDown);
	}

	availableUp = max_c(availableUp, 0);
	availableDown = max_c(availableDown, 0);
	return B_OK;
}


//! The USB3 tunnel from the host router towards the deeper end.
Tunnel*
Domain::_FirstUSB3Tunnel(Adapter* source, Adapter* destination)
{
	Router* router = source->router->Depth() > destination->router->Depth()
		? source->router : destination->router;
	if (router->IsHost())
		return NULL;

	Adapter* hostLane = fHost->AdapterTowards(router->Route());
	for (TunnelList::Iterator it = fTunnels.GetIterator();
			Tunnel* tunnel = it.Next();) {
		if (tunnel->Type() == TUNNEL_USB3 && tunnel->Source()->router->IsHost()
			&& !tunnel->IsInvalid() && tunnel->Crosses(hostLane))
			return tunnel;
	}
	return NULL;
}


void
Domain::_ReleaseUSB3Bandwidth(Adapter* source, Adapter* destination)
{
	Tunnel* tunnel = _FirstUSB3Tunnel(source, destination);
	if (tunnel != NULL)
		tunnel->ReleaseUnusedBandwidth();
}


void
Domain::_ReclaimUSB3Bandwidth(Adapter* source, Adapter* destination)
{
	Tunnel* tunnel = _FirstUSB3Tunnel(source, destination);
	if (tunnel == NULL)
		return;

	int32 up, down;
	if (_AvailableBandwidth(tunnel->Source(), tunnel->Destination(), up, down)
			!= B_OK)
		return;
	tunnel->ReclaimAvailableBandwidth(up, down);
}


// #pragma mark - diagnostics


static const char*
lane_state_name(int32 state)
{
	switch (state) {
		case ADAPTER_STATE_DISABLED:
			return "disabled";
		case ADAPTER_STATE_TRAINING:
			return "training";
		case ADAPTER_STATE_CL0:
			return "up";
		case ADAPTER_STATE_CL0S_TX:
		case ADAPTER_STATE_CL0S_RX:
			return "up (CL0s)";
		case ADAPTER_STATE_CL1:
			return "up (CL1)";
		case ADAPTER_STATE_CL2:
			return "up (CL2)";
		case ADAPTER_STATE_CLD:
			return "unplugged";
		default:
			return "?";
	}
}


size_t
Domain::Dump(char* buffer, size_t size)
{
	size_t length = 0;
#define PRINT(format, args...) \
	do { \
		if (length < size) { \
			int written = snprintf(buffer + length, size - length, format, \
				##args); \
			if (written > 0) \
				length += min_c((size_t)written, size - length); \
		} \
	} while (false)

	const pci_info& info = fNhi->Info();
	PRINT("domain %" B_PRId32 ": host interface %04x:%04x at %02x:%02x.%x, "
		"%s interrupts (%" B_PRIu32 "), %" B_PRIu32 " paths\n", fIndex,
		info.vendor_id, info.device_id, info.bus, info.device, info.function,
		fNhi->InterruptMode(), fNhi->InterruptCount(), fNhi->HopCount());
	PRINT("  tunneling: USB3 %s, DisplayPort %s, PCIe %s\n",
		MayTunnelUSB3() ? "yes" : "no", MayTunnelDP() ? "yes" : "no",
		MayTunnelPCIe() ? "yes" : "no");
	if (!fStarted) {
		PRINT("  not running: %s\n", strerror(fStartStatus));
		return length;
	}

	MutexLocker locker(fLock);

	for (int32 i = 0; i < USB4_MAX_ROUTERS; i++) {
		Router* router = fRouters[i];
		if (router == NULL)
			continue;
		PRINT("\nrouter %#" B_PRIx64 ": %04x:%04x %s%s%s, %s", router->Route(),
			router->VendorID(), router->DeviceID(), router->VendorName(),
			router->VendorName()[0] != '\0' ? " " : "", router->DeviceName(),
			router->IsUSB4() ? "USB4" : "Thunderbolt");
		if (!router->IsHost()) {
			PRINT(", %" B_PRId32 " Gb/s x %d, %s link, upstream %u",
				router->LinkSpeed(),
				router->LinkWidth() >= LINK_WIDTH_DUAL ? 2 : 1,
				router->LinkUSB4() ? "USB4" : "Thunderbolt",
				router->UpstreamAdapterNumber());
		}
		PRINT(", TMU %s\n", router->TMUMode() == TMU_MODE_OFF ? "off"
			: router->TMUMode() == TMU_MODE_HIFI_BI ? "bidirectional"
			: router->TMUMode() == TMU_MODE_LOWRES ? "low resolution"
			: "unidirectional");
		if (router->UID() != 0)
			PRINT("  uid %#" B_PRIx64 "\n", router->UID());

		for (uint8 j = 1; j <= router->MaxAdapter(); j++) {
			Adapter* adapter = router->AdapterAt(j);
			if (!adapter->present || adapter->type == ADAPTER_INACTIVE)
				continue;
			PRINT("  %2u %-14s", j, adapter->TypeName());
			if (adapter->IsLane()) {
				int32 state = adapter->State();
				PRINT(" %-10s", lane_state_name(state));
				if (state >= ADAPTER_STATE_CL0 && state <= ADAPTER_STATE_CL2) {
					PRINT(" %" B_PRId32 " Gb/s, width %" B_PRId32,
						adapter->LinkSpeed(), adapter->LinkWidth());
				}
				PRINT(" credits %u", adapter->totalCredits);
				if (adapter->bonded)
					PRINT(" bonded");
				if (adapter->IsUpstream())
					PRINT(" (upstream)");
				else if (adapter->remote != NULL)
					PRINT(" -> %#" B_PRIx64, adapter->remote->router->Route());
			} else if (adapter->IsDPOut() || adapter->IsDPIn()) {
				uint32 cs2 = 0;
				if (adapter->capAdapter != 0)
					adapter->ReadAdapter(adapter->capAdapter + ADP_DP_CS_2, cs2);
				PRINT(" %s%s%s", adapter->IsEnabled() ? "enabled" : "idle",
					(cs2 & ADP_DP_CS_2_HPD) != 0 ? ", HPD" : "",
					adapter->dpResource ? ", resource" : "");
			} else if (adapter->capAdapter != 0) {
				PRINT(" %s", adapter->IsEnabled() ? "enabled" : "idle");
			}
			PRINT("\n");
		}
	}

	PRINT("\ntunnels:\n");
	for (TunnelList::Iterator it = fTunnels.GetIterator();
			Tunnel* tunnel = it.Next();) {
		int32 up = 0, down = 0;
		tunnel->ConsumedBandwidth(up, down);
		PRINT("  %-11s %#" B_PRIx64 ":%u -> %#" B_PRIx64 ":%u, %s",
			tunnel->TypeName(), tunnel->Source()->router->Route(),
			tunnel->Source()->number, tunnel->Destination()->router->Route(),
			tunnel->Destination()->number,
			tunnel->IsActive() ? "active" : "inactive");
		if (tunnel->Type() == TUNNEL_DP) {
			PRINT(", %" B_PRIu32 " Mb/s x%" B_PRIu32 ", %s",
				tunnel->DPRate(), tunnel->DPLanes(),
				tunnel->DPRXDone() ? "monitor read" : "waiting for the "
					"graphics driver");
		}
		if (up != 0 || down != 0)
			PRINT(", %" B_PRId32 "/%" B_PRId32 " Mb/s", up, down);
		PRINT("\n");
	}

	return length;
#undef PRINT
}
