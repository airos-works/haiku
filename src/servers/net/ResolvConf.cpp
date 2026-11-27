/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	The part of resolv.conf that the net_server writes from the leases of the
	interfaces. Kept apart from the server so that it can be tested.
*/


#include "ResolvConf.h"

#include <string.h>


namespace ResolvConf {


static const char* kBlockStart = "# Added automatically by DHCP";
static const char* kBlockEnd = "# End of automatic DHCP additions";
static const char* kStaticOnly = "# Static DNS Only";
static const char* kDynamicEntries = "# Dynamic DNS entries";


static bool
starts_with(const std::string& line, const char* prefix)
{
	return line.compare(0, strlen(prefix), prefix) == 0;
}


/*!	The name servers of all sources, in their order, each once, and the
	domain of the first source that has one.
*/
std::string
MakeBlock(const std::vector<Source>& sources)
{
	std::string block = kBlockStart;
	block += "\n";

	std::vector<std::string> written;
	for (size_t i = 0; i < sources.size(); i++) {
		const std::vector<std::string>& servers = sources[i].nameservers;
		for (size_t j = 0; j < servers.size(); j++) {
			bool known = false;
			for (size_t k = 0; k < written.size() && !known; k++)
				known = written[k] == servers[j];
			if (known || servers[j].empty())
				continue;
			written.push_back(servers[j]);
			block += "nameserver " + servers[j] + "\n";
		}
	}

	for (size_t i = 0; i < sources.size(); i++) {
		if (!sources[i].domain.empty()) {
			block += "domain " + sources[i].domain + "\n";
			break;
		}
	}

	block += kBlockEnd;
	block += "\n";
	return block;
}


/*!	The file with \a block in place of the one written before. Lines that
	were not written by the net_server stay where they are. A file that says
	"# Static DNS Only" is left alone (returns \c false); one that has a
	"# Dynamic DNS entries" line (Network preferences writes one) keeps what
	is above that line and gets the block below it, as before.
*/
bool
Merge(const std::string& current, bool exists, const std::string& block,
	std::string& _result)
{
	_result.clear();
	if (!exists) {
		_result = block;
		return true;
	}

	bool inBlock = false;
	bool placed = false;
	size_t position = 0;
	while (position < current.size()) {
		size_t end = current.find('\n', position);
		if (end == std::string::npos)
			end = current.size();
		std::string line = current.substr(position, end - position);
		position = end + 1;

		if (starts_with(line, kStaticOnly))
			return false;
		if (starts_with(line, kDynamicEntries)) {
			_result += line + "\n" + block;
			return true;
		}
		if (inBlock) {
			if (starts_with(line, kBlockEnd))
				inBlock = false;
			continue;
		}
		if (starts_with(line, kBlockStart)) {
			inBlock = true;
			if (!placed) {
				_result += block;
				placed = true;
			}
			continue;
		}
		_result += line + "\n";
	}

	if (!placed)
		_result += block;
	return true;
}


}	// namespace ResolvConf
