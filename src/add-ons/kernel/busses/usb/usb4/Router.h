/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef USB4_ROUTER_H
#define USB4_ROUTER_H


#include <SupportDefs.h>

#include "usb4_regs.h"


class Domain;
class Router;


#define USB4_MAX_HOPS		2048


class HopAllocator {
public:
								HopAllocator();

			int32				Allocate(int32 min, int32 max);
				// returns the hop or -1
			bool				Reserve(int32 hop);
			void				Release(int32 hop);
			void				Clear();

private:
			uint32				fBits[USB4_MAX_HOPS / 32];
};


/*!	An adapter (port) of a router: a lane adapter of a USB4 port, or a
	protocol adapter (PCIe, USB3, DP, host interface).
*/
struct Adapter {
			Router*				router;
			uint8				number;
			bool				present;
			uint32				type;
			uint32				header[8];
				// ADP_CS_0 to ADP_CS_7
			uint16				capLane;
			uint16				capUSB4;
			uint16				capAdapter;
				// protocol adapter capability
			uint16				capTMU;
			uint16				maxInHop;
			uint16				maxOutHop;
			uint16				totalCredits;
			uint16				ctlCredits;

			Adapter*			remote;
			Adapter*			dualLink;
			uint8				linkNumber;
			bool				bonded;
			bool				dpResource;
				// listed as a DP IN or DP OUT resource of the domain

			HopAllocator		inHops;
			HopAllocator		outHops;

			bool				IsLane() const
									{ return type == ADAPTER_LANE; }
			bool				IsDPIn() const
									{ return type == ADAPTER_DP_IN; }
			bool				IsDPOut() const
									{ return type == ADAPTER_DP_OUT; }
			bool				IsPCIeDown() const
									{ return type == ADAPTER_PCIE_DOWN; }
			bool				IsPCIeUp() const
									{ return type == ADAPTER_PCIE_UP; }
			bool				IsUSB3Down() const
									{ return type == ADAPTER_USB3_DOWN; }
			bool				IsUSB3Up() const
									{ return type == ADAPTER_USB3_UP; }
			bool				IsNHI() const
									{ return type == ADAPTER_NHI; }
			bool				IsUpstream() const;

			status_t			Read(usb4_config_space space, uint16 offset,
									uint32* buffer, size_t dwords);
			status_t			Write(usb4_config_space space, uint16 offset,
									const uint32* buffer, size_t dwords);
			status_t			ReadAdapter(uint16 offset, uint32& value)
									{ return Read(USB4_CFG_ADAPTER, offset,
										&value, 1); }
			status_t			WriteAdapter(uint16 offset, uint32 value)
									{ return Write(USB4_CFG_ADAPTER, offset,
										&value, 1); }

			int32				FindCapability(uint8 id);
			int32				State();
				// lane adapter state, or an error
			int32				LinkSpeed();
				// Gb/s per lane
			int32				LinkWidth();
			bool				IsEnabled();
				// protocol adapter enabled
			status_t			UpdateCredits();
			status_t			AddNFCCredits(int32 credits);
			const char*			TypeName() const;
};


enum tmu_mode {
	TMU_MODE_OFF,
	TMU_MODE_LOWRES,
	TMU_MODE_HIFI_UNI,
	TMU_MODE_HIFI_BI,
};


class Router {
public:
								Router(Domain* domain, Router* parent,
									uint64 route);
								~Router();

			Domain*				GetDomain() const { return fDomain; }
			Router*				Parent() const { return fParent; }
			uint64				Route() const { return fRoute; }
			uint8				Depth() const { return fDepth; }
			bool				IsHost() const { return fRoute == 0; }
			bool				IsUSB4() const { return fUSB4; }
			uint32				USB4Version() const
									{ return USB4_VERSION_MAJOR(fConfig[4]
										>> 24); }
			uint16				VendorID() const
									{ return fConfig[0] & 0xffff; }
			uint16				DeviceID() const
									{ return fConfig[0] >> 16; }
			uint8				MaxAdapter() const { return fMaxAdapter; }
			uint8				UpstreamAdapterNumber() const
									{ return fUpstreamAdapter; }
			Adapter*			AdapterAt(uint8 number)
									{ return number <= fMaxAdapter
										? &fAdapters[number] : NULL; }
			Adapter*			UpstreamAdapter()
									{ return AdapterAt(fUpstreamAdapter); }
			Adapter*			DownstreamAdapter();
				// the parent's adapter this router hangs off
			Adapter*			AdapterTowards(uint64 route);
			Adapter*			FindAdapter(uint32 type);
			Adapter*			FindUnusedAdapter(uint32 type);
			Adapter*			MapProtocolDown(Adapter* lane, uint32 type);

			bool				IsUnplugged() const { return fUnplugged; }
			void				SetUnplugged();
				// marks this router and the ones below it

			bool				LinkUSB4() const { return fLinkUSB4; }
			int32				LinkSpeed() const { return fLinkSpeed; }
			int32				LinkWidth() const { return fLinkWidth; }
			uint64				UID() const { return fUID; }
			const char*			VendorName() const { return fVendorName; }
			const char*			DeviceName() const { return fDeviceName; }

			// credits
			bool				CreditAllocation() const
									{ return fCreditAllocation; }
			uint32				MaxUSB3Credits() const
									{ return fMaxUSB3Credits; }
			uint32				MinDPAuxCredits() const
									{ return fMinDPAuxCredits; }
			uint32				MinDPMainCredits() const
									{ return fMinDPMainCredits; }
			uint32				MaxPCIeCredits() const
									{ return fMaxPCIeCredits; }
			uint32				MaxDMACredits() const
									{ return fMaxDMACredits; }

			status_t			ReadConfig(uint16 offset, uint32* buffer,
									size_t dwords);
			status_t			WriteConfig(uint16 offset,
									const uint32* buffer, size_t dwords);
			status_t			ReadConfig(uint16 offset, uint32& value)
									{ return ReadConfig(offset, &value, 1); }
			status_t			WriteConfig(uint16 offset, uint32 value)
									{ return WriteConfig(offset, &value, 1); }
			status_t			WaitForBit(uint16 offset, uint32 bit,
									uint32 value, bigtime_t timeout);

			// enumeration
			status_t			Probe();
				// reads the router and its adapters
			status_t			Configure();
				// enumerates it (writes route, depth, upstream)
			status_t			Setup();
				// capabilities, credits, link state, hot plug
			status_t			SetConfigurationValid();
			status_t			ConfigureLink();
			void				UnconfigureLink();
			status_t			EnableLaneBonding();
			status_t			UnlockAdapter(Adapter* adapter);
			status_t			ResetHost();
			status_t			ResetPort(Adapter* lane);

			// router operations
			status_t			Operation(uint16 opcode, uint32* metadata,
									uint8* opStatus, const uint32* txData,
									size_t txDwords, uint32* rxData,
									size_t rxDwords);
			bool				QueryDPResource(Adapter* in);
			status_t			AllocateDPResource(Adapter* in);
			status_t			DeallocateDPResource(Adapter* in);

			// time management
			status_t			EnableTMU();
			status_t			DisableTMU();
			tmu_mode			TMUMode() const { return fTMUMode; }

private:
			status_t			_InitAdapter(Adapter& adapter);
			int32				_NextCapability(uint16 offset);
			int32				_FindCapability(uint8 id);
			int32				_FindVSECapability(uint8 id);
			status_t			_InitCredits();
			void				_LinkDualPorts();
			void				_ReadDROM();
			status_t			_PostTime();
			status_t			_SetTimeDisruption(bool set);
			status_t			_WriteTMURate(uint32 rate);
			status_t			_SetAdapterTMU(Adapter* adapter,
									uint16 offset, uint32 mask,
									uint32 value);

private:
			Domain*				fDomain;
			Router*				fParent;
			uint64				fRoute;
			uint8				fDepth;
			uint32				fConfig[5];
			uint8				fUpstreamAdapter;
			uint8				fMaxAdapter;
			Adapter*			fAdapters;
			bool				fUSB4;
			bool				fLinkUSB4;
			bool				fUnplugged;
			int32				fLinkSpeed;
			int32				fLinkWidth;
			uint64				fUID;
			char				fVendorName[32];
			char				fDeviceName[48];

			uint16				fCapTMU;
			uint16				fCapPlugEvents;
			uint16				fCapLinkController;
			tmu_mode			fTMUMode;
			bool				fTMUUnidirectional;

			bool				fCreditAllocation;
			uint32				fMaxUSB3Credits;
			uint32				fMinDPAuxCredits;
			uint32				fMinDPMainCredits;
			uint32				fMaxPCIeCredits;
			uint32				fMaxDMACredits;
};


static inline uint8
route_adapter_at(uint64 route, uint8 depth)
{
	return (route >> (depth * 8)) & 0xff;
}


static inline uint8
route_length(uint64 route)
{
	uint8 depth = 0;
	while (route != 0) {
		route >>= 8;
		depth++;
	}
	return depth;
}


#endif	// USB4_ROUTER_H
