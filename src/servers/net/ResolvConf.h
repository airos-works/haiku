/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RESOLV_CONF_H
#define RESOLV_CONF_H


#include <string>
#include <vector>


namespace ResolvConf {


struct Source {
	std::vector<std::string>	nameservers;
	std::string					domain;
};


std::string	MakeBlock(const std::vector<Source>& sources);
bool		Merge(const std::string& current, bool exists,
				const std::string& block, std::string& _result);


}	// namespace ResolvConf


#endif	// RESOLV_CONF_H
