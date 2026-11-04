/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Paths and the PCIe, USB3 and DisplayPort tunnels built from them (USB4
 * specification chapter 10 and the tunneling chapters).
 */


#include "Tunnel.h"

#include <KernelExport.h>
#include <new>
#include <string.h>

#include "Domain.h"
#include "usb4_debug.h"


#define PCIE_HOP			8
#define USB3_HOP			8
#define DP_AUX_TX_HOP		8
#define DP_AUX_RX_HOP		8
#define DP_VIDEO_HOP		9

#define PCIE_PRIORITY		3
#define PCIE_WEIGHT			1
#define USB3_PRIORITY		3
#define USB3_WEIGHT			2
#define DP_VIDEO_PRIORITY	1
#define DP_VIDEO_WEIGHT		1
#define DP_AUX_PRIORITY		2
#define DP_AUX_WEIGHT		1

#define MIN_PCIE_CREDITS	6


uint32
dp_bandwidth(uint32 rate, uint32 lanes)
{
	// tunneling drops the 8b/10b (or 128b/132b) line coding
	if (rate >= 10000)
		return rate * lanes * 128 / 132;
	return rate * lanes * 8 / 10;
}


static uint32
dp_cap_rate(uint32 cap)
{
	switch (DP_CAP_RATE(cap)) {
		case 0:
			return 1620;
		case 1:
			return 2700;
		case 2:
			return 5400;
		case 3:
			return 8100;
		default:
			return 0;
	}
}


static uint32
dp_cap_lanes(uint32 cap)
{
	switch (DP_CAP_LANES(cap)) {
		case 0:
			return 1;
		case 1:
			return 2;
		case 2:
			return 4;
		default:
			return 0;
	}
}


static uint32
dp_cap_set(uint32 cap, uint32 rate, uint32 lanes)
{
	uint32 rateCode;
	switch (rate) {
		case 2700:
			rateCode = 1;
			break;
		case 5400:
			rateCode = 2;
			break;
		case 8100:
			rateCode = 3;
			break;
		default:
			rateCode = 0;
			break;
	}
	uint32 laneCode = lanes == 4 ? 2 : lanes == 2 ? 1 : 0;
	cap &= ~(DP_CAP_RATE_MASK | DP_CAP_LANES_MASK);
	return cap | (rateCode << 8) | (laneCode << 12);
}


static status_t
dp_reduce_bandwidth(int32 maxBandwidth, uint32 inRate, uint32 inLanes,
	uint32 outRate, uint32 outLanes, uint32& newRate, uint32& newLanes)
{
	static const uint32 kModes[][2] = {
		{ 8100, 4 }, { 5400, 4 }, { 8100, 2 }, { 2700, 4 }, { 5400, 2 },
		{ 8100, 1 }, { 1620, 4 }, { 5400, 1 }, { 2700, 2 }, { 1620, 2 },
		{ 2700, 1 }, { 1620, 1 },
	};

	for (size_t i = 0; i < B_COUNT_OF(kModes); i++) {
		if (kModes[i][0] > outRate || kModes[i][1] > outLanes
			|| kModes[i][0] > inRate || kModes[i][1] > inLanes)
			continue;
		if ((int32)dp_bandwidth(kModes[i][0], kModes[i][1]) <= maxBandwidth) {
			newRate = kModes[i][0];
			newLanes = kModes[i][1];
			return B_OK;
		}
	}
	return B_DEVICE_FULL;
}


static bool
uses_credit_allocation(Adapter* adapter)
{
	return adapter->IsLane() && adapter->router->CreditAllocation();
}


// #pragma mark - Path


Path::Path(Domain* domain, const char* name)
	:
	priority(0),
	weight(0),
	ingressFlowControl(PATH_NONE),
	egressFlowControl(PATH_NONE),
	ingressShared(PATH_NONE),
	egressShared(PATH_NONE),
	dropPackets(false),
	clearFlowControl(false),
	fDomain(domain),
	fName(name),
	fLength(0),
	fActive(false)
{
	memset(fHops, 0, sizeof(fHops));
}


Path::~Path()
{
	if (fActive)
		Deactivate();

	for (int32 i = 0; i < fLength; i++) {
		PathHop& hop = fHops[i];
		if (hop.in != NULL && hop.inHop >= 0)
			hop.in->inHops.Release(hop.inHop);
		if (hop.out != NULL && hop.nextHop >= 0)
			hop.out->outHops.Release(hop.nextHop);
	}
}


/*!	Walks from \a source up to the first router both ends hang below and
	then down to \a destination, reserving a hop ID on every adapter on the
	way. The first and last hop IDs are given; those in between are free
	ones both sides of a link agree on.
*/
/*static*/ Path*
Path::Create(Domain* domain, Adapter* source, int32 sourceHop,
	Adapter* destination, int32 destinationHop, const char* name)
{
	Path* path = new(std::nothrow) Path(domain, name);
	if (path == NULL)
		return NULL;

	Router* end = destination->router;
	Router* router = source->router;

	// the common ancestor
	Router* top = router;
	while (top != NULL && !(top->Depth() <= end->Depth()
		&& (end->Route() & ((top->Depth() == 0 ? 0
			: (1ULL << (top->Depth() * 8)) - 1))) == top->Route()))
		top = top->Parent();
	if (top == NULL) {
		delete path;
		return NULL;
	}

	Adapter* pairs[USB4_MAX_PATH_HOPS][2];
	int32 count = 0;
	Adapter* in = source;
	while (router != top) {
		Adapter* out = router->UpstreamAdapter();
		if (out == NULL || out->remote == NULL || count >= USB4_MAX_PATH_HOPS) {
			delete path;
			return NULL;
		}
		pairs[count][0] = in;
		pairs[count][1] = out;
		count++;
		in = out->remote;
		router = in->router;
	}
	while (router != end) {
		Adapter* out = router->AdapterTowards(end->Route());
		if (out == NULL || out->remote == NULL || count >= USB4_MAX_PATH_HOPS) {
			delete path;
			return NULL;
		}
		pairs[count][0] = in;
		pairs[count][1] = out;
		count++;
		in = out->remote;
		router = in->router;
	}
	if (count >= USB4_MAX_PATH_HOPS) {
		delete path;
		return NULL;
	}
	pairs[count][0] = in;
	pairs[count][1] = destination;
	count++;

	// hop IDs
	int32 inHop = sourceHop;
	for (int32 i = 0; i < count; i++) {
		PathHop& hop = path->fHops[i];
		hop.in = pairs[i][0];
		hop.out = pairs[i][1];
		hop.inHop = -1;
		hop.nextHop = -1;
		path->fLength = i + 1;

		if (i == 0) {
			if (!hop.in->inHops.Reserve(inHop)) {
				TRACE_ERROR("%s path: in hop %" B_PRId32 " of %#" B_PRIx64
					":%u is taken\n", name, inHop, hop.in->router->Route(),
					hop.in->number);
				delete path;
				return NULL;
			}
		}
		hop.inHop = inHop;

		if (i == count - 1) {
			if (!hop.out->outHops.Reserve(destinationHop)) {
				TRACE_ERROR("%s path: out hop %" B_PRId32 " of %#" B_PRIx64
					":%u is taken\n", name, destinationHop,
					hop.out->router->Route(), hop.out->number);
				delete path;
				return NULL;
			}
			hop.nextHop = destinationHop;
			break;
		}

		// the hop ID on the link has to be free on both of its ends
		Adapter* next = pairs[i + 1][0];
		int32 max = min_c(hop.out->maxOutHop, next->maxInHop);
		int32 id = -1;
		for (int32 candidate = USB4_FIRST_PATH_HOP; candidate <= max;
				candidate++) {
			if (!hop.out->outHops.Reserve(candidate))
				continue;
			if (!next->inHops.Reserve(candidate)) {
				hop.out->outHops.Release(candidate);
				continue;
			}
			id = candidate;
			break;
		}
		if (id < 0) {
			TRACE_ERROR("%s path: no free hop between %#" B_PRIx64 ":%u and %#"
				B_PRIx64 ":%u\n", name, hop.out->router->Route(),
				hop.out->number, next->router->Route(), next->number);
			delete path;
			return NULL;
		}
		hop.nextHop = id;
		inHop = id;
	}

	return path;
}


bool
Path::IsInvalid() const
{
	for (int32 i = 0; i < fLength; i++) {
		if (fHops[i].in->router->IsUnplugged()
			|| fHops[i].out->router->IsUnplugged())
			return true;
	}
	return false;
}


bool
Path::Crosses(const Adapter* adapter) const
{
	for (int32 i = 0; i < fLength; i++) {
		if (fHops[i].in == adapter || fHops[i].out == adapter)
			return true;
	}
	return false;
}


void
Path::_ReleaseNFC(int32 first)
{
	for (int32 i = first; i < fLength; i++)
		fHops[i].in->AddNFCCredits(-(int32)fHops[i].nfcCredits);
}


void
Path::_DeactivateHops(int32 first)
{
	for (int32 i = first; i < fLength; i++) {
		PathHop& hop = fHops[i];
		if (hop.in->router->IsUnplugged())
			continue;
		fDomain->DeactivateHop(hop.in, hop.inHop, clearFlowControl);
	}
}


status_t
Path::Activate()
{
	if (fActive)
		return B_BAD_VALUE;

	TRACE("activating %s path %#" B_PRIx64 ":%u -> %#" B_PRIx64 ":%u (%"
		B_PRId32 " hops)\n", fName, fHops[0].in->router->Route(),
		fHops[0].in->number, fHops[fLength - 1].out->router->Route(),
		fHops[fLength - 1].out->number, fLength);

	for (int32 i = fLength - 1; i >= 0; i--) {
		status_t status = fHops[i].in->AddNFCCredits(fHops[i].nfcCredits);
		if (status != B_OK) {
			_ReleaseNFC(i + 1);
			return status;
		}
	}

	for (int32 i = fLength - 1; i >= 0; i--) {
		PathHop& hop = fHops[i];

		fDomain->DeactivateHop(hop.in, hop.inHop, clearFlowControl);

		uint8 outMask = i == fLength - 1 ? PATH_DESTINATION : PATH_INTERNAL;
		uint8 inMask = i == 0 ? PATH_SOURCE : PATH_INTERNAL;

		uint32 entry[2];
		entry[0] = HOP_0_NEXT_HOP(hop.nextHop)
			| HOP_0_OUT_ADAPTER(hop.out->number)
			| HOP_0_INITIAL_CREDITS(hop.initialCredits) | HOP_0_VALID;
		entry[1] = HOP_1_WEIGHT(weight) | HOP_1_PRIORITY(priority)
			| (dropPackets ? HOP_1_DROP_PACKETS : 0)
			| HOP_1_COUNTER(0x7ff)
			| ((ingressFlowControl & inMask) != 0 ? HOP_1_INGRESS_FC : 0)
			| ((egressFlowControl & outMask) != 0 ? HOP_1_EGRESS_FC : 0)
			| ((ingressShared & inMask) != 0 ? HOP_1_INGRESS_SHARED : 0)
			| ((egressShared & outMask) != 0 ? HOP_1_EGRESS_SHARED : 0);

		TRACE_VERBOSE("  hop %" B_PRId32 ": %#" B_PRIx64 ":%u hop %" B_PRId32
			" -> %u hop %" B_PRId32 ", credits %" B_PRIu32 "/%" B_PRIu32
			" (%08" B_PRIx32 " %08" B_PRIx32 ")\n", i, hop.in->router->Route(),
			hop.in->number, hop.inHop, hop.out->number, hop.nextHop,
			hop.initialCredits, hop.nfcCredits, entry[0], entry[1]);

		status_t status = hop.in->Write(USB4_CFG_HOPS, 2 * hop.inHop, entry,
			2);
		if (status != B_OK) {
			TRACE_ERROR("%s path: writing hop %" B_PRId32 " failed: %s\n",
				fName, i, strerror(status));
			_DeactivateHops(i);
			_ReleaseNFC(0);
			return status;
		}
	}

	fActive = true;
	return B_OK;
}


void
Path::Deactivate()
{
	if (!fActive)
		return;
	TRACE("deactivating %s path\n", fName);
	_DeactivateHops(0);
	_ReleaseNFC(0);
	fActive = false;
}


// #pragma mark - Tunnel


Tunnel::Tunnel(Domain* domain, tunnel_type type, Adapter* source,
	Adapter* destination)
	:
	fDomain(domain),
	fType(type),
	fSource(source),
	fDestination(destination),
	fPathCount(0),
	fActive(false),
	fMaxUp(0),
	fMaxDown(0),
	fAllocatedUp(0),
	fAllocatedDown(0),
	fDPRate(0),
	fDPLanes(0)
{
	memset(fPaths, 0, sizeof(fPaths));
}


Tunnel::~Tunnel()
{
	if (fActive)
		Deactivate();
	for (int32 i = 0; i < fPathCount; i++)
		delete fPaths[i];
}


const char*
Tunnel::TypeName() const
{
	switch (fType) {
		case TUNNEL_PCIE:
			return "PCIe";
		case TUNNEL_DP:
			return "DisplayPort";
		case TUNNEL_USB3:
			return "USB3";
	}
	return "?";
}


bool
Tunnel::Downstream() const
{
	return fSource->router->Depth() < fDestination->router->Depth();
}


bool
Tunnel::IsInvalid() const
{
	for (int32 i = 0; i < fPathCount; i++) {
		if (fPaths[i] == NULL || fPaths[i]->IsInvalid())
			return true;
	}
	return false;
}


bool
Tunnel::Crosses(const Adapter* adapter) const
{
	for (int32 i = 0; i < fPathCount; i++) {
		if (fPaths[i] != NULL && fPaths[i]->Crosses(adapter))
			return true;
	}
	return false;
}


static void
init_pcie_path(Path* path, Domain* domain)
{
	path->egressFlowControl = PATH_SOURCE | PATH_INTERNAL;
	path->egressShared = PATH_NONE;
	path->ingressFlowControl = PATH_ALL;
	path->ingressShared = PATH_NONE;
	path->priority = PCIE_PRIORITY;
	path->weight = PCIE_WEIGHT;
	path->dropPackets = false;
}


static status_t
init_pcie_credits(Path* path, Domain* domain)
{
	for (int32 i = 0; i < path->Length(); i++) {
		PathHop& hop = path->HopAt(i);
		if (uses_credit_allocation(hop.in)) {
			uint32 available = domain->AvailableCredits(hop.in, NULL);
			uint32 credits = min_c(hop.in->router->MaxPCIeCredits(), available);
			if (credits < MIN_PCIE_CREDITS)
				return B_DEVICE_FULL;
			hop.initialCredits = credits;
		} else if (hop.in->IsLane())
			hop.initialCredits = hop.in->bonded ? 32 : 16;
		else
			hop.initialCredits = 7;
	}
	return B_OK;
}


/*static*/ Tunnel*
Tunnel::CreatePCIe(Domain* domain, Adapter* up, Adapter* down)
{
	Tunnel* tunnel = new(std::nothrow) Tunnel(domain, TUNNEL_PCIE, down, up);
	if (tunnel == NULL)
		return NULL;

	Path* path = Path::Create(domain, down, PCIE_HOP, up, PCIE_HOP,
		"PCIe down");
	if (path == NULL) {
		delete tunnel;
		return NULL;
	}
	tunnel->fPaths[tunnel->fPathCount++] = path;
	init_pcie_path(path, domain);
	if (init_pcie_credits(path, domain) != B_OK) {
		delete tunnel;
		return NULL;
	}

	path = Path::Create(domain, up, PCIE_HOP, down, PCIE_HOP, "PCIe up");
	if (path == NULL) {
		delete tunnel;
		return NULL;
	}
	tunnel->fPaths[tunnel->fPathCount++] = path;
	init_pcie_path(path, domain);
	if (init_pcie_credits(path, domain) != B_OK) {
		delete tunnel;
		return NULL;
	}

	return tunnel;
}


static int32
usb3_max_link_rate(Adapter* adapter)
{
	uint32 value;
	if (adapter->capAdapter == 0
		|| adapter->ReadAdapter(adapter->capAdapter + ADP_USB3_CS_4, value)
			!= B_OK)
		return -1;
	return ADP_USB3_CS_4_MSLR(value) == ADP_USB3_CS_4_MSLR_20G ? 20000 : 10000;
}


static void
init_usb3_path(Path* path)
{
	path->egressFlowControl = PATH_SOURCE | PATH_INTERNAL;
	path->egressShared = PATH_NONE;
	path->ingressFlowControl = PATH_ALL;
	path->ingressShared = PATH_NONE;
	path->priority = USB3_PRIORITY;
	path->weight = USB3_WEIGHT;
	path->dropPackets = false;

	for (int32 i = 0; i < path->Length(); i++) {
		PathHop& hop = path->HopAt(i);
		if (uses_credit_allocation(hop.in))
			hop.initialCredits = hop.in->router->MaxUSB3Credits();
		else if (hop.in->IsLane())
			hop.initialCredits = hop.in->bonded ? 32 : 16;
		else
			hop.initialCredits = 7;
	}
}


/*static*/ Tunnel*
Tunnel::CreateUSB3(Domain* domain, Adapter* up, Adapter* down, int32 maxUp,
	int32 maxDown)
{
	int32 maxRate = 0;
	if (down->router->IsHost() && (maxUp > 0 || maxDown > 0)) {
		// isochronous transfers may take 90% of what the adapters do
		int32 upRate = usb3_max_link_rate(up);
		int32 downRate = usb3_max_link_rate(down);
		if (upRate < 0 || downRate < 0)
			return NULL;
		maxRate = min_c(upRate, downRate) * 90 / 100;
	}

	Tunnel* tunnel = new(std::nothrow) Tunnel(domain, TUNNEL_USB3, down, up);
	if (tunnel == NULL)
		return NULL;
	tunnel->fMaxUp = maxUp;
	tunnel->fMaxDown = maxDown;

	Path* path = Path::Create(domain, down, USB3_HOP, up, USB3_HOP,
		"USB3 down");
	if (path == NULL) {
		delete tunnel;
		return NULL;
	}
	tunnel->fPaths[tunnel->fPathCount++] = path;
	init_usb3_path(path);

	path = Path::Create(domain, up, USB3_HOP, down, USB3_HOP, "USB3 up");
	if (path == NULL) {
		delete tunnel;
		return NULL;
	}
	tunnel->fPaths[tunnel->fPathCount++] = path;
	init_usb3_path(path);

	if (down->router->IsHost()) {
		tunnel->fAllocatedUp = min_c(maxRate, maxUp);
		tunnel->fAllocatedDown = min_c(maxRate, maxDown);
	}
	return tunnel;
}


static void
init_dp_aux_path(Path* path)
{
	path->egressFlowControl = PATH_SOURCE | PATH_INTERNAL;
	path->egressShared = PATH_NONE;
	path->ingressFlowControl = PATH_ALL;
	path->ingressShared = PATH_NONE;
	path->priority = DP_AUX_PRIORITY;
	path->weight = DP_AUX_WEIGHT;

	for (int32 i = 0; i < path->Length(); i++) {
		PathHop& hop = path->HopAt(i);
		hop.initialCredits = uses_credit_allocation(hop.in)
			? hop.in->router->MinDPAuxCredits() : 1;
	}
}


static status_t
init_dp_video_path(Path* path, Domain* domain)
{
	path->egressFlowControl = PATH_NONE;
	path->egressShared = PATH_NONE;
	path->ingressFlowControl = PATH_NONE;
	path->ingressShared = PATH_NONE;
	path->priority = DP_VIDEO_PRIORITY;
	path->weight = DP_VIDEO_WEIGHT;

	for (int32 i = 0; i < path->Length(); i++) {
		PathHop& hop = path->HopAt(i);
		Router* router = hop.in->router;
		if (uses_credit_allocation(hop.in)) {
			size_t maxStreams = 0;
			domain->AvailableCredits(hop.in, &maxStreams);
			uint32 nfc = hop.in->header[4] & ADP_CS_4_NFC_BUFFERS_MASK;
			if (router->MinDPMainCredits() == 0
				|| nfc / router->MinDPMainCredits() > maxStreams)
				return B_DEVICE_FULL;
			hop.nfcCredits = router->MinDPMainCredits();
		} else {
			hop.nfcCredits = hop.in->totalCredits > 2
				? min_c((uint32)hop.in->totalCredits - 2, 12u) : 0;
		}
	}
	return B_OK;
}


/*static*/ Tunnel*
Tunnel::CreateDP(Domain* domain, Adapter* in, Adapter* out, int32 maxUp,
	int32 maxDown)
{
	if (in->capAdapter == 0 || out->capAdapter == 0)
		return NULL;

	Tunnel* tunnel = new(std::nothrow) Tunnel(domain, TUNNEL_DP, in, out);
	if (tunnel == NULL)
		return NULL;
	tunnel->fMaxUp = maxUp;
	tunnel->fMaxDown = maxDown;

	Path* path = Path::Create(domain, in, DP_VIDEO_HOP, out, DP_VIDEO_HOP,
		"DP video");
	if (path == NULL) {
		delete tunnel;
		return NULL;
	}
	tunnel->fPaths[tunnel->fPathCount++] = path;
	if (init_dp_video_path(path, domain) != B_OK) {
		TRACE_ERROR("no credits left for another DisplayPort stream\n");
		delete tunnel;
		return NULL;
	}

	path = Path::Create(domain, in, DP_AUX_TX_HOP, out, DP_AUX_TX_HOP,
		"DP AUX TX");
	if (path == NULL) {
		delete tunnel;
		return NULL;
	}
	tunnel->fPaths[tunnel->fPathCount++] = path;
	init_dp_aux_path(path);

	path = Path::Create(domain, out, DP_AUX_RX_HOP, in, DP_AUX_RX_HOP,
		"DP AUX RX");
	if (path == NULL) {
		delete tunnel;
		return NULL;
	}
	tunnel->fPaths[tunnel->fPathCount++] = path;
	init_dp_aux_path(path);

	return tunnel;
}


// #pragma mark - USB3 bandwidth


static uint32
usb3_bw_to_mbps(uint32 bw, uint32 scale)
{
	uint64 uframes = (uint64)bw * 512 << scale;
	return (uint32)((uframes * 8000 + 500000) / 1000000);
}


static uint32
mbps_to_usb3_bw(uint32 mbps, uint32 scale)
{
	// one microframe is 125 us
	uint64 uframes = (uint64)mbps * 1000000 / 8000;
	uint64 unit = 512ULL << scale;
	return (uint32)((uframes + unit - 1) / unit);
}


static status_t
usb3_cm_request(Adapter* down, bool request)
{
	uint32 value;
	status_t status = down->ReadAdapter(down->capAdapter + ADP_USB3_CS_2,
		value);
	if (status != B_OK)
		return status;
	if (request)
		value |= ADP_USB3_CS_2_CMR;
	else
		value &= ~ADP_USB3_CS_2_CMR;
	status = down->WriteAdapter(down->capAdapter + ADP_USB3_CS_2, value);
	if (status != B_OK)
		return status;

	// the host controller acknowledges
	uint32 wanted = request ? ADP_USB3_CS_1_HCA : 0;
	bigtime_t deadline = system_time() + 1500000;
	do {
		uint32 cs1;
		status = down->ReadAdapter(down->capAdapter + ADP_USB3_CS_1, cs1);
		if (status != B_OK)
			return status;
		if ((cs1 & ADP_USB3_CS_1_HCA) == wanted)
			return B_OK;
		snooze(50);
	} while (system_time() < deadline);
	return B_TIMED_OUT;
}


static status_t
usb3_read_consumed(Adapter* down, int32& up, int32& downstream)
{
	uint32 cs1, scale;
	status_t status = down->ReadAdapter(down->capAdapter + ADP_USB3_CS_1, cs1);
	if (status == B_OK)
		status = down->ReadAdapter(down->capAdapter + ADP_USB3_CS_3, scale);
	if (status != B_OK)
		return status;
	scale &= ADP_USB3_CS_3_SCALE_MASK;
	up = usb3_bw_to_mbps(ADP_USB3_CS_1_CUBW(cs1), scale);
	downstream = usb3_bw_to_mbps(ADP_USB3_CS_1_CDBW(cs1), scale);
	return B_OK;
}


static status_t
usb3_write_allocated(Adapter* down, int32 up, int32 downstream)
{
	uint32 scale = 0;
	int32 max = max_c(up, downstream);
	while (scale < 64 && mbps_to_usb3_bw(max, scale) >= 4096)
		scale++;
	if (scale >= 64)
		return B_BAD_VALUE;

	status_t status = down->WriteAdapter(down->capAdapter + ADP_USB3_CS_3,
		scale);
	if (status != B_OK)
		return status;

	uint32 value;
	status = down->ReadAdapter(down->capAdapter + ADP_USB3_CS_2, value);
	if (status != B_OK)
		return status;
	value &= ~(ADP_USB3_CS_2_AUBW_MASK | ADP_USB3_CS_2_ADBW_MASK);
	value |= mbps_to_usb3_bw(downstream, scale) << ADP_USB3_CS_2_ADBW_SHIFT;
	value |= mbps_to_usb3_bw(up, scale);
	return down->WriteAdapter(down->capAdapter + ADP_USB3_CS_2, value);
}


status_t
Tunnel::_USB3AllocateBandwidth(int32& up, int32& down)
{
	status_t status = usb3_cm_request(fSource, true);
	if (status != B_OK)
		return status;

	int32 consumedUp, consumedDown;
	status = usb3_read_consumed(fSource, consumedUp, consumedDown);
	if (status == B_OK) {
		up = max_c(up, consumedUp);
		down = max_c(down, consumedDown);
		status = usb3_write_allocated(fSource, up, down);
	}

	usb3_cm_request(fSource, false);
	return status;
}


status_t
Tunnel::ReleaseUnusedBandwidth()
{
	if (fType != TUNNEL_USB3 || !fSource->router->IsHost())
		return B_OK;

	status_t status = usb3_cm_request(fSource, true);
	if (status != B_OK)
		return status;

	int32 consumedUp, consumedDown;
	status = usb3_read_consumed(fSource, consumedUp, consumedDown);
	if (status == B_OK) {
		// keep a minimum for the isochronous transfers that might start
		consumedUp = max_c(consumedUp, 900);
		consumedDown = max_c(consumedDown, 900);
		status = usb3_write_allocated(fSource, consumedUp, consumedDown);
		if (status == B_OK) {
			fAllocatedUp = consumedUp;
			fAllocatedDown = consumedDown;
		}
	}

	usb3_cm_request(fSource, false);
	TRACE("USB3 bandwidth released down to %" B_PRId32 "/%" B_PRId32
		" Mb/s\n", fAllocatedUp, fAllocatedDown);
	return status;
}


void
Tunnel::ReclaimAvailableBandwidth(int32& availableUp, int32& availableDown)
{
	if (fType != TUNNEL_USB3 || !fSource->router->IsHost())
		return;

	int32 upRate = usb3_max_link_rate(fDestination);
	int32 downRate = usb3_max_link_rate(fSource);
	if (upRate < 0 || downRate < 0)
		return;
	int32 maxRate = min_c(upRate, downRate) * 90 / 100;

	if (fAllocatedUp >= maxRate && fAllocatedDown >= maxRate)
		return;

	int32 up = max_c(min_c(maxRate, availableUp), fAllocatedUp);
	int32 down = max_c(min_c(maxRate, availableDown), fAllocatedDown);
	if (up == fAllocatedUp && down == fAllocatedDown)
		return;

	if (_USB3AllocateBandwidth(up, down) != B_OK) {
		TRACE_ERROR("USB3 bandwidth allocation failed\n");
		return;
	}
	fAllocatedUp = up;
	fAllocatedDown = down;
	availableUp -= up;
	availableDown -= down;
	TRACE("USB3 bandwidth raised to %" B_PRId32 "/%" B_PRId32 " Mb/s\n", up,
		down);
}


status_t
Tunnel::ConsumedBandwidth(int32& up, int32& down)
{
	up = down = 0;
	switch (fType) {
		case TUNNEL_PCIE:
			return B_OK;

		case TUNNEL_USB3:
		{
			if (!fSource->router->IsHost())
				return B_OK;
			// PCIe traffic shares the bandwidth by the weights
			int32 weights = USB3_WEIGHT
				+ (fDomain->MayTunnelPCIe() ? PCIE_WEIGHT : 0);
			up = fAllocatedUp * weights / USB3_WEIGHT;
			down = fAllocatedDown * weights / USB3_WEIGHT;
			return B_OK;
		}

		case TUNNEL_DP:
		{
			uint32 cap;
			uint32 reg = DPRXDone() ? DP_COMMON_CAP : DP_REMOTE_CAP;
			status_t status = fSource->ReadAdapter(fSource->capAdapter + reg,
				cap);
			if (status != B_OK)
				return status;
			int32 bandwidth = dp_bandwidth(dp_cap_rate(cap), dp_cap_lanes(cap));
			if (Downstream())
				down = bandwidth;
			else
				up = bandwidth;
			return B_OK;
		}
	}
	return B_OK;
}


// #pragma mark - DisplayPort


bool
Tunnel::DPRXDone()
{
	uint32 value;
	return fType == TUNNEL_DP
		&& fSource->ReadAdapter(fSource->capAdapter + DP_COMMON_CAP, value)
			== B_OK
		&& (value & DP_CAP_DPRX_DONE) != 0;
}


status_t
Tunnel::_ExchangeDPCapabilities()
{
	Adapter* in = fSource;
	Adapter* out = fDestination;

	// connection manager handshake with the DP OUT adapter
	if (in->router->IsUSB4() && out->router->IsUSB4()) {
		uint32 value;
		status_t status = out->ReadAdapter(out->capAdapter + DP_STATUS_CTRL,
			value);
		if (status != B_OK)
			return status;
		value |= DP_STATUS_CTRL_UF | DP_STATUS_CTRL_CMHS;
		status = out->WriteAdapter(out->capAdapter + DP_STATUS_CTRL, value);
		if (status != B_OK)
			return status;

		bigtime_t deadline = system_time() + 3000000;
		while (true) {
			status = out->ReadAdapter(out->capAdapter + DP_STATUS_CTRL, value);
			if (status != B_OK)
				return status;
			if ((value & DP_STATUS_CTRL_CMHS) == 0)
				break;
			if (system_time() > deadline) {
				TRACE_ERROR("DP OUT %#" B_PRIx64 ":%u: no handshake\n",
					out->router->Route(), out->number);
				return B_TIMED_OUT;
			}
			snooze(100);
		}
	}

	uint32 inCap, outCap;
	status_t status = in->ReadAdapter(in->capAdapter + DP_LOCAL_CAP, inCap);
	if (status == B_OK)
		status = out->ReadAdapter(out->capAdapter + DP_LOCAL_CAP, outCap);
	if (status != B_OK)
		return status;

	status = out->WriteAdapter(out->capAdapter + DP_REMOTE_CAP, inCap);
	if (status != B_OK)
		return status;

	uint32 inRate = dp_cap_rate(inCap);
	uint32 inLanes = dp_cap_lanes(inCap);
	uint32 outRate = dp_cap_rate(outCap);
	uint32 outLanes = dp_cap_lanes(outCap);
	int32 bandwidth = dp_bandwidth(outRate, outLanes);
	TRACE("DP IN %" B_PRIu32 " Mb/s x%" B_PRIu32 ", DP OUT %" B_PRIu32
		" Mb/s x%" B_PRIu32 " = %" B_PRId32 " Mb/s\n", inRate, inLanes,
		outRate, outLanes, bandwidth);

	int32 maxBandwidth = Downstream() ? fMaxDown : fMaxUp;
	fDPRate = min_c(inRate, outRate);
	fDPLanes = min_c(inLanes, outLanes);
	if (maxBandwidth > 0 && bandwidth > maxBandwidth) {
		uint32 newRate, newLanes;
		status = dp_reduce_bandwidth(maxBandwidth, inRate, inLanes, outRate,
			outLanes, newRate, newLanes);
		if (status != B_OK) {
			TRACE_ERROR("not enough bandwidth for DisplayPort (%" B_PRId32
				" Mb/s available)\n", maxBandwidth);
			return status;
		}
		TRACE("DisplayPort reduced to %" B_PRIu32 " Mb/s x%" B_PRIu32 "\n",
			newRate, newLanes);
		outCap = dp_cap_set(outCap, newRate, newLanes);
		fDPRate = newRate;
		fDPLanes = newLanes;
	}

	return in->WriteAdapter(in->capAdapter + DP_REMOTE_CAP, outCap);
}


static status_t
dp_enable(Adapter* adapter, bool enable)
{
	uint32 value[2];
	status_t status = adapter->Read(USB4_CFG_ADAPTER,
		adapter->capAdapter + ADP_DP_CS_0, value, 2);
	if (status != B_OK)
		return status;
	if (enable)
		value[0] |= ADP_DP_CS_0_VE | ADP_DP_CS_0_AE;
	else
		value[0] &= ~(ADP_DP_CS_0_VE | ADP_DP_CS_0_AE);
	return adapter->Write(USB4_CFG_ADAPTER, adapter->capAdapter + ADP_DP_CS_0,
		value, 2);
}


static status_t
dp_set_hops(Adapter* adapter, uint32 video, uint32 auxTx, uint32 auxRx)
{
	// USB4 DP adapters know their hops from the path configuration
	if (adapter->router->IsUSB4())
		return B_OK;

	uint32 value[2];
	status_t status = adapter->Read(USB4_CFG_ADAPTER,
		adapter->capAdapter + ADP_DP_CS_0, value, 2);
	if (status != B_OK)
		return status;
	value[0] = (value[0] & ~ADP_DP_CS_0_VIDEO_HOP_MASK)
		| ((video << ADP_DP_CS_0_VIDEO_HOP_SHIFT) & ADP_DP_CS_0_VIDEO_HOP_MASK);
	value[1] = (value[1] & ~(ADP_DP_CS_1_AUX_TX_HOP_MASK
			| ADP_DP_CS_1_AUX_RX_HOP_MASK))
		| (auxTx & ADP_DP_CS_1_AUX_TX_HOP_MASK)
		| ((auxRx << ADP_DP_CS_1_AUX_RX_HOP_SHIFT)
			& ADP_DP_CS_1_AUX_RX_HOP_MASK);
	return adapter->Write(USB4_CFG_ADAPTER, adapter->capAdapter + ADP_DP_CS_0,
		value, 2);
}


status_t
Tunnel::_ActivateDP(bool activate)
{
	Path* video = fPaths[0];
	Path* auxOut = fPaths[1];
	Path* auxIn = fPaths[2];

	if (activate) {
		int32 last = video->Length() - 1;
		dp_set_hops(fSource, video->HopAt(0).inHop, auxOut->HopAt(0).inHop,
			auxIn->HopAt(auxIn->Length() - 1).nextHop);
		dp_set_hops(fDestination, video->HopAt(last).nextHop,
			auxIn->HopAt(0).inHop,
			auxOut->HopAt(auxOut->Length() - 1).nextHop);
	} else {
		uint32 value;
		if (fSource->ReadAdapter(fSource->capAdapter + ADP_DP_CS_3, value)
				== B_OK) {
			fSource->WriteAdapter(fSource->capAdapter + ADP_DP_CS_3,
				value | ADP_DP_CS_3_HPDC);
		}
		dp_set_hops(fSource, 0, 0, 0);
		dp_set_hops(fDestination, 0, 0, 0);
	}

	status_t status = dp_enable(fSource, activate);
	if (status != B_OK && activate)
		return status;
	status = dp_enable(fDestination, activate);
	return activate ? status : B_OK;
}


// #pragma mark - activation


static status_t
protocol_enable(Adapter* adapter, tunnel_type type, bool enable)
{
	if (adapter->capAdapter == 0)
		return B_BAD_VALUE;
	if (type == TUNNEL_PCIE) {
		return adapter->WriteAdapter(adapter->capAdapter + ADP_PCIE_CS_0,
			enable ? ADP_PCIE_CS_0_PE : 0);
	}
	return adapter->WriteAdapter(adapter->capAdapter + ADP_USB3_CS_0,
		enable ? (ADP_USB3_CS_0_PE | ADP_USB3_CS_0_V) : ADP_USB3_CS_0_V);
}


status_t
Tunnel::Activate()
{
	TRACE("activating %s tunnel %#" B_PRIx64 ":%u -> %#" B_PRIx64 ":%u\n",
		TypeName(), fSource->router->Route(), fSource->number,
		fDestination->router->Route(), fDestination->number);

	for (int32 i = 0; i < fPathCount; i++)
		fPaths[i]->Deactivate();

	status_t status = B_OK;
	if (fType == TUNNEL_USB3 && fSource->router->IsHost()) {
		int32 up = fAllocatedUp;
		int32 down = fAllocatedDown;
		status = _USB3AllocateBandwidth(up, down);
		if (status == B_OK) {
			fAllocatedUp = up;
			fAllocatedDown = down;
			TRACE("USB3 bandwidth %" B_PRId32 "/%" B_PRId32 " Mb/s\n", up,
				down);
		}
	} else if (fType == TUNNEL_DP)
		status = _ExchangeDPCapabilities();
	if (status != B_OK) {
		TRACE_ERROR("%s tunnel: preparing failed: %s\n", TypeName(),
			strerror(status));
		return status;
	}

	for (int32 i = 0; i < fPathCount; i++) {
		status = fPaths[i]->Activate();
		if (status != B_OK)
			break;
	}

	if (status == B_OK) {
		switch (fType) {
			case TUNNEL_PCIE:
				// downstream end first
				status = protocol_enable(fDestination, fType, true);
				if (status == B_OK)
					status = protocol_enable(fSource, fType, true);
				break;
			case TUNNEL_USB3:
				status = protocol_enable(fSource, fType, true);
				if (status == B_OK && fDestination->IsUSB3Up())
					status = protocol_enable(fDestination, fType, true);
				break;
			case TUNNEL_DP:
				status = _ActivateDP(true);
				break;
		}
	}

	if (status != B_OK) {
		TRACE_ERROR("%s tunnel activation failed: %s\n", TypeName(),
			strerror(status));
		fActive = true;
		Deactivate();
		return status;
	}

	fActive = true;
	return B_OK;
}


void
Tunnel::Deactivate()
{
	if (!fActive)
		return;

	TRACE("deactivating %s tunnel %#" B_PRIx64 ":%u -> %#" B_PRIx64 ":%u\n",
		TypeName(), fSource->router->Route(), fSource->number,
		fDestination->router->Route(), fDestination->number);

	switch (fType) {
		case TUNNEL_PCIE:
			protocol_enable(fSource, fType, false);
			protocol_enable(fDestination, fType, false);
			break;
		case TUNNEL_USB3:
			protocol_enable(fSource, fType, false);
			if (fDestination->IsUSB3Up())
				protocol_enable(fDestination, fType, false);
			break;
		case TUNNEL_DP:
			_ActivateDP(false);
			break;
	}

	for (int32 i = 0; i < fPathCount; i++) {
		if (fPaths[i] != NULL)
			fPaths[i]->Deactivate();
	}
	fActive = false;
}
