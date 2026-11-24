/*
 * Copyright 2015, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef UTILITY_H
#define UTILITY_H


#include <String.h>
#include <StringList.h>


namespace Utility {
	bool IsReadOnlyVolume(dev_t device);
	bool IsReadOnlyVolume(const char* path);

	status_t BlockMedia(const char* path, bool block);
	status_t EjectMedia(const char* path);

	BString TranslatePath(const char* path);

	void ParseExportOutput(BStringList& environment, BString& rest,
		const char* data, size_t length);
	void ParseExportVariable(BStringList& environment, const BString& line);
}


#endif // UTILITY_H
