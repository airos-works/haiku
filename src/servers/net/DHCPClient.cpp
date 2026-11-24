/*
 * Copyright 2006-2018, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 *		Vegard Wærp, vegarwa@online.no
 *		Philippe Houdoin, <phoudoin at haiku-os dot org>
 */


#include "DHCPClient.h"

#include <Message.h>
#include <MessageRunner.h>
#include <NetworkDevice.h>
#include <NetworkInterface.h>
#include <NetworkRoute.h>

#include <algorithm>
#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <unistd.h>

#include <Debug.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Looper.h>

#include "NetServer.h"


// See RFC 2131 for DHCP, see RFC 1533 for BOOTP/DHCP options

#define DHCP_CLIENT_PORT	68
#define DHCP_SERVER_PORT	67

#define DEFAULT_TIMEOUT		0.25	// secs
#define MAX_TIMEOUT			64		// secs

#define AS_USECS(t) (1000000 * t)

#define MAX_RETRIES			5

enum message_opcode {
	BOOT_REQUEST = 1,
	BOOT_REPLY
};

enum message_option {
	OPTION_MAGIC = 0x63825363,

	// generic options
	OPTION_PAD = 0,
	OPTION_END = 255,
	OPTION_SUBNET_MASK = 1,
	OPTION_TIME_OFFSET = 2,
	OPTION_ROUTER_ADDRESS = 3,
	OPTION_DOMAIN_NAME_SERVER = 6,
	OPTION_HOST_NAME = 12,
	OPTION_DOMAIN_NAME = 15,
	OPTION_MAX_DATAGRAM_SIZE = 22,
	OPTION_INTERFACE_MTU = 26,
	OPTION_BROADCAST_ADDRESS = 28,
	OPTION_NETWORK_TIME_PROTOCOL_SERVERS = 42,
	OPTION_NETBIOS_NAME_SERVER = 44,
	OPTION_NETBIOS_SCOPE = 47,

	// DHCP specific options
	OPTION_REQUEST_IP_ADDRESS = 50,
	OPTION_ADDRESS_LEASE_TIME = 51,
	OPTION_OVERLOAD = 52,
	OPTION_MESSAGE_TYPE = 53,
	OPTION_SERVER_ADDRESS = 54,
	OPTION_REQUEST_PARAMETERS = 55,
	OPTION_ERROR_MESSAGE = 56,
	OPTION_MAX_MESSAGE_SIZE = 57,
	OPTION_RENEWAL_TIME = 58,
	OPTION_REBINDING_TIME = 59,
	OPTION_CLASS_IDENTIFIER = 60,
	OPTION_CLIENT_IDENTIFIER = 61,
};

enum message_type {
	DHCP_NONE = 0,
	DHCP_DISCOVER = 1,
	DHCP_OFFER = 2,
	DHCP_REQUEST = 3,
	DHCP_DECLINE = 4,
	DHCP_ACK = 5,
	DHCP_NACK = 6,
	DHCP_RELEASE = 7,
	DHCP_INFORM = 8
};

struct dhcp_option_cookie {
	dhcp_option_cookie()
		:
		state(0),
		file_has_options(false),
		server_name_has_options(false)
	{
	}

	const uint8* next;
	uint8	state;
	bool	file_has_options;
	bool	server_name_has_options;
};

struct dhcp_message {
	dhcp_message(message_type type);

	uint8		opcode;
	uint8		hardware_type;
	uint8		hardware_address_length;
	uint8		hop_count;
	uint32		transaction_id;
	uint16		seconds_since_start;
	uint16		flags;
	in_addr_t	client_address;
	in_addr_t	your_address;
	in_addr_t	server_address;
	in_addr_t	gateway_address;
	uint8		mac_address[16];
	uint8		server_name[64];
	uint8		file[128];
	uint32		options_magic;
	uint8		options[1260];

	size_t MinSize() const { return 576; }
	size_t Size() const;

	bool HasOptions() const;
	bool NextOption(dhcp_option_cookie& cookie, message_option& option,
		const uint8*& data, size_t& size) const;
	const uint8* FindOption(message_option which, size_t* _size = NULL) const;
	const uint8* LastOption() const;
	message_type Type() const;

	uint8* PrepareMessage(uint8 type);
	uint8* PutOption(uint8* options, message_option option);
	uint8* PutOption(uint8* options, message_option option, uint8 data);
	uint8* PutOption(uint8* options, message_option option, uint16 data);
	uint8* PutOption(uint8* options, message_option option, uint32 data);
	uint8* PutOption(uint8* options, message_option option, const uint8* data,
		uint32 size);
	uint8* FinishOptions(uint8* options);

	static const char* TypeToString(message_type type);
} _PACKED;

struct socket_timeout {
	socket_timeout(int socket)
		:
		timeout(time_t(AS_USECS(DEFAULT_TIMEOUT))),
		tries(0)
	{
		UpdateSocket(socket);
	}

	bigtime_t timeout; // in micro secs
	uint8 tries;

	bool Shift(int socket, bigtime_t stateMaxTime, const char* device);
	void UpdateSocket(int socket) const;
};

#define DHCP_FLAG_BROADCAST		0x8000

#define ARP_HARDWARE_TYPE_ETHER	1

const uint32 kMsgLeaseTime = 'lstm';

static const uint8 kRequestParameters[] = {
	OPTION_SUBNET_MASK, OPTION_ROUTER_ADDRESS,
	OPTION_DOMAIN_NAME_SERVER, OPTION_BROADCAST_ADDRESS,
	OPTION_DOMAIN_NAME, OPTION_RENEWAL_TIME, OPTION_REBINDING_TIME
};


dhcp_message::dhcp_message(message_type type)
{
	// ASSERT(this == offsetof(this, opcode));
	memset(this, 0, sizeof(*this));
	options_magic = htonl(OPTION_MAGIC);

	uint8* next = PrepareMessage(type);
	FinishOptions(next);
}


bool
dhcp_message::HasOptions() const
{
	return options_magic == htonl(OPTION_MAGIC);
}


bool
dhcp_message::NextOption(dhcp_option_cookie& cookie,
	message_option& option, const uint8*& data, size_t& size) const
{
	if (!HasOptions())
		return false;

	if (cookie.state == 0) {
		cookie.state++;
		cookie.next = options;
	}

	uint32 bytesLeft = 0;

	switch (cookie.state) {
		case 1:
			// options from "options"
			bytesLeft = sizeof(options) - (cookie.next - options);
			break;

		case 2:
			// options from "file"
			bytesLeft = sizeof(file) - (cookie.next - file);
			break;

		case 3:
			// options from "server_name"
			bytesLeft = sizeof(server_name) - (cookie.next - server_name);
			break;
	}

	while (true) {
		if (bytesLeft == 0) {
			cookie.state++;

			// handle option overload in file and/or server_name fields.
			switch (cookie.state) {
				case 2:
					// options from "file" field
					if (cookie.file_has_options) {
						bytesLeft = sizeof(file);
						cookie.next = file;
					}
					break;

				case 3:
					// options from "server_name" field
					if (cookie.server_name_has_options) {
						bytesLeft = sizeof(server_name);
						cookie.next = server_name;
					}
					break;

				default:
					// no more place to look for options
					// if last option is not OPTION_END,
					// there is no space left for other option!
					if (option != OPTION_END)
						cookie.next = NULL;
					return false;
			}

			if (bytesLeft == 0) {
				// no options in this state, try next one
				continue;
			}
		}

		option = (message_option)cookie.next[0];
		if (option == OPTION_END) {
			bytesLeft = 0;
			continue;
		} else if (option == OPTION_PAD) {
			bytesLeft--;
			cookie.next++;
			continue;
		}

		// An option that does not fit into what is left of its field ends
		// the parsing: a damaged or truncated reply made bytesLeft wrap
		// around, and the parsing went on past the message, up the stack
		// (airos-works/haiku#174).
		if (bytesLeft < 2 || (uint32)cookie.next[1] + 2 > bytesLeft) {
			cookie.next = NULL;
			return false;
		}

		size = cookie.next[1];
		data = &cookie.next[2];
		cookie.next += 2 + size;
		bytesLeft -= 2 + size;

		if (option == OPTION_OVERLOAD) {
			if (size >= 1) {
				cookie.file_has_options = data[0] & 1;
				cookie.server_name_has_options = data[0] & 2;
			}
			continue;
		}

		return true;
	}
}


const uint8*
dhcp_message::FindOption(message_option which, size_t* _size) const
{
	dhcp_option_cookie cookie;
	message_option option;
	const uint8* data;
	size_t size;
	while (NextOption(cookie, option, data, size)) {
		// iterate through all options
		if (option == which) {
			if (_size != NULL)
				*_size = size;
			return data;
		}
	}

	return NULL;
}


const uint8*
dhcp_message::LastOption() const
{
	dhcp_option_cookie cookie;
	message_option option;
	const uint8* data;
	size_t size;
	while (NextOption(cookie, option, data, size)) {
		// iterate through all options
	}

	return cookie.next;
}


message_type
dhcp_message::Type() const
{
	size_t size;
	const uint8* data = FindOption(OPTION_MESSAGE_TYPE, &size);
	if (data != NULL && size >= 1)
		return (message_type)data[0];

	return DHCP_NONE;
}


size_t
dhcp_message::Size() const
{
	const uint8* last = LastOption();

	if (last < options) {
		// if last option is stored above "options" field, it means
		// the whole options field and message is already filled...
		return sizeof(dhcp_message);
	}

	return sizeof(dhcp_message) - sizeof(options) + last + 1 - options;
}


uint8*
dhcp_message::PrepareMessage(uint8 type)
{
	uint8* next = options;
	next = PutOption(next, OPTION_MESSAGE_TYPE, type);
	next = PutOption(next, OPTION_MAX_MESSAGE_SIZE,
		(uint16)htons(sizeof(dhcp_message)));
	return next;
}


uint8*
dhcp_message::PutOption(uint8* options, message_option option)
{
	options[0] = option;
	return options + 1;
}


uint8*
dhcp_message::PutOption(uint8* options, message_option option, uint8 data)
{
	return PutOption(options, option, &data, 1);
}


uint8*
dhcp_message::PutOption(uint8* options, message_option option, uint16 data)
{
	return PutOption(options, option, (uint8*)&data, sizeof(data));
}


uint8*
dhcp_message::PutOption(uint8* options, message_option option, uint32 data)
{
	return PutOption(options, option, (uint8*)&data, sizeof(data));
}


uint8*
dhcp_message::PutOption(uint8* options, message_option option,
	const uint8* data, uint32 size)
{
	// TODO: check enough space is available

	options[0] = option;
	options[1] = size;
	memcpy(&options[2], data, size);

	return options + 2 + size;
}


uint8*
dhcp_message::FinishOptions(uint8* options)
{
	return PutOption(options, OPTION_END);
}


/*static*/ const char*
dhcp_message::TypeToString(message_type type)
{
	switch (type) {
#define CASE(x) case x: return #x;
		CASE(DHCP_NONE)
		CASE(DHCP_DISCOVER)
		CASE(DHCP_OFFER)
		CASE(DHCP_REQUEST)
		CASE(DHCP_DECLINE)
		CASE(DHCP_ACK)
		CASE(DHCP_NACK)
		CASE(DHCP_RELEASE)
		CASE(DHCP_INFORM)
#undef CASE
	}

	return "<unknown>";
}


void
socket_timeout::UpdateSocket(int socket) const
{
	struct timeval value;
	value.tv_sec = timeout / 1000000;
	value.tv_usec = timeout % 1000000;
	setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
}


bool
socket_timeout::Shift(int socket, bigtime_t stateMaxTime, const char* device)
{
	if (tries == UINT8_MAX)
		return false;

	tries++;

	if (tries > MAX_RETRIES) {
		bigtime_t now = system_time();
		if (stateMaxTime == -1 || stateMaxTime < now)
			return false;
		bigtime_t remaining = (stateMaxTime - now) / 2 + 1;
		timeout = std::max(remaining, bigtime_t(AS_USECS(60)));
	} else
		timeout += timeout;

	if (timeout > AS_USECS(MAX_TIMEOUT))
		timeout = AS_USECS(MAX_TIMEOUT);

	syslog(LOG_DEBUG, "%s: Timeout shift: %" B_PRIdTIME " msecs (try %" B_PRIu8 ")\n",
		device, timeout / 1000, tries);

	UpdateSocket(socket);
	return true;
}


//	#pragma mark -


DHCPClient::DHCPClient(BMessenger target, const char* device)
	:
	AutoconfigClient("dhcp", target, device),
	fConfiguration(kMsgConfigureInterface),
	fResolverConfiguration(kMsgConfigureResolver),
	fRunner(NULL),
	fNegotiateThread(-1),
	fAssignedAddress(0),
	fServer(AF_INET, NULL, DHCP_SERVER_PORT, B_UNCONFIGURED_ADDRESS_FAMILIES),
	fStartTime(0),
	fRequestTime(0),
	fRenewalTime(0),
	fRebindingTime(0),
	fLeaseTime(0),
	fRenewalDuration(0),
	fRebindingDuration(0),
	fLeaseDuration(0),
	fKeepLease(false)
{
	fTransactionID = (uint32)system_time() ^ rand();

	BNetworkAddress link;
	BNetworkInterface interface(device);
	fStatus = interface.GetHardwareAddress(link);
	if (fStatus != B_OK)
		return;

	memcpy(fMAC, link.LinkLevelAddress(), sizeof(fMAC));

	if ((interface.Flags() & IFF_AUTO_CONFIGURED) != 0) {
		// Check for interface previous auto-configured address, if any.
		BNetworkInterfaceAddress interfaceAddress;
		int index = interface.FindFirstAddress(AF_INET);
		if (index >= 0
			&& interface.GetAddressAt(index, interfaceAddress) == B_OK) {
			BNetworkAddress address = interfaceAddress.Address();
			const sockaddr_in& addr = (sockaddr_in&)address.SockAddr();
			fAssignedAddress = addr.sin_addr.s_addr;

			if ((ntohl(fAssignedAddress) & IN_CLASSB_NET) == 0xa9fe0000) {
				// previous auto-configured address is a link-local one:
				// there is no point asking a DHCP server to renew such
				// server-less assigned address...
				fAssignedAddress = 0;
			}
		}
	}
}


DHCPClient::~DHCPClient()
{
	thread_id thread = fNegotiateThread;
	if (thread != -1) {
		fNegotiateThread = -1;
		suspend_thread(thread);
		resume_thread(thread);

		UnlockLooper();
		wait_for_thread(thread, NULL);
		LockLooper();
	}

	delete fRunner;

	// (without a link it could not be sent anyway, and the address is
	// asked for again once the link is back: a sleep or a roam keeps it)
	if (fStatus != B_OK || fKeepLease)
		return;

	int socket = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (socket < 0)
		return;

	// release lease

	dhcp_message release(DHCP_RELEASE);
	_PrepareMessage(release, BOUND);

	_SendMessage(socket, release, fServer);
	close(socket);
}


status_t
DHCPClient::Start()
{
	if (fNegotiateThread != -1)
		return EINPROGRESS;

	fNegotiateThread = spawn_thread(_NegotiatorThread, "DHCP negotiator",
		B_NORMAL_PRIORITY, this);
	return resume_thread(fNegotiateThread);
}


status_t
DHCPClient::_NegotiatorThread(void* data)
{
	openlog_thread("DHCP", 0, LOG_DAEMON);

	DHCPClient* client = (DHCPClient*)data;
	client->LockLooper();
	client->fStatus = client->_Negotiate();
	syslog(LOG_DEBUG, "%s: DHCP status = %s\n", client->Device(), strerror(client->fStatus));
	client->fNegotiateThread = -1;
	client->UnlockLooper();

	closelog_thread();
	return B_OK;
}


status_t
DHCPClient::_Negotiate()
{
	dhcp_state state = _CurrentState();
	if (state == BOUND)
		return B_OK;

	fStartTime = system_time();
	fTransactionID++;

	char hostName[MAXHOSTNAMELEN];
	if (gethostname(hostName, MAXHOSTNAMELEN) == 0)
		fHostName.SetTo(hostName, MAXHOSTNAMELEN);
	else
		fHostName.Truncate(0);

	int socket = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (socket < 0)
		return errno;

	// Enable reusing the port. This is needed in case there is more
	// than 1 interface that needs to be configured. Note that the only reason
	// this works is because there is code below to bind to a specific
	// interface.
	int option = 1;
	setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option));

	BNetworkAddress local;
	local.SetToWildcard(AF_INET, DHCP_CLIENT_PORT);

	option = 1;
	setsockopt(socket, SOL_SOCKET, SO_BROADCAST, &option, sizeof(option));

	if (bind(socket, local, local.Length()) < 0) {
		close(socket);
		return errno;
	}

	// Only this negotiation's reply counts. The times used to be kept as
	// the absolute times of the previous lease when the server sent no T1
	// and T2 (options 58 and 59), and the time of the request was added to
	// them once more: every renewal came twice as late as the one before,
	// long after the lease had ended (airos-works/haiku#174).
	bigtime_t previousLeaseDuration = fLeaseDuration;
	fLeaseDuration = 0;
	fRenewalDuration = 0;
	fRebindingDuration = 0;

	status_t status = B_OK;
	while (state != BOUND && fNegotiateThread != -1) {
		status = _StateTransition(socket, state);
		if (status != B_OK && (state == SELECTING || state == REBOOTING))
			break;
	}

	close(socket);

	if (fLeaseDuration == 0)
		fLeaseDuration = previousLeaseDuration;
	if (fLeaseDuration == 0)
		fLeaseDuration = AS_USECS(60);

	// T1 and T2 default to 1/2 and 7/8 of the lease (RFC 2131 4.4.5)
	if (fRenewalDuration == 0 || fRenewalDuration > fLeaseDuration)
		fRenewalDuration = fLeaseDuration / 2;
	if (fRebindingDuration == 0 || fRebindingDuration > fLeaseDuration)
		fRebindingDuration = fLeaseDuration * 7 / 8;
	if (fRebindingDuration < fRenewalDuration)
		fRebindingDuration = fRenewalDuration;

	fLeaseTime = fRequestTime + fLeaseDuration;
	fRenewalTime = fRequestTime + fRenewalDuration;
	fRebindingTime = fRequestTime + fRebindingDuration;
	syslog(LOG_DEBUG, "%s: renewal in %" B_PRIdBIGTIME " s, rebinding in %"
		B_PRIdBIGTIME " s, lease ends in %" B_PRIdBIGTIME " s\n", Device(),
		(fRenewalTime - system_time()) / 1000000,
		(fRebindingTime - system_time()) / 1000000,
		(fLeaseTime - system_time()) / 1000000);
	_RestartLease(fRenewalTime);

	fStatus = status;
	if (status != B_OK) {
		// If we were asked to quit, don't inform the looper that we failed.
		if (fNegotiateThread != -1)
			Looper()->PostMessage(kMsgAutoConfigureFailed, Looper(), this);
		return status;
	}

	return _SendConfiguration();
}


/*!	Hands the lease to the net_server: interface address, mask, broadcast
	and default route, then the resolver configuration.
*/
status_t
DHCPClient::_SendConfiguration()
{
	// configure interface
	BMessage reply;
	status_t status = Target().SendMessage(&fConfiguration, &reply);
	if (status == B_OK)
		status = reply.FindInt32("status", &fStatus);

	// configure resolver
	reply.MakeEmpty();
	fResolverConfiguration.RemoveName("device");
	fResolverConfiguration.AddString("device", Device());
	status = Target().SendMessage(&fResolverConfiguration, &reply);
	if (status == B_OK)
		status = reply.FindInt32("status", &fStatus);
	return status;
}


/*!	Called by the looper whenever the interface may have lost what the lease
	configured (it went down and up, its link came back without a down
	notification) and periodically. Taking an interface down deletes all of
	its routes, and only the lease puts the default route back: without this
	the interface stays reachable on the local network, but every other
	destination silently goes out over another interface, or nowhere.
*/
void
DHCPClient::CheckConfiguration()
{
	if (fStatus != B_OK || fNegotiateThread != -1 || fAssignedAddress == 0)
		return;

	BMessage address;
	if (fConfiguration.FindMessage("address", &address) != B_OK)
		return;

	BNetworkInterface interface(Device());
	if (!interface.Exists() || (interface.Flags() & IFF_UP) == 0)
		return;

	if ((interface.Flags() & IFF_AUTO_CONFIGURED) == 0) {
		// Someone configured the interface by hand (ifconfig clears the
		// flag on purpose): putting the lease back undid that within
		// seconds (airos-works/haiku#174).
		return;
	}

	bool hasAddress = false;
	int32 count = interface.CountAddresses();
	for (int32 i = 0; i < count && !hasAddress; i++) {
		BNetworkInterfaceAddress interfaceAddress;
		if (interface.GetAddressAt(i, interfaceAddress) != B_OK)
			continue;
		const sockaddr& local = interfaceAddress.Address().SockAddr();
		if (local.sa_family == AF_INET
			&& ((const sockaddr_in&)local).sin_addr.s_addr
				== fAssignedAddress)
			hasAddress = true;
	}

	if (!hasAddress) {
		syslog(LOG_NOTICE, "%s: leased address %s is gone, configuring the "
			"interface again\n", Device(),
			_AddressToString(fAssignedAddress).String());
		_SendConfiguration();
		return;
	}

	const char* gateway = address.GetString("gateway", NULL);
	if (gateway == NULL)
		return;

	BNetworkRoute route;
	if (BNetworkRoute::GetDefaultRoute(AF_INET, Device(), route)
			!= B_ENTRY_NOT_FOUND)
		return;

	BNetworkAddress gatewayAddress;
	if (gatewayAddress.SetTo(AF_INET, gateway) != B_OK)
		return;

	status_t status = interface.AddDefaultRoute(gatewayAddress);
	syslog(LOG_NOTICE, "%s: default route via %s was missing, added it "
		"again: %s\n", Device(), gateway, strerror(status));
}


status_t
DHCPClient::_GotMessage(dhcp_state& state, dhcp_message* message)
{
	switch (state) {
		case SELECTING:
			if (message->Type() == DHCP_OFFER) {
				state = REQUESTING;

				fAssignedAddress = message->your_address;
				syslog(LOG_INFO, "  your_address: %s\n",
						_AddressToString(fAssignedAddress).String());

				fConfiguration.MakeEmpty();
				fConfiguration.AddString("device", Device());
				fConfiguration.AddBool("auto_configured", true);

				BMessage address;
				address.AddString("family", "inet");
				address.AddString("address", _AddressToString(fAssignedAddress));
				fResolverConfiguration.MakeEmpty();
				_ParseOptions(*message, address, fResolverConfiguration);

				fConfiguration.AddMessage("address", &address);
				return B_OK;
			}

			return B_BAD_VALUE;

		case REBOOTING:
		case REBINDING:
		case RENEWING:
		case REQUESTING:
			if (message->Type() == DHCP_ACK) {
				// The ACK is authoritative: configure from it. A client that
				// starts in INIT-REBOOT (the link came back, the lease was
				// kept) never saw an OFFER, so this is the only place its
				// configuration (and with it the default route) comes from.
				if (message->your_address != 0)
					fAssignedAddress = message->your_address;

				BMessage address;
				address.AddString("family", "inet");
				address.AddString("address",
					_AddressToString(fAssignedAddress));
				fResolverConfiguration.MakeEmpty();
				_ParseOptions(*message, address, fResolverConfiguration);

				fConfiguration.MakeEmpty();
				fConfiguration.AddString("device", Device());
				fConfiguration.AddBool("auto_configured", true);
				fConfiguration.AddMessage("address", &address);

				// our address request has been acknowledged
				state = BOUND;

				return B_OK;
			}

			if (message->Type() == DHCP_NACK) {
				// server reject our request on previous assigned address
				// back to square one...
				fAssignedAddress = 0;
				state = INIT;
				return B_OK;
			}

		default:
			return B_BAD_VALUE;
	}
}


status_t
DHCPClient::_StateTransition(int socket, dhcp_state& state)
{
	if (state == INIT) {
		// The local interface does not have an address yet, bind the socket
		// to the device directly.
		BNetworkDevice device(Device());
		int index = device.Index();

		setsockopt(socket, SOL_SOCKET, SO_BINDTODEVICE, &index, sizeof(int));
	}

	BNetworkAddress broadcast;
	broadcast.SetToBroadcast(AF_INET, DHCP_SERVER_PORT);

	socket_timeout timeout(socket);

	dhcp_message discover(DHCP_DISCOVER);
	_PrepareMessage(discover, state);

	dhcp_message request(DHCP_REQUEST);
	_PrepareMessage(request, state);

	bool skipRequest = false;
	dhcp_state originalState = state;
	fRequestTime = system_time();
	while (true) {
		if (!skipRequest) {
			_SendMessage(socket, originalState == INIT ? discover : request,
				originalState == RENEWING ? fServer : broadcast);

			if (originalState == INIT)
				state = SELECTING;
			else if (originalState == INIT_REBOOT)
				state = REBOOTING;
		}

		char buffer[2048];
		struct sockaddr_in from;
		socklen_t fromLength = sizeof(from);

		UnlockLooper();
		ssize_t bytesReceived = recvfrom(socket, buffer, sizeof(buffer),
			0, (struct sockaddr*)&from, &fromLength);
		LockLooper();

		if (bytesReceived < 0 && errno == B_TIMED_OUT) {
			// depending on the state, we'll just try again
			if (!_TimeoutShift(socket, state, timeout))
				return B_TIMED_OUT;
			skipRequest = false;
			continue;
		} else if (bytesReceived < 0)
			return errno;

		skipRequest = true;
		if ((size_t)bytesReceived < offsetof(dhcp_message, options)) {
			syslog(LOG_DEBUG, "%s: Ignoring %zd bytes from %s: too short\n",
				Device(), bytesReceived,
				_AddressToString(from.sin_addr.s_addr).String());
			continue;
		}

		// What was not received reads as padding, which ends the options:
		// they were read on into what an earlier reply, or the stack, had
		// left in the buffer (airos-works/haiku#174).
		static_assert(sizeof(buffer) >= sizeof(dhcp_message),
			"the buffer holds a whole message");
		if ((size_t)bytesReceived < sizeof(dhcp_message)) {
			memset(buffer + bytesReceived, OPTION_PAD,
				sizeof(dhcp_message) - bytesReceived);
		}

		dhcp_message* message = (dhcp_message*)buffer;
		if (message->transaction_id != htonl(fTransactionID)
			|| !message->HasOptions()
			|| memcmp(message->mac_address, discover.mac_address,
				discover.hardware_address_length)) {
			// this message is not for us
			syslog(LOG_DEBUG, "%s: Ignoring %s not for us from %s\n",
				Device(), dhcp_message::TypeToString(message->Type()),
				_AddressToString(from.sin_addr.s_addr).String());
			continue;
		}

		syslog(LOG_DEBUG, "%s: Received %s from %s\n",
			Device(), dhcp_message::TypeToString(message->Type()),
			_AddressToString(from.sin_addr.s_addr).String());

		if (_GotMessage(state, message) == B_OK)
			break;
	}

	return B_OK;
}


void
DHCPClient::_RestartLease(bigtime_t leaseTime)
{
	if (leaseTime == 0)
		return;

	delete fRunner;
		// (it sent its one message already)

	BMessage lease(kMsgLeaseTime);
	fRunner = new BMessageRunner(this, &lease,
		max_c(leaseTime - system_time(), (bigtime_t)AS_USECS(1)), 1);
}


void
DHCPClient::_ParseOptions(dhcp_message& message, BMessage& address,
	BMessage& resolverConfiguration)
{
	dhcp_option_cookie cookie;
	message_option option;
	const uint8* data;
	size_t size;
	while (message.NextOption(cookie, option, data, size)) {
		// iterate through all options
		switch (option) {
			case OPTION_ROUTER_ADDRESS:
			case OPTION_SUBNET_MASK:
			case OPTION_BROADCAST_ADDRESS:
			case OPTION_SERVER_ADDRESS:
			case OPTION_ADDRESS_LEASE_TIME:
			case OPTION_RENEWAL_TIME:
			case OPTION_REBINDING_TIME:
				// an address or a time: four bytes (a list of routers
				// starts with the first one)
				if (size < 4) {
					syslog(LOG_DEBUG, "  option %" B_PRIu32 " too short (%"
						B_PRIuSIZE " bytes)\n", (uint32)option, size);
					continue;
				}
				break;
			default:
				break;
		}

		switch (option) {
			case OPTION_ROUTER_ADDRESS:
				syslog(LOG_DEBUG, "  gateway: %s\n",
					_AddressToString(data).String());
				address.AddString("gateway", _AddressToString(data));
				break;
			case OPTION_SUBNET_MASK:
				syslog(LOG_DEBUG, "  subnet: %s\n",
					_AddressToString(data).String());
				address.AddString("mask", _AddressToString(data));
				break;
			case OPTION_BROADCAST_ADDRESS:
				syslog(LOG_DEBUG, "  broadcast: %s\n",
					_AddressToString(data).String());
				address.AddString("broadcast", _AddressToString(data));
				break;
			case OPTION_DOMAIN_NAME_SERVER:
			{
				for (uint32 i = 0; i < size / 4; i++) {
					syslog(LOG_DEBUG, "  nameserver[%d]: %s\n", i,
						_AddressToString(&data[i * 4]).String());
					resolverConfiguration.AddString("nameserver",
						_AddressToString(&data[i * 4]).String());
				}
				resolverConfiguration.AddInt32("nameserver_count",
					size / 4);
				break;
			}
			case OPTION_SERVER_ADDRESS:
			{
				syslog(LOG_DEBUG, "  server: %s\n",
					_AddressToString(data).String());
				status_t status = fServer.SetAddress(*(in_addr_t*)data);
				if (status != B_OK) {
					syslog(LOG_ERR, "   BNetworkAddress::SetAddress failed with %s!\n",
						strerror(status));
					fServer.Unset();
				}
				break;
			}

			case OPTION_ADDRESS_LEASE_TIME:
				syslog(LOG_DEBUG, "  lease time: %lu seconds\n",
					ntohl(*(uint32*)data));
				fLeaseDuration = ntohl(*(uint32*)data) * 1000000LL;
				break;
			case OPTION_RENEWAL_TIME:
				syslog(LOG_DEBUG, "  renewal time: %lu seconds\n",
					ntohl(*(uint32*)data));
				fRenewalDuration = ntohl(*(uint32*)data) * 1000000LL;
				break;
			case OPTION_REBINDING_TIME:
				syslog(LOG_DEBUG, "  rebinding time: %lu seconds\n",
					ntohl(*(uint32*)data));
				fRebindingDuration = ntohl(*(uint32*)data) * 1000000LL;
				break;

			case OPTION_HOST_NAME:
				syslog(LOG_DEBUG, "  host name: \"%.*s\"\n", (int)size,
					(const char*)data);
				break;

			case OPTION_DOMAIN_NAME:
			{
				// not NUL terminated: strlcpy() read on past the option
				char domain[256];
				size_t length = min_c(size, sizeof(domain) - 1);
				memcpy(domain, data, length);
				domain[length] = '\0';

				syslog(LOG_DEBUG, "  domain name: \"%s\"\n", domain);

				resolverConfiguration.AddString("domain", domain);
				break;
			}

			case OPTION_MESSAGE_TYPE:
				break;

			case OPTION_ERROR_MESSAGE:
				syslog(LOG_INFO, "  error message: \"%.*s\"\n", (int)size,
					(const char*)data);
				break;

			default:
				syslog(LOG_DEBUG, "  UNKNOWN OPTION %" B_PRIu32 " (0x%" B_PRIx32 ")\n",
					(uint32)option, (uint32)option);
				break;
		}
	}
}


void
DHCPClient::_PrepareMessage(dhcp_message& message, dhcp_state state)
{
	message.opcode = BOOT_REQUEST;
	message.hardware_type = ARP_HARDWARE_TYPE_ETHER;
	message.hardware_address_length = 6;
	message.transaction_id = htonl(fTransactionID);
	message.seconds_since_start = htons(min_c((system_time() - fStartTime)
		/ 1000000LL, 65535));
	memcpy(message.mac_address, fMAC, 6);

	message_type type = message.Type();

	uint8 *next = message.PrepareMessage(type);

	switch (type) {
		case DHCP_DISCOVER:
			next = message.PutOption(next, OPTION_REQUEST_PARAMETERS,
				kRequestParameters, sizeof(kRequestParameters));

			if (fHostName.Length() > 0) {
				next = message.PutOption(next, OPTION_HOST_NAME,
					reinterpret_cast<const uint8*>(fHostName.String()),
					fHostName.Length());
			}
			break;

		case DHCP_REQUEST:
			next = message.PutOption(next, OPTION_REQUEST_PARAMETERS,
				kRequestParameters, sizeof(kRequestParameters));

			if (fHostName.Length() > 0) {
				next = message.PutOption(next, OPTION_HOST_NAME,
					reinterpret_cast<const uint8*>(fHostName.String()),
					fHostName.Length());
			}

			if (state == REQUESTING) {
				const sockaddr_in& server = (sockaddr_in&)fServer.SockAddr();
				next = message.PutOption(next, OPTION_SERVER_ADDRESS,
					(uint32)server.sin_addr.s_addr);
			}

			if (state == INIT || state == INIT_REBOOT
				|| state == REQUESTING) {
				next = message.PutOption(next, OPTION_REQUEST_IP_ADDRESS,
					(uint32)fAssignedAddress);
			} else
				message.client_address = fAssignedAddress;
			break;

		case DHCP_RELEASE: {
			const sockaddr_in& server = (sockaddr_in&)fServer.SockAddr();
			next = message.PutOption(next, OPTION_SERVER_ADDRESS,
				(uint32)server.sin_addr.s_addr);

			message.client_address = fAssignedAddress;
			break;
		}

		default:
			break;
	}

	message.FinishOptions(next);
}


bool
DHCPClient::_TimeoutShift(int socket, dhcp_state& state,
	socket_timeout& timeout)
{
	bigtime_t stateMaxTime = -1;

	// Compute the date at which we must consider the DHCP negociation failed.
	// This varies depending on the current state. In renewing and rebinding
	// states, it is based on the lease expiration.
	// We can stay for up to 1 minute in the selecting and requesting states
	// (these correspond to sending DHCP_DISCOVER and DHCP_REQUEST,
	// respectively).
	// All other states time out immediately after a single try.
	// If this timeout expires, the DHCP negociation is aborted and starts
	// over. As long as the timeout is not expired, we repeat the message with
	// increasing delays (the delay is computed in timeout.shift below, and is
	// at most equal to the timeout, but usually much shorter).
	if (state == RENEWING)
		stateMaxTime = fRebindingTime;
	else if (state == REBINDING)
		stateMaxTime = fLeaseTime;
	else if (state == SELECTING || state == REQUESTING)
		stateMaxTime = fRequestTime + AS_USECS(MAX_TIMEOUT);

	if (system_time() > stateMaxTime) {
		state = state == REBINDING ? INIT : REBINDING;
		return false;
	}

	return timeout.Shift(socket, stateMaxTime, Device());
}


/*static*/ BString
DHCPClient::_AddressToString(const uint8* data)
{
	BString target = inet_ntoa(*(in_addr*)data);
	return target;
}


/*static*/ BString
DHCPClient::_AddressToString(in_addr_t address)
{
	BString target = inet_ntoa(*(in_addr*)&address);
	return target;
}


status_t
DHCPClient::_SendMessage(int socket, dhcp_message& message,
	const BNetworkAddress& address) const
{
	message_type type = message.Type();
	BString text;
	text << dhcp_message::TypeToString(type);

	const uint8* requestAddress = message.FindOption(OPTION_REQUEST_IP_ADDRESS);
	if (type == DHCP_REQUEST && requestAddress != NULL)
		text << " for " << _AddressToString(requestAddress).String();

	syslog(LOG_DEBUG, "%s: Send %s to %s\n", Device(), text.String(),
		address.ToString().String());

	ssize_t bytesSent = sendto(socket, &message, message.Size(),
		address.IsBroadcast() ? MSG_BCAST : 0, address, address.Length());
	if (bytesSent < 0)
		return errno;

	return B_OK;
}


dhcp_state
DHCPClient::_CurrentState() const
{
	bigtime_t now = system_time();

	if (now > fLeaseTime || fStatus != B_OK)
		return fAssignedAddress == 0 ? INIT : INIT_REBOOT;
	if (now >= fRebindingTime)
		return REBINDING;
	if (now >= fRenewalTime)
		return RENEWING;
	return BOUND;
}


void
DHCPClient::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgLeaseTime:
		{
			BNetworkInterface interface(Device());
			if ((interface.Flags() & IFF_AUTO_CONFIGURED) == 0) {
				// configured by hand since: the renewal would put the
				// lease back (see CheckConfiguration())
				syslog(LOG_INFO, "%s: not renewing the lease, the interface "
					"was configured by hand\n", Device());
				break;
			}
			Start();
			break;
		}

		default:
			BHandler::MessageReceived(message);
			break;
	}
}
