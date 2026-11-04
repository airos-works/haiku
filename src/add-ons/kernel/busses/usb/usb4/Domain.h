/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef USB4_DOMAIN_H
#define USB4_DOMAIN_H


#include <lock.h>
#include <util/DoublyLinkedList.h>

#include "Nhi.h"
#include "Router.h"
#include "Tunnel.h"


#define USB4_MAX_ROUTERS		64
#define USB4_MAX_DP_RESOURCES	16

// what the platform lets the operating system tunnel (ACPI _OSC)
#define USB4_CONTROL_USB3		0x1
#define USB4_CONTROL_DP			0x2
#define USB4_CONTROL_PCIE		0x4
#define USB4_CONTROL_XDOMAIN	0x8
#define USB4_CONTROL_ALL		0xf


struct DomainEvent : DoublyLinkedListLinkImpl<DomainEvent> {
	enum {
		HOTPLUG,
		NOTIFICATION,
		RESCAN,
	}			type;
	uint64		route;
	uint8		adapter;
	bool		unplug;
	uint8		code;
};


struct PCIeTunnelListener {
	virtual	void				PCIeTunnelChanged(Domain* domain,
									bool up) = 0;
};


/*!	A USB4 domain: the host router of one host interface and everything
	connected to it. This is the software connection manager: it
	enumerates routers, handles hot plugs and sets up PCIe, USB3 and
	DisplayPort tunnels.
*/
class Domain {
public:
								Domain(Nhi* nhi, int32 index,
									uint32 control);
								~Domain();

			status_t			Start();
				// starts the connection manager thread
			void				Stop();
			bool				IsRunning() const { return fRunning; }

			int32				Index() const { return fIndex; }
			Nhi*				HostInterface() const { return fNhi; }
			Router*				HostRouter() const { return fHost; }

			bool				MayTunnelUSB3() const
									{ return (fControl & USB4_CONTROL_USB3)
										!= 0; }
			bool				MayTunnelDP() const
									{ return (fControl & USB4_CONTROL_DP)
										!= 0; }
			bool				MayTunnelPCIe() const
									{ return (fControl & USB4_CONTROL_PCIE)
										!= 0; }

			void				SetPCIeListener(PCIeTunnelListener* listener)
									{ fPCIeListener = listener; }

			// configuration access, used by routers and adapters
			ControlResult		RawRead(uint64 route, uint8 adapter,
									usb4_config_space space, uint16 offset,
									uint32* buffer, size_t dwords);
			status_t			ReadConfig(uint64 route, uint8 adapter,
									usb4_config_space space, uint16 offset,
									uint32* buffer, size_t dwords);
			status_t			WriteConfig(uint64 route, uint8 adapter,
									usb4_config_space space, uint16 offset,
									const uint32* buffer, size_t dwords);
			status_t			DeactivateHop(Adapter* adapter, uint32 hop,
									bool clearFlowControl);

			uint32				AvailableCredits(Adapter* lane,
									size_t* maxDPStreams);

			// called from the ring thread
			void				QueueHotplug(uint64 route, uint8 adapter,
									bool unplug);
			void				QueueNotification(uint64 route,
									uint8 adapter, uint8 code);
			void				QueueRescan();
			void				SetControl(uint32 control);
			uint32				Control() const { return fControl; }

			// diagnostics
			size_t				Dump(char* buffer, size_t size);

private:
	static	status_t			_Thread(void* data);
			void				_Loop();
			status_t			_StartDomain();
			void				_StopDomain();
			void				_HandleEvent(DomainEvent* event);
			void				_HandleHotplug(uint64 route, uint8 number,
									bool unplug);
			void				_CheckPendingDP();

			Router*				_FindRouter(uint64 route);
			status_t			_AddRouter(Router* router);
			void				_RemoveRouter(Router* router);

			void				_ScanRouter(Router* router);
			void				_ScanAdapter(Adapter* adapter);
			void				_Unplug(Adapter* adapter);

			void				_TunnelUSB3(Router* router);
			void				_TunnelPCIe(Router* router);
			void				_AddDPResources(Router* router);
			void				_RemoveDPResources(Router* router);
			void				_DPResourceAvailable(Adapter* adapter);
			void				_DPResourceUnavailable(Adapter* adapter,
									const char* reason);
			void				_TunnelDP();
			Adapter*			_FindDPOut(Adapter* in);
			void				_TunnelOneDP(Adapter* in, Adapter* out);
			Tunnel*				_FindTunnel(tunnel_type type, Adapter* source,
									Adapter* destination);
			void				_FreeTunnel(Tunnel* tunnel);
			void				_FreeInvalidTunnels();

			int32				_CollectLinks(Adapter* source,
									Adapter* destination, Adapter** links,
									int32 maxLinks);
			status_t			_AvailableBandwidth(Adapter* source,
									Adapter* destination, int32& up,
									int32& down);
			Tunnel*				_FirstUSB3Tunnel(Adapter* source,
									Adapter* destination);
			void				_ReleaseUSB3Bandwidth(Adapter* source,
									Adapter* destination);
			void				_ReclaimUSB3Bandwidth(Adapter* source,
									Adapter* destination);

private:
			Nhi*				fNhi;
			int32				fIndex;
			uint32				fControl;
			PCIeTunnelListener*	fPCIeListener;

			mutex				fLock;
			Router*				fHost;
			Router*				fRouters[USB4_MAX_ROUTERS];
			TunnelList			fTunnels;
			Adapter*			fDPResources[USB4_MAX_DP_RESOURCES];
			int32				fDPResourceCount;

			struct PendingDP {
				Tunnel*		tunnel;
				bigtime_t	deadline;
			};
			PendingDP			fPendingDP[USB4_MAX_DP_RESOURCES];
			int32				fPendingDPCount;

			spinlock			fEventLock;
			DoublyLinkedList<DomainEvent> fEvents;
			sem_id				fEventSem;
			thread_id			fThread;
			volatile bool		fRunning;
			volatile bool		fStarted;
			status_t			fStartStatus;
};


#endif	// USB4_DOMAIN_H
