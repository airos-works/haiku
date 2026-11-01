/*
 * Copyright 2004-2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, <axeld@pinc-software.de>
 *		Alexander von Gluck, kallisti5@unixzen.com
 *		John Scipione, jscipione@gmail.com
 */
#ifndef INTERFACE_VIEW_H
#define INTERFACE_VIEW_H


#include <GroupView.h>
#include <NetworkInterface.h>
#include <String.h>


class BButton;
class BMenuField;
class BMessage;
class BStringView;
class StatusField;
class ThroughputView;
struct WiFiLinkDetails;


class InterfaceView : public BGroupView {
public:
								InterfaceView();
	virtual						~InterfaceView();

			void				SetTo(const char* name);

	virtual	void				MessageReceived(BMessage* message);
	virtual void				AttachedToWindow();
	virtual	void				Pulse();

private:
			struct Row {
				BStringView*	label;
				BStringView*	value;
			};

			status_t			_Update(bool updateWirelessNetworks = true);
			void				_UpdateStatus();
			void				_UpdateAddresses();
			void				_UpdateLink(const WiFiLinkDetails* wireless);
			void				_UpdateWireless(
									const WiFiLinkDetails* wireless);
			void				_UpdateWirelessMenu(bool isWireless);
			void				_UpdateStats();

			Row					_AddRow(const char* name,
									const char* label);
			void				_SetRow(const Row& row, const BString& value,
									bool visible = true);

	static	bool				_IsPrimaryInterface(
									const BNetworkAddress& address);
	static	BString				_FormatLinkSpeed(uint64 bitsPerSecond);
	static	BString				_DNSServers(BString& searchDomain);

private:
			BNetworkInterface	fInterface;
			int					fPulseCount;

			StatusField*		fStatusField;
			BMenuField*			fNetworkMenuField;

			Row					fAddressRow;
			Row					fIPv6Row;
			Row					fRouteRow;
			Row					fDNSRow;
			Row					fSearchDomainRow;

			Row					fLinkSpeedRow;
			Row					fMacAddressRow;
			Row					fMTURow;

			Row					fSecurityRow;
			Row					fBSSIDRow;
			Row					fChannelRow;
			Row					fRSSIRow;
			Row					fTxRateRow;
			Row					fPhyModeRow;

			Row					fSentRow;
			Row					fReceivedRow;

			ThroughputView*		fThroughputView;

			BButton*			fToggleButton;
			BButton*			fRenegotiateButton;

			BString				fDNSServers;
			BString				fSearchDomain;
			int					fDNSAge;
};


#endif // INTERFACE_VIEW_H
