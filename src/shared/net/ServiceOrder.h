/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SERVICE_ORDER_H
#define _SERVICE_ORDER_H


/*!	The network service order: which interface carries the traffic when more
	than one has a default route, like the service order of macOS.

	The order is a list of interface names in the system settings file
	"network/service_order", one per line, most preferred first. The
	net_server turns it into interface metrics, and the stack prefers the
	default route of the connected interface with the lowest metric.

	Interfaces that are not in the list follow those that are: wired ones
	first, then wireless ones, then everything else. Interfaces of the same
	kind share a metric, so that the stack picks the faster link of them.

	Header only, shared by the net_server and the Network preferences.
*/


#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <NetworkDevice.h>
#include <Path.h>
#include <String.h>
#include <StringList.h>


namespace BPrivate {
namespace ServiceOrder {


enum service_kind {
	SERVICE_KIND_WIRED = 0,
	SERVICE_KIND_WIRELESS,
	SERVICE_KIND_OTHER
};


static const uint32 kListedMetricStep = 10;
static const uint32 kUnlistedMetricBase = 1000;
	// Unlisted wired interfaces get 1000, wireless 2000, others 3000


struct Service {
	BString			name;
	service_kind	kind;
	bool			present;
	bool			listed;
	uint32			metric;
};


static inline service_kind
KindOf(const char* name)
{
	if (strncmp(name, "/dev/net/", 9) != 0)
		return SERVICE_KIND_OTHER;

	BNetworkDevice device(name);
	if (device.IsWireless())
		return SERVICE_KIND_WIRELESS;
	if (device.IsEthernet())
		return SERVICE_KIND_WIRED;
	return SERVICE_KIND_OTHER;
}


static inline status_t
GetPath(BPath& path, bool create = false)
{
	status_t status = find_directory(B_SYSTEM_SETTINGS_DIRECTORY, &path, create);
	if (status != B_OK)
		return status;
	return path.Append("network/service_order");
}


static inline status_t
Read(BStringList& order)
{
	order.MakeEmpty();

	BPath path;
	status_t status = GetPath(path);
	if (status != B_OK)
		return status;

	BFile file(path.Path(), B_READ_ONLY);
	status = file.InitCheck();
	if (status != B_OK)
		return status;

	off_t size;
	status = file.GetSize(&size);
	if (status != B_OK)
		return status;
	if (size > 65536)
		return B_BAD_DATA;

	BString contents;
	char* buffer = contents.LockBuffer(size + 1);
	if (buffer == NULL)
		return B_NO_MEMORY;
	ssize_t bytesRead = file.Read(buffer, size);
	buffer[bytesRead > 0 ? bytesRead : 0] = '\0';
	contents.UnlockBuffer();

	BStringList lines;
	contents.Split("\n", true, lines);
	for (int32 i = 0; i < lines.CountStrings(); i++) {
		BString line = lines.StringAt(i);
		line.Trim();
		if (line.IsEmpty() || line.ByteAt(0) == '#' || order.HasString(line))
			continue;
		order.Add(line);
	}
	return B_OK;
}


static inline status_t
Write(const BStringList& order)
{
	BPath path;
	status_t status = GetPath(path, true);
	if (status != B_OK)
		return status;

	BPath parent;
	if (path.GetParent(&parent) == B_OK)
		create_directory(parent.Path(), 0755);

	BString contents("# Network service order, most preferred first\n");
	for (int32 i = 0; i < order.CountStrings(); i++)
		contents << order.StringAt(i) << "\n";

	BString temporary(path.Path());
	temporary << ".new";
	BFile file(temporary.String(), B_WRITE_ONLY | B_CREATE_FILE
		| B_ERASE_FILE);
	status = file.InitCheck();
	if (status != B_OK)
		return status;
	ssize_t written = file.Write(contents.String(), contents.Length());
	if (written != contents.Length())
		return written < 0 ? (status_t)written : B_IO_ERROR;
	file.Sync();
	file.Unset();

	if (rename(temporary.String(), path.Path()) != 0)
		return errno;
	return B_OK;
}


/*!	Merges the saved \a order with the interfaces that exist now, and
	returns all of them in service order with the metric each should get.
	\a services holds the present interfaces on input (name and kind).
*/
static inline void
Resolve(const BStringList& order, std::vector<Service>& services)
{
	std::vector<Service> result;

	for (int32 i = 0; i < order.CountStrings(); i++) {
		Service service;
		service.name = order.StringAt(i);
		service.kind = SERVICE_KIND_OTHER;
		service.present = false;
		service.listed = true;
		service.metric = (i + 1) * kListedMetricStep;
		for (size_t j = 0; j < services.size(); j++) {
			if (services[j].name == service.name) {
				service.kind = services[j].kind;
				service.present = true;
				break;
			}
		}
		result.push_back(service);
	}

	std::vector<Service> unlisted;
	for (size_t i = 0; i < services.size(); i++) {
		if (order.HasString(services[i].name))
			continue;
		Service service = services[i];
		service.present = true;
		service.listed = false;
		service.metric = kUnlistedMetricBase * (service.kind + 1);
		unlisted.push_back(service);
	}
	std::stable_sort(unlisted.begin(), unlisted.end(),
		[](const Service& a, const Service& b) {
			if (a.kind != b.kind)
				return a.kind < b.kind;
			return strcasecmp(a.name.String(), b.name.String()) < 0;
		});
	result.insert(result.end(), unlisted.begin(), unlisted.end());

	services = result;
}


}	// namespace ServiceOrder
}	// namespace BPrivate


#endif	// _SERVICE_ORDER_H
