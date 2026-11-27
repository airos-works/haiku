/*
 * Copyright 2006-2019, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 * 		Vegard Wærp, vegarwa@online.no
 *		Alexander von Gluck, kallisti5@unixzen.com
 */


#include "NetServer.h"

#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <strings.h>
#include <syslog.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <Alert.h>
#include <Deskbar.h>
#include <Directory.h>
#include <Entry.h>
#include <NetworkDevice.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <NetworkSettings.h>
#include <Path.h>
#include <PathMonitor.h>
#include <Roster.h>
#include <Server.h>
#include <TextView.h>
#include <FindDirectory.h>

#include <AutoDeleter.h>
#include <WPASupplicant.h>

#include "AutoconfigLooper.h"
#include "ResolvConf.h"
#include "Services.h"
#include "../../shared/net/ServiceOrder.h"

extern "C" {
#	include <freebsd_network/compat/sys/cdefs.h>
#	include <freebsd_network/compat/sys/ioccom.h>
#	include <net80211/ieee80211_ioctl.h>
}


using namespace BNetworkKit;


typedef std::map<std::string, AutoconfigLooper*> LooperMap;
typedef std::map<std::string, BMessage> ResolverMap;


class NetServer : public BServer {
public:
								NetServer(status_t& status);
	virtual						~NetServer();

	virtual	void				ReadyToRun();
	virtual	void				MessageReceived(BMessage* message);

private:
			bool				_IsValidFamily(uint32 family);
			bool				_IsValidInterface(BNetworkInterface& interface);
			void				_RemoveInvalidInterfaces();
			status_t			_RemoveInterface(const char* name);
			status_t			_DisableInterface(const char* name);
			bool				_TestForInterface(const char* name);
			status_t			_ConfigureInterface(BMessage& interface);
			status_t			_ConfigureResolver(
									BMessage& resolverConfiguration);
			void				_ForgetResolver(const char* device);
			status_t			_WriteResolverConfiguration();
			bool				_QuitLooperForDevice(const char* device);
			AutoconfigLooper*	_LooperForDevice(const char* device);
			status_t			_ConfigureDevice(const char* path);
			void				_ConfigureDevices(const char* path,
									BStringList& devicesAlreadyConfigured,
									BMessage* suggestedInterface = NULL);
			void				_ConfigureInterfacesFromSettings(
									BStringList& devicesSet,
									BMessage* _missingDevice = NULL);
			void				_ConfigureIPv6LinkLocal(const char* name);

			void				_BringUpInterfaces();
			void				_ApplyServiceOrder();
			status_t			_SetServiceOrder(const BMessage& message);
			void				_StartServices();
			status_t			_HandleDeviceMonitor(BMessage* message);

			status_t			_AutoJoinNetwork(const BMessage& message);
			status_t			_JoinNetwork(const BMessage& message,
									const BNetworkAddress* address = NULL,
									const char* name = NULL);
			status_t			_LeaveNetwork(const BMessage& message);

			status_t			_ConvertNetworkToSettings(BMessage& message);
			status_t			_ConvertNetworkFromSettings(BMessage& message);

private:
			BNetworkSettings	fSettings;
			LooperMap			fDeviceMap;
			ResolverMap			fResolverMap;
				// the resolver settings of each device's lease
			bool				fResolverConfigured;
			BMessenger			fServices;
};


// #pragma mark - private functions


static status_t
set_80211(const char* name, int32 type, void* data,
	int32 length = 0, int32 value = 0)
{
	FileDescriptorCloser socket(::socket(AF_INET, SOCK_DGRAM, 0));
	if (!socket.IsSet())
		return errno;

	struct ieee80211req ireq;
	strlcpy(ireq.i_name, name, IF_NAMESIZE);
	ireq.i_type = type;
	ireq.i_val = value;
	ireq.i_len = length;
	ireq.i_data = data;

	if (ioctl(socket.Get(), SIOCS80211, &ireq, sizeof(struct ieee80211req))
		< 0)
		return errno;

	return B_OK;
}


//	#pragma mark -


NetServer::NetServer(status_t& error)
	:
	BServer(kNetServerSignature, false, &error),
	fResolverConfigured(false)
{
}


NetServer::~NetServer()
{
	BPrivate::BPathMonitor::StopWatching("/dev/net", this);
}


void
NetServer::ReadyToRun()
{
	fSettings.StartMonitoring(this);
	_BringUpInterfaces();
	_StartServices();

	BPrivate::BPathMonitor::StartWatching("/dev/net",
		B_WATCH_FILES_ONLY | B_WATCH_RECURSIVELY, this);
}


void
NetServer::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_PATH_MONITOR:
		{
			fSettings.Update(message);
			_HandleDeviceMonitor(message);
			break;
		}

		case BNetworkSettings::kMsgInterfaceSettingsUpdated:
		{
			BStringList devicesSet;
			_ConfigureInterfacesFromSettings(devicesSet);
			break;
		}

		case BNetworkSettings::kMsgServiceSettingsUpdated:
		{
			BMessage update = fSettings.Services();
			update.what = kMsgUpdateServices;

			fServices.SendMessage(&update);
			break;
		}

		case kMsgConfigureInterface:
		{
			status_t status = _ConfigureInterface(*message);

			BMessage reply(B_REPLY);
			reply.AddInt32("status", status);
			message->SendReply(&reply);
			break;
		}

		case kMsgConfigureResolver:
		{
			status_t status = _ConfigureResolver(*message);

			BMessage reply(B_REPLY);
			reply.AddInt32("status", status);
			message->SendReply(&reply);
			break;
		}

		case kMsgJoinNetwork:
		{
			status_t status = _JoinNetwork(*message);

			BMessage reply(B_REPLY);
			reply.AddInt32("status", status);
			message->SendReply(&reply);
			break;
		}

		case kMsgLeaveNetwork:
		{
			status_t status = _LeaveNetwork(*message);

			BMessage reply(B_REPLY);
			reply.AddInt32("status", status);
			message->SendReply(&reply);
			break;
		}

		case kMsgAutoJoinNetwork:
		{
			_AutoJoinNetwork(*message);
			break;
		}

		case kMsgCountPersistentNetworks:
		{
			BMessage reply(B_REPLY);
			reply.AddInt32("count", fSettings.CountNetworks());
			message->SendReply(&reply);
			break;
		}

		case kMsgGetPersistentNetwork:
		{
			uint32 index = 0;
			status_t result = message->FindInt32("index", (int32*)&index);

			BMessage reply(B_REPLY);
			if (result == B_OK) {
				BMessage network;
				result = fSettings.GetNextNetwork(index, network);
				if (result == B_OK)
					result = reply.AddMessage("network", &network);
			}

			reply.AddInt32("status", result);
			message->SendReply(&reply);
			break;
		}

		case kMsgAddPersistentNetwork:
		{
			BMessage network = *message;
			status_t result = fSettings.AddNetwork(network);

			BMessage reply(B_REPLY);
			reply.AddInt32("status", result);
			message->SendReply(&reply);
			break;
		}

		case kMsgRemovePersistentNetwork:
		{
			const char* networkName = NULL;
			status_t result = message->FindString("name", &networkName);
			if (result == B_OK)
				result = fSettings.RemoveNetwork(networkName);

			BMessage reply(B_REPLY);
			reply.AddInt32("status", result);
			message->SendReply(&reply);
			break;
		}

		case kMsgSetServiceOrder:
		{
			status_t status = _SetServiceOrder(*message);

			BMessage reply(B_REPLY);
			reply.AddInt32("status", status);
			message->SendReply(&reply);
			break;
		}

		case kMsgIsServiceRunning:
		{
			// Forward the message to the handler that can answer it
			BHandler* handler = fServices.Target(NULL);
			if (handler != NULL)
				handler->MessageReceived(message);
			break;
		}

		default:
			BApplication::MessageReceived(message);
			return;
	}
}


/*!	Checks if provided address family is valid.
	Families include AF_INET, AF_INET6, AF_APPLETALK, etc
*/
bool
NetServer::_IsValidFamily(uint32 family)
{
	// Mostly verifies add-on is present
	int socket = ::socket(family, SOCK_DGRAM, 0);
	if (socket < 0)
		return false;

	close(socket);
	return true;
}


/*!	Checks if an interface is valid, that is, if it has an address in any
	family, and, in case of ethernet, a hardware MAC address.
*/
bool
NetServer::_IsValidInterface(BNetworkInterface& interface)
{
	// check if it has an address

	if (interface.CountAddresses() == 0)
		return false;

	// check if it has a hardware address, too, in case of ethernet

	BNetworkAddress link;
	if (interface.GetHardwareAddress(link) != B_OK)
		return false;

	if (link.LinkLevelType() == IFT_ETHER && link.LinkLevelAddressLength() != 6)
		return false;

	return true;
}


void
NetServer::_RemoveInvalidInterfaces()
{
	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;

	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		if (!_IsValidInterface(interface)) {
			// remove invalid interface
			_RemoveInterface(interface.Name());
		}
	}
}


bool
NetServer::_TestForInterface(const char* name)
{

	BNetworkRoster& roster = BNetworkRoster::Default();
	int32 nameLength = strlen(name);
	BNetworkInterface interface;
	uint32 cookie = 0;

	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		if (!strncmp(interface.Name(), name, nameLength))
			return true;
	}

	return false;
}


status_t
NetServer::_RemoveInterface(const char* name)
{
	BNetworkRoster& roster = BNetworkRoster::Default();
	status_t status = roster.RemoveInterface(name);
	_ForgetResolver(name);
	if (status != B_OK) {
		fprintf(stderr, "%s: Could not delete interface %s: %s\n",
			Name(), name, strerror(status));
		return status;
	}

	return B_OK;
}


status_t
NetServer::_DisableInterface(const char* name)
{
	BNetworkInterface interface(name);
	int32 flags = interface.Flags();

	// Set interface down
	flags &= ~(IFF_UP | IFF_AUTO_CONFIGURED | IFF_CONFIGURING);

	status_t status = interface.SetFlags(flags);
	if (status != B_OK) {
		fprintf(stderr, "%s: Setting flags failed: %s\n", Name(),
			strerror(status));
		return status;
	}

	fprintf(stderr, "%s: set %s interface down...\n", Name(), name);
	return B_OK;
}


status_t
NetServer::_ConfigureInterface(BMessage& message)
{
	const char* name;
	if (message.FindString("device", &name) != B_OK)
		return B_BAD_VALUE;

	bool startAutoConfig = false;

	int32 flags;
	if (message.FindInt32("flags", &flags) != B_OK)
		flags = IFF_UP;

	bool autoConfigured = false;
	if (message.FindBool("auto_configured", &autoConfigured) == B_OK
			&& autoConfigured) {
		flags |= IFF_AUTO_CONFIGURED;
	} else {
		_QuitLooperForDevice(name);
	}

	int32 mtu;
	if (message.FindInt32("mtu", &mtu) != B_OK)
		mtu = -1;

	int32 metric;
	if (message.FindInt32("metric", &metric) != B_OK)
		metric = -1;

	BNetworkInterface interface(name);
	if (!interface.Exists()) {
		// the interface does not exist yet, we have to add it first
		BNetworkRoster& roster = BNetworkRoster::Default();

		status_t status = roster.AddInterface(interface);
		if (status != B_OK) {
			fprintf(stderr, "%s: Could not add interface: %s\n",
				interface.Name(), strerror(status));
			return status;
		}
	}

	// Set up IPv6 Link Local address (based on MAC, if not loopback)

	// TODO: our IPv6 stack is still fairly fragile. We need more v6 work
	// (including IPv6 address scope flags before we start attaching link
	// local addresses by default.
	//_ConfigureIPv6LinkLocal(name);

	BMessage addressMessage;
	for (int32 index = 0; message.FindMessage("address", index,
			&addressMessage) == B_OK; index++) {
		BNetworkInterfaceAddressSettings addressSettings(addressMessage);

		if (addressSettings.IsAutoConfigure())
			startAutoConfig = true;

		// set address/mask/broadcast/peer

		if (!addressSettings.Address().IsEmpty()
			|| !addressSettings.Mask().IsEmpty()
			|| !addressSettings.Broadcast().IsEmpty()
			|| !addressSettings.Peer().IsEmpty()
			|| !addressSettings.IsAutoConfigure()) {
			BNetworkInterfaceAddress interfaceAddress;
			interfaceAddress.SetAddress(addressSettings.Address());
			interfaceAddress.SetMask(addressSettings.Mask());
			if (!addressSettings.Broadcast().IsEmpty())
				interfaceAddress.SetBroadcast(addressSettings.Broadcast());
			else if (!addressSettings.Peer().IsEmpty())
				interfaceAddress.SetDestination(addressSettings.Peer());

			status_t status = interface.SetAddress(interfaceAddress);
			if (status != B_OK) {
				fprintf(stderr, "%s: Setting address failed: %s\n", Name(),
					strerror(status));
				return status;
			}
		}

		// set gateway

		if (!addressSettings.Gateway().IsEmpty()) {
			// add gateway route, if we're asked for it
			interface.RemoveDefaultRoute(addressSettings.Family());
				// Try to remove a previous default route, doesn't matter
				// if it fails.

			status_t status = interface.AddDefaultRoute(
				addressSettings.Gateway());
			if (status != B_OK) {
				fprintf(stderr, "%s: Could not add route for %s: %s\n",
					Name(), name, strerror(errno));
			}
		}

		// set flags

		if (flags != 0) {
			int32 newFlags = interface.Flags();
			newFlags = (newFlags & ~IFF_CONFIGURING) | flags;
			if (!autoConfigured)
				newFlags &= ~IFF_AUTO_CONFIGURED;

			status_t status = interface.SetFlags(newFlags);
			if (status != B_OK) {
				fprintf(stderr, "%s: Setting flags failed: %s\n", Name(),
					strerror(status));
			}
		}

		// set options

		if (mtu != -1) {
			status_t status = interface.SetMTU(mtu);
			if (status != B_OK) {
				fprintf(stderr, "%s: Setting MTU failed: %s\n", Name(),
					strerror(status));
			}
		}

		if (metric != -1) {
			status_t status = interface.SetMetric(metric);
			if (status != B_OK) {
				fprintf(stderr, "%s: Setting metric failed: %s\n", Name(),
					strerror(status));
			}
		}
	}

	// Join the specified networks
	BMessage networkMessage;
	for (int32 index = 0; message.FindMessage("network", index,
			&networkMessage) == B_OK; index++) {
		const char* networkName = message.GetString("name", NULL);
		const char* addressString = message.GetString("mac", NULL);

		BNetworkAddress address;
		status_t addressStatus = address.SetTo(AF_LINK, addressString);

		BNetworkDevice device(name);
		if (device.IsWireless() && !device.HasLink()) {
			status_t status = _JoinNetwork(message,
				addressStatus == B_OK ? &address : NULL, networkName);
			if (status != B_OK) {
				fprintf(stderr, "%s: joining network \"%s\" failed: %s\n",
					interface.Name(), networkName, strerror(status));
			}
		}
	}

	if (startAutoConfig) {
		// start auto configuration
		AutoconfigLooper* looper = new AutoconfigLooper(this, name);
		looper->Run();

		fDeviceMap[name] = looper;
	}

	_ApplyServiceOrder();
	return B_OK;
}


/*!	Takes the resolver settings of a device's lease ("device", "nameserver",
	"domain"). A message without name servers and domain removes what the
	device had (its link went away).
*/
status_t
NetServer::_ConfigureResolver(BMessage& resolverConfiguration)
{
	const char* device = resolverConfiguration.GetString("device", "");
	if (!resolverConfiguration.HasString("nameserver")
		&& !resolverConfiguration.HasString("domain")) {
		if (fResolverMap.erase(device) == 0)
			return B_OK;
	} else
		fResolverMap[device] = resolverConfiguration;

	fResolverConfigured = true;
	return _WriteResolverConfiguration();
}


void
NetServer::_ForgetResolver(const char* device)
{
	if (fResolverMap.erase(device) != 0)
		_WriteResolverConfiguration();
}


struct ResolverSource {
	int32		metric;
	std::string	device;
	ResolvConf::Source source;

	bool operator<(const ResolverSource& other) const
	{
		if (metric != other.metric)
			return metric < other.metric;
		return device < other.device;
	}
};


/*!	Writes the name servers of all interfaces that have a link and a lease
	into resolv.conf, the most preferred interface (the lowest metric, which
	is what the service order sets) first. Before, the file held whatever
	the last lease to arrive had sent: the servers of an interface that was
	unplugged since, or of a standby one, while the others were ignored.
	The rest of the file stays as it is; it is replaced in one step, so that
	no reader sees half of it.
*/
status_t
NetServer::_WriteResolverConfiguration()
{
	if (!fResolverConfigured)
		return B_OK;

	BPath path;
	if (find_directory(B_SYSTEM_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append("network/resolv.conf") != B_OK)
		return B_ERROR;

	std::vector<ResolverSource> sources;
	for (ResolverMap::const_iterator iterator = fResolverMap.begin();
			iterator != fResolverMap.end(); iterator++) {
		ResolverSource source;
		source.device = iterator->first;
		source.metric = INT32_MAX;
		if (!source.device.empty()) {
			BNetworkInterface interface(source.device.c_str());
			if (!interface.Exists() || (interface.Flags() & IFF_UP) == 0
				|| !interface.HasLink())
				continue;
			source.metric = interface.Metric();
		}

		const BMessage& message = iterator->second;
		const char* string;
		for (int32 i = 0; message.FindString("nameserver", i, &string)
				== B_OK; i++) {
			source.source.nameservers.push_back(string);
		}
		if (message.FindString("domain", &string) == B_OK)
			source.source.domain = string;
		sources.push_back(source);
	}
	std::stable_sort(sources.begin(), sources.end());

	std::vector<ResolvConf::Source> ordered;
	for (size_t i = 0; i < sources.size(); i++)
		ordered.push_back(sources[i].source);
	std::string block = ResolvConf::MakeBlock(ordered);

	std::string current;
	bool exists = false;
	FILE* file = fopen(path.Path(), "r");
	if (file != NULL) {
		exists = true;
		char buffer[1024];
		size_t bytes;
		while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
			current.append(buffer, bytes);
		fclose(file);
	}

	std::string contents;
	if (!ResolvConf::Merge(current, exists, block, contents)) {
		// "# Static DNS Only"
		return B_OK;
	}
	if (exists && contents == current)
		return B_OK;

	BString temporary(path.Path());
	temporary << ".new";
	int fd = open(temporary.String(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "%s: Could not write %s: %s\n", Name(),
			temporary.String(), strerror(errno));
		return errno;
	}
	ssize_t written = write(fd, contents.c_str(), contents.size());
	status_t status = written == (ssize_t)contents.size() ? B_OK
		: written < 0 ? errno : B_IO_ERROR;
	close(fd);
	if (status == B_OK && rename(temporary.String(), path.Path()) != 0)
		status = errno;
	if (status != B_OK) {
		fprintf(stderr, "%s: Could not write %s: %s\n", Name(), path.Path(),
			strerror(status));
		unlink(temporary.String());
		return status;
	}

	syslog(LOG_INFO, "%s: resolv.conf: %d name server source(s)\n", Name(),
		(int)sources.size());
	return B_OK;
}


bool
NetServer::_QuitLooperForDevice(const char* device)
{
	LooperMap::iterator iterator = fDeviceMap.find(device);
	if (iterator == fDeviceMap.end())
		return false;

	// there is a looper for this device - quit it
	if (iterator->second->Lock())
		iterator->second->Quit();

	fDeviceMap.erase(iterator);

	// its lease is gone, and with it the name servers it brought
	_ForgetResolver(device);
	return true;
}


AutoconfigLooper*
NetServer::_LooperForDevice(const char* device)
{
	LooperMap::const_iterator iterator = fDeviceMap.find(device);
	if (iterator == fDeviceMap.end())
		return NULL;

	return iterator->second;
}


status_t
NetServer::_ConfigureDevice(const char* device)
{
	// bring interface up, but don't configure it just yet
	BMessage interface;
	interface.AddString("device", device);
	BMessage address;
	address.AddString("family", "inet");
	address.AddBool("auto_config", true);
	interface.AddMessage("address", &address);

	return _ConfigureInterface(interface);
}


/*! \brief Traverses the device tree starting from \a startPath, and configures
		everything that has not yet been configured via settings before.

	\param suggestedInterface Contains the configuration of an interface that
		does not have any hardware left. It is used to configure the first
		unconfigured device. This allows to move a Haiku configuration around
		without losing the network configuration.
*/
void
NetServer::_ConfigureDevices(const char* startPath,
	BStringList& devicesAlreadyConfigured, BMessage* suggestedInterface)
{
	BDirectory directory(startPath);
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		struct stat stat;
		BPath path;
		if (entry.GetName(name) != B_OK
			|| entry.GetPath(&path) != B_OK
			|| entry.GetStat(&stat) != B_OK
			|| devicesAlreadyConfigured.HasString(path.Path()))
			continue;

		if (S_ISBLK(stat.st_mode) || S_ISCHR(stat.st_mode)) {
			if (suggestedInterface != NULL
				&& suggestedInterface->SetString("device", path.Path()) == B_OK
				&& _ConfigureInterface(*suggestedInterface) == B_OK)
				suggestedInterface = NULL;
			else
				_ConfigureDevice(path.Path());
		} else if (entry.IsDirectory()) {
			_ConfigureDevices(path.Path(), devicesAlreadyConfigured,
				suggestedInterface);
		}
	}
}


void
NetServer::_ConfigureInterfacesFromSettings(BStringList& devicesSet,
	BMessage* _missingDevice)
{
	BMessage interface;
	uint32 cookie = 0;
	bool missing = false;
	while (fSettings.GetNextInterface(cookie, interface) == B_OK) {
		const char *device;
		if (interface.FindString("device", &device) != B_OK)
			continue;

		bool disabled = false;
		if (interface.FindBool("disabled", &disabled) == B_OK && disabled) {
			// disabled by user request
			_DisableInterface(device);
			continue;
		}

		if (!strncmp(device, "/dev/net/", 9)) {
			// it's a kernel device, check if it's present
			BEntry entry(device);
			if (!entry.Exists()) {
				if (!missing && _missingDevice != NULL) {
					*_missingDevice = interface;
					missing = true;
				}
				continue;
			}
		}

		if (_ConfigureInterface(interface) == B_OK)
			devicesSet.Add(device);
	}
}


void
NetServer::_BringUpInterfaces()
{
	// we need a socket to talk to the networking stack
	if (!_IsValidFamily(AF_LINK)) {
		fprintf(stderr, "%s: The networking stack doesn't seem to be "
			"available.\n", Name());
		Quit();
		return;
	}

	_RemoveInvalidInterfaces();

	// First, we look into the settings, and try to bring everything up from
	// there

	BStringList devicesAlreadyConfigured;
	BMessage missingDevice;
	_ConfigureInterfacesFromSettings(devicesAlreadyConfigured, &missingDevice);

	// Check configuration

	if (!_TestForInterface("loop")) {
		// there is no loopback interface, create one
		BMessage interface;
		interface.AddString("device", "loop");
		BMessage v4address;
		v4address.AddString("family", "inet");
		v4address.AddString("address", "127.0.0.1");
		interface.AddMessage("address", &v4address);

		// Check for IPv6 support and add ::1
		if (_IsValidFamily(AF_INET6)) {
			BMessage v6address;
			v6address.AddString("family", "inet6");
			v6address.AddString("address", "::1");
			interface.AddMessage("address", &v6address);
		}
		_ConfigureInterface(interface);
	}

	// TODO: also check if the networking driver is correctly initialized!
	//	(and check for other devices to take over its configuration)

	// There is no driver configured - see if there is one and try to use it
	_ConfigureDevices("/dev/net", devicesAlreadyConfigured,
		missingDevice.HasString("device") ? &missingDevice : NULL);

	_ApplyServiceOrder();
}


/*!	Gives every interface the metric of its place in the service order, so
	that the stack sends the traffic over the most preferred connected one.
	Interfaces whose settings set a metric explicitly keep that one.
*/
void
NetServer::_ApplyServiceOrder()
{
	using namespace BPrivate::ServiceOrder;

	std::vector<Service> services;
	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;
	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		if ((interface.Flags() & IFF_LOOPBACK) != 0)
			continue;

		Service service;
		service.name = interface.Name();
		service.kind = KindOf(interface.Name());
		services.push_back(service);
	}

	BStringList order;
	Read(order);
	Resolve(order, services);

	for (size_t i = 0; i < services.size(); i++) {
		const Service& service = services[i];
		if (!service.present)
			continue;

		BMessage settings;
		if (fSettings.GetInterface(service.name.String(), settings) == B_OK
			&& settings.HasInt32("metric"))
			continue;

		BNetworkInterface target(service.name.String());
		if (target.Metric() == service.metric)
			continue;

		status_t status = target.SetMetric(service.metric);
		if (status != B_OK) {
			fprintf(stderr, "%s: Setting metric of %s failed: %s\n", Name(),
				service.name.String(), strerror(status));
		} else {
			syslog(LOG_INFO, "%s: service order: %s metric %" B_PRIu32 "\n",
				Name(), service.name.String(), service.metric);
		}
	}
}


status_t
NetServer::_SetServiceOrder(const BMessage& message)
{
	BStringList order;
	const char* name;
	for (int32 index = 0; message.FindString("service", index, &name) == B_OK;
			index++) {
		if (name[0] != '\0' && !order.HasString(name))
			order.Add(name);
	}

	status_t status = BPrivate::ServiceOrder::Write(order);
	if (status != B_OK) {
		fprintf(stderr, "%s: Saving the service order failed: %s\n", Name(),
			strerror(status));
		return status;
	}

	_ApplyServiceOrder();
	_WriteResolverConfiguration();
		// the name servers follow the order of the interfaces
	return B_OK;
}


/*!	Configure the link local address based on the network card's MAC address
	if this isn't a loopback device.
*/
void
NetServer::_ConfigureIPv6LinkLocal(const char* name)
{
	// Check for IPv6 support
	if (!_IsValidFamily(AF_INET6))
		return;

	BNetworkInterface interface(name);

	// Lets make sure this is *not* the loopback interface
	if ((interface.Flags() & IFF_LOOPBACK) != 0)
		return;

	BNetworkAddress link;
	status_t result = interface.GetHardwareAddress(link);

	if (result != B_OK || link.LinkLevelAddressLength() != 6)
		return;

	const uint8* mac = link.LinkLevelAddress();

	// Check for a few failure situations
	static const uint8 zeroMac[6] = {0, 0, 0, 0, 0, 0};
	static const uint8 fullMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	if (memcmp(mac, zeroMac, 6) == 0
		|| memcmp(mac, fullMac, 6) == 0) {
		// Mac address is all 0 or all FF's
		syslog(LOG_DEBUG, "%s: MacAddress for interface '%s' is invalid.",
			__func__, name);
		return;
	}

	// Generate a Link Local Scope address
	// (IPv6 address based on Mac address)
	in6_addr addressRaw;
	memset(addressRaw.s6_addr, 0, sizeof(addressRaw.s6_addr));
	addressRaw.s6_addr[0] = 0xfe;
	addressRaw.s6_addr[1] = 0x80;
	addressRaw.s6_addr[8] = mac[0] ^ 0x02;
	addressRaw.s6_addr[9] = mac[1];
	addressRaw.s6_addr[10] = mac[2];
	addressRaw.s6_addr[11] = 0xff;
	addressRaw.s6_addr[12] = 0xfe;
	addressRaw.s6_addr[13] = mac[3];
	addressRaw.s6_addr[14] = mac[4];
	addressRaw.s6_addr[15] = mac[5];

	BNetworkAddress localLinkAddress(addressRaw, 0);
	BNetworkAddress localLinkMask(
		AF_INET6,
		"ffff:ffff:ffff:ffff::", // 64
		(uint16)0,
		B_UNCONFIGURED_ADDRESS_FAMILIES);
	BNetworkAddress localLinkBroadcast(
		AF_INET6,
		"fe80::ffff:ffff:ffff:ffff",
		(uint16)0,
		B_UNCONFIGURED_ADDRESS_FAMILIES);

	if (interface.FindAddress(localLinkAddress) >= 0) {
		// uhoh... already has a local link address

		/*	TODO: Check for any local link scope addresses assigned to card
			There isn't any flag at the moment though for address scope
		*/
		syslog(LOG_DEBUG, "%s: Local Link address already assigned to %s\n",
			__func__, name);
		return;
	}

	BNetworkInterfaceAddress interfaceAddress;
	interfaceAddress.SetAddress(localLinkAddress);
	interfaceAddress.SetMask(localLinkMask);
	interfaceAddress.SetBroadcast(localLinkMask);

	/*	TODO: Duplicate Address Detection.  (DAD)
		Need to blast an icmp packet over the IPv6 network from :: to ensure
		there aren't duplicate MAC addresses on the network. (definitely an
		edge case, but a possible issue)
	*/

	interface.AddAddress(interfaceAddress);
}


void
NetServer::_StartServices()
{
	BHandler* services = new (std::nothrow) Services(fSettings.Services());
	if (services != NULL) {
		AddHandler(services);
		fServices = BMessenger(services);
	}
}


status_t
NetServer::_HandleDeviceMonitor(BMessage* message)
{
	int32 opcode;
	const char* path;
	if (message->FindInt32("opcode", &opcode) != B_OK
		|| (opcode != B_ENTRY_CREATED && opcode != B_ENTRY_REMOVED)
		|| message->FindString("path", &path) != B_OK)
		return B_BAD_VALUE;

	if (strncmp(path, "/dev/net/", 9)) {
		// not a valid device entry, ignore
		return B_NAME_NOT_FOUND;
	}

	if (opcode == B_ENTRY_CREATED)
		_ConfigureDevice(path);
	else
		_RemoveInterface(path);

	return B_OK;
}


status_t
NetServer::_AutoJoinNetwork(const BMessage& message)
{
	const char* name = NULL;
	if (message.FindString("device", &name) != B_OK)
		return B_BAD_VALUE;

	BNetworkDevice device(name);

	// Choose among configured networks

	uint32 cookie = 0;
	BMessage networkMessage;
	while (fSettings.GetNextNetwork(cookie, networkMessage) == B_OK) {
		status_t status = B_ERROR;
		wireless_network network;
		const char* networkName;
		BNetworkAddress link;

		const char* mac = NULL;
		if (networkMessage.FindString("mac", &mac) == B_OK) {
			link.SetTo(AF_LINK, mac);
			status = device.GetNetwork(link, network);
		} else if (networkMessage.FindString("name", &networkName) == B_OK)
			status = device.GetNetwork(networkName, network);

		if (status == B_OK) {
			status = _JoinNetwork(message, mac != NULL ? &link : NULL,
				network.name);
			printf("auto join network \"%s\": %s\n", network.name,
				strerror(status));
			if (status == B_OK)
				return B_OK;
		}
	}

	return B_NO_INIT;
}


status_t
NetServer::_JoinNetwork(const BMessage& message, const BNetworkAddress* address,
	const char* name)
{
	const char* deviceName;
	if (message.FindString("device", &deviceName) != B_OK)
		return B_BAD_VALUE;

	BNetworkAddress deviceAddress;
	message.FindFlat("address", &deviceAddress);
	if (address == NULL)
		address = &deviceAddress;

	if (name == NULL)
		message.FindString("name", &name);
	if (name == NULL) {
		// No name specified, we need a network address
		if (address->Family() != AF_LINK)
			return B_BAD_VALUE;
	}

	// Search for a network configuration that may override the defaults

	bool found = false;
	uint32 cookie = 0;
	BMessage networkMessage;
	while (fSettings.GetNextNetwork(cookie, networkMessage) == B_OK) {
		const char* networkName;
		if (networkMessage.FindString("name", &networkName) == B_OK
			&& name != NULL && address->Family() != AF_LINK
			&& !strcmp(name, networkName)) {
			found = true;
			break;
		}

		const char* mac;
		if (networkMessage.FindString("mac", &mac) == B_OK
			&& address->Family() == AF_LINK) {
			BNetworkAddress link(AF_LINK, mac);
			if (link == *address) {
				found = true;
				break;
			}
		}
	}

	const char* password;
	if (message.FindString("password", &password) != B_OK && found)
		password = networkMessage.FindString("password");

	// Get network
	BNetworkDevice device(deviceName);
	wireless_network network;

	bool askForConfig = false;
	if ((address->Family() != AF_LINK
			|| device.GetNetwork(*address, network) != B_OK)
		&& device.GetNetwork(name, network) != B_OK) {
		// We did not find a network - just ignore that, and continue
		// with some defaults
		strlcpy(network.name, name != NULL ? name : "", sizeof(network.name));
		network.address = *address;
		network.authentication_mode = B_NETWORK_AUTHENTICATION_NONE;
		network.cipher = 0;
		network.group_cipher = 0;
		network.key_mode = 0;
		askForConfig = true;
	}

	BString string;
	if ((message.FindString("authentication", &string) == B_OK
			&& !string.IsEmpty())
		|| (found && networkMessage.FindString("authentication", &string)
				== B_OK && !string.IsEmpty())) {
		askForConfig = false;
		if (string.ICompare("wpa2") == 0) {
			network.authentication_mode = B_NETWORK_AUTHENTICATION_WPA2;
			network.key_mode = B_KEY_MODE_IEEE802_1X;
			network.cipher = network.group_cipher = B_NETWORK_CIPHER_CCMP;
		} else if (string.ICompare("wpa") == 0) {
			network.authentication_mode = B_NETWORK_AUTHENTICATION_WPA;
			network.key_mode = B_KEY_MODE_IEEE802_1X;
			network.cipher = network.group_cipher = B_NETWORK_CIPHER_TKIP;
		} else if (string.ICompare("wep") == 0) {
			network.authentication_mode = B_NETWORK_AUTHENTICATION_WEP;
			network.key_mode = B_KEY_MODE_NONE;
			network.cipher = network.group_cipher = B_NETWORK_CIPHER_WEP_40;
		} else if (string.ICompare("none") != 0 && string.ICompare("open") != 0) {
			fprintf(stderr, "%s: invalid authentication mode.\n", name);
			askForConfig = true;
		}
	}

	// We always try to join via the wpa_supplicant. Even if we could join
	// ourselves, we need to make sure that the wpa_supplicant knows about
	// our intention, as otherwise it would interfere with it.

	BMessenger wpaSupplicant(kWPASupplicantSignature);
	if (!wpaSupplicant.IsValid()) {
		// The wpa_supplicant isn't running yet, we may join ourselves.
		if (!askForConfig
			&& network.authentication_mode == B_NETWORK_AUTHENTICATION_NONE) {
			// We can join this network ourselves.
			status_t status = set_80211(deviceName, IEEE80211_IOC_SSID,
				network.name, strlen(network.name));
			if (status != B_OK) {
				fprintf(stderr, "%s: joining SSID failed: %s\n", name,
					strerror(status));
				return status;
			}
		}

		// We need the supplicant, try to launch it.
		status_t status = be_roster->Launch(kWPASupplicantSignature);
		if (status != B_OK && status != B_ALREADY_RUNNING)
			return status;

		wpaSupplicant.SetTo(kWPASupplicantSignature);
		if (!wpaSupplicant.IsValid())
			return B_ERROR;
	}

	// TODO: listen to notifications from the supplicant!

	BMessage join(kMsgWPAJoinNetwork);
	status_t status = join.AddString("device", deviceName);
	if (status == B_OK)
		status = join.AddString("name", network.name);
	if (status == B_OK)
		status = join.AddFlat("address", &network.address);
	if (status == B_OK && !askForConfig)
		status = join.AddUInt32("authentication", network.authentication_mode);
	if (status == B_OK && password != NULL)
		status = join.AddString("password", password);
	if (status != B_OK)
		return status;

	status = wpaSupplicant.SendMessage(&join);
	if (status != B_OK)
		return status;

	return B_OK;
}


status_t
NetServer::_LeaveNetwork(const BMessage& message)
{
	const char* deviceName;
	if (message.FindString("device", &deviceName) != B_OK)
		return B_BAD_VALUE;

	int32 reason;
	if (message.FindInt32("reason", &reason) != B_OK)
		reason = IEEE80211_REASON_AUTH_LEAVE;

	// We always try to send the leave request to the wpa_supplicant.

	BMessenger wpaSupplicant(kWPASupplicantSignature);
	if (wpaSupplicant.IsValid()) {
		BMessage leave(kMsgWPALeaveNetwork);
		status_t status = leave.AddString("device", deviceName);
		if (status == B_OK)
			status = leave.AddInt32("reason", reason);
		if (status != B_OK)
			return status;

		status = wpaSupplicant.SendMessage(&leave);
		if (status == B_OK)
			return B_OK;
	}

	// The wpa_supplicant doesn't seem to be running, check if this was an open
	// network we connected ourselves.
	BNetworkDevice device(deviceName);
	wireless_network network;

	uint32 cookie = 0;
	if (device.GetNextAssociatedNetwork(cookie, network) != B_OK
		|| network.authentication_mode != B_NETWORK_AUTHENTICATION_NONE) {
		// We didn't join ourselves, we can't do much.
		return B_ERROR;
	}

	// We joined ourselves, so we can just disassociate again.
	ieee80211req_mlme mlmeRequest;
	memset(&mlmeRequest, 0, sizeof(mlmeRequest));
	mlmeRequest.im_op = IEEE80211_MLME_DISASSOC;
	mlmeRequest.im_reason = reason;

	return set_80211(deviceName, IEEE80211_IOC_MLME, &mlmeRequest,
		sizeof(mlmeRequest));
}


//	#pragma mark -


int
main(int argc, char** argv)
{
	srand(system_time());

	status_t status;
	NetServer server(status);
	if (status != B_OK) {
		fprintf(stderr, "net_server: Failed to create application: %s\n",
			strerror(status));
		return 1;
	}

	server.Run();
	return 0;
}

