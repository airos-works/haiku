/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef USB4_TUNNEL_H
#define USB4_TUNNEL_H


#include <util/DoublyLinkedList.h>

#include "Router.h"


class Domain;


enum path_hop_mask {
	PATH_NONE			= 0,
	PATH_SOURCE			= 1,
	PATH_INTERNAL		= 2,
	PATH_DESTINATION	= 4,
	PATH_ALL			= 7,
};


struct PathHop {
	Adapter*	in;
	int32		inHop;
	Adapter*	out;
	int32		nextHop;
	uint32		initialCredits;
	uint32		nfcCredits;
};


#define USB4_MAX_PATH_HOPS	8


/*!	A one way path through the routers from a source adapter to a
	destination adapter. Every router on the way gets one hop entry that
	forwards the packets of the path to the next adapter.
*/
class Path {
public:
								Path(Domain* domain, const char* name);
								~Path();

	static	Path*				Create(Domain* domain, Adapter* source,
									int32 sourceHop, Adapter* destination,
									int32 destinationHop, const char* name);
	static	Path*				Discover(Domain* domain, Adapter* source,
									int32 sourceHop, Adapter* destination,
									int32 destinationHop, Adapter** _last,
									const char* name);

			status_t			Activate();
			void				Deactivate();
			bool				IsActive() const { return fActive; }
			bool				IsInvalid() const;
			bool				Crosses(const Adapter* adapter) const;

			int32				Length() const { return fLength; }
			PathHop&			HopAt(int32 index) { return fHops[index]; }
			const char*			Name() const { return fName; }

			uint8				priority;
			uint8				weight;
			uint8				ingressFlowControl;
			uint8				egressFlowControl;
			uint8				ingressShared;
			uint8				egressShared;
			bool				dropPackets;
			bool				clearFlowControl;

private:
			void				_DeactivateHops(int32 first);
			void				_ReleaseNFC(int32 first);

			Domain*				fDomain;
			const char*			fName;
			PathHop				fHops[USB4_MAX_PATH_HOPS];
			int32				fLength;
			bool				fActive;
};


enum tunnel_type {
	TUNNEL_PCIE,
	TUNNEL_DP,
	TUNNEL_USB3,
};


class Tunnel : public DoublyLinkedListLinkImpl<Tunnel> {
public:
								Tunnel(Domain* domain, tunnel_type type,
									Adapter* source, Adapter* destination);
								~Tunnel();

	static	Tunnel*				CreatePCIe(Domain* domain, Adapter* up,
									Adapter* down);
	static	Tunnel*				CreateUSB3(Domain* domain, Adapter* up,
									Adapter* down, int32 maxUp,
									int32 maxDown);
	static	Tunnel*				CreateDP(Domain* domain, Adapter* in,
									Adapter* out, int32 maxUp,
									int32 maxDown);
	static	Tunnel*				Discover(Domain* domain, Adapter* source);
									// a tunnel the boot firmware set up

			status_t			Activate();
			void				Deactivate();
			bool				IsActive() const { return fActive; }
			bool				IsInvalid() const;
			bool				Crosses(const Adapter* adapter) const;

			tunnel_type			Type() const { return fType; }
			const char*			TypeName() const;
			Adapter*			Source() const { return fSource; }
			Adapter*			Destination() const { return fDestination; }
			bool				Downstream() const;

			status_t			ConsumedBandwidth(int32& up, int32& down);
			int32				AllocatedUp() const { return fAllocatedUp; }
			int32				AllocatedDown() const
									{ return fAllocatedDown; }

			// USB3 bandwidth management (host router tunnels only)
			status_t			ReleaseUnusedBandwidth();
			void				ReclaimAvailableBandwidth(int32& availableUp,
									int32& availableDown);

			// DP
			bool				DPRXDone();
			uint32				DPRate() const { return fDPRate; }
			uint32				DPLanes() const { return fDPLanes; }

private:
			status_t			_ActivateDP(bool activate);
			status_t			_ExchangeDPCapabilities();
			status_t			_USB3AllocateBandwidth(int32& up,
									int32& down);

			Domain*				fDomain;
			tunnel_type			fType;
			Adapter*			fSource;
			Adapter*			fDestination;
			Path*				fPaths[3];
			int32				fPathCount;
			bool				fActive;

			int32				fMaxUp;
			int32				fMaxDown;
			int32				fAllocatedUp;
			int32				fAllocatedDown;

			uint32				fDPRate;
			uint32				fDPLanes;
};


typedef DoublyLinkedList<Tunnel> TunnelList;


uint32 dp_bandwidth(uint32 rate, uint32 lanes);


#endif	// USB4_TUNNEL_H
