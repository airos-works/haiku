/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


/*!	A second address added to an interface that is up (haiku#183, item 5).

	add_interface_address() (SIOCAIFADDR, BNetworkInterface::AddAddress())
	did not add the routes every address needs, the local host route and the
	one to its subnet: those were only added when the interface came up or
	the address was changed with SIOCSIFADDR. A connection from the new
	address to another address of the same host left through the network
	device instead of being delivered locally, and never got an answer.

	The test adds 10.123.45.1/24 to the first interface that is up and has
	an IPv4 address, looks for the two routes, connects from the new address
	to the interface's first address and from there to the new address, and
	removes the address again.
*/


#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <NetworkAddress.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <NetworkRoute.h>
#include <ObjectList.h>


static const char* const kAlias = "10.123.45.1";
static const char* const kAliasMask = "255.255.255.0";
static const char* const kAliasNet = "10.123.45.0";

static int sFailures;


static void
check(bool ok, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("%s: ", ok ? "ok" : "FAILED");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	if (!ok)
		sFailures++;
}


static bool
find_interface(BNetworkInterface& _interface, in_addr& _address)
{
	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;
	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		if ((interface.Flags() & IFF_LOOPBACK) != 0
			|| (interface.Flags() & IFF_UP) == 0) {
			continue;
		}
		for (int32 i = 0; i < interface.CountAddresses(); i++) {
			BNetworkInterfaceAddress address;
			if (interface.GetAddressAt(i, address) != B_OK
				|| address.Address().Family() != AF_INET) {
				continue;
			}
			in_addr inAddress;
			inAddress.s_addr = ((const sockaddr_in&)address.Address()
				.SockAddr()).sin_addr.s_addr;
			if (inAddress.s_addr == INADDR_ANY)
				continue;
			_interface = interface;
			_address = inAddress;
			return true;
		}
	}
	return false;
}


static void
check_routes(const char* interfaceName)
{
	BObjectList<BNetworkRoute, true> routes(8);
	status_t status = BNetworkRoute::GetRoutes(AF_INET, interfaceName,
		routes);
	if (status != B_OK) {
		check(false, "getting the routes: %s", strerror(status));
		return;
	}

	in_addr_t alias = inet_addr(kAlias);
	in_addr_t net = inet_addr(kAliasNet);
	bool hostRoute = false;
	bool netRoute = false;
	for (int32 i = 0; i < routes.CountItems(); i++) {
		const BNetworkRoute* route = routes.ItemAt(i);
		const sockaddr_in* destination
			= (const sockaddr_in*)route->Destination();
		if (destination == NULL || destination->sin_family != AF_INET)
			continue;
		if (destination->sin_addr.s_addr == alias
			&& (route->Flags() & (RTF_LOCAL | RTF_HOST))
				== (RTF_LOCAL | RTF_HOST)) {
			hostRoute = true;
		}
		if (destination->sin_addr.s_addr == net
			&& (route->Flags() & RTF_HOST) == 0) {
			netRoute = true;
		}
	}
	check(hostRoute, "local host route to %s", kAlias);
	check(netRoute, "route to the subnet %s/24", kAliasNet);
}


/*!	Connects a socket bound to \a from to a listener on \a to; returns 0, or
	an error (ETIMEDOUT after 3 seconds).
*/
static int
connect_between(in_addr from, in_addr to)
{
	int result = 0;
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	int client = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0 || client < 0)
		return errno;

	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_len = sizeof(address);
	address.sin_addr = to;
	socklen_t length = sizeof(address);
	if (bind(listener, (sockaddr*)&address, sizeof(address)) != 0
		|| listen(listener, 1) != 0
		|| getsockname(listener, (sockaddr*)&address, &length) != 0) {
		result = errno;
	}

	if (result == 0) {
		sockaddr_in local;
		memset(&local, 0, sizeof(local));
		local.sin_family = AF_INET;
		local.sin_len = sizeof(local);
		local.sin_addr = from;
		if (bind(client, (sockaddr*)&local, sizeof(local)) != 0)
			result = errno;
	}

	if (result == 0) {
		fcntl(client, F_SETFL, fcntl(client, F_GETFL) | O_NONBLOCK);
		if (connect(client, (sockaddr*)&address, sizeof(address)) != 0) {
			if (errno != EINPROGRESS)
				result = errno;
			else {
				pollfd pollFD = { client, POLLOUT, 0 };
				int polled = poll(&pollFD, 1, 3000);
				if (polled == 0)
					result = ETIMEDOUT;
				else if (polled < 0)
					result = errno;
				else {
					int error = 0;
					socklen_t errorLength = sizeof(error);
					getsockopt(client, SOL_SOCKET, SO_ERROR, &error,
						&errorLength);
					result = error;
				}
			}
		}
	}

	close(client);
	close(listener);
	return result;
}


int
main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	alarm(60);

	BNetworkInterface interface;
	in_addr primary;
	if (!find_interface(interface, primary)) {
		printf("skipped: no interface that is up and has an IPv4 address\n");
		return 0;
	}
	printf("interface %s, address %s\n", interface.Name(),
		inet_ntoa(primary));

	BNetworkAddress aliasAddress(AF_INET, kAlias);
	BNetworkAddress aliasMask(AF_INET, kAliasMask);
	if (interface.FindAddress(aliasAddress) >= 0) {
		printf("skipped: %s is already an address of %s\n", kAlias,
			interface.Name());
		return 0;
	}

	BNetworkInterfaceAddress alias;
	alias.SetAddress(aliasAddress);
	alias.SetMask(aliasMask);
	status_t status = interface.AddAddress(alias);
	if (status != B_OK) {
		printf("FAILED: adding %s to %s: %s\n", kAlias, interface.Name(),
			strerror(status));
		return 1;
	}

	check_routes(interface.Name());

	in_addr aliasIn;
	aliasIn.s_addr = inet_addr(kAlias);
	int error = connect_between(aliasIn, primary);
	check(error == 0, "connection from %s to the interface's address: %s",
		kAlias, error == 0 ? "connected" : strerror(error));
	error = connect_between(primary, aliasIn);
	check(error == 0, "connection from the interface's address to %s: %s",
		kAlias, error == 0 ? "connected" : strerror(error));

	status = interface.RemoveAddress(aliasAddress);
	check(status == B_OK, "removing %s again: %s", kAlias, strerror(status));

	// its routes went with it
	BObjectList<BNetworkRoute, true> routes(8);
	int left = 0;
	if (BNetworkRoute::GetRoutes(AF_INET, interface.Name(), routes) == B_OK) {
		for (int32 i = 0; i < routes.CountItems(); i++) {
			const sockaddr_in* destination
				= (const sockaddr_in*)routes.ItemAt(i)->Destination();
			if (destination != NULL
				&& (destination->sin_addr.s_addr == aliasIn.s_addr
					|| destination->sin_addr.s_addr
						== inet_addr(kAliasNet))) {
				left++;
			}
		}
	}
	check(left == 0, "%d routes of %s are left", left, kAlias);

	printf("%s\n", sFailures == 0 ? "PASSED" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
