/*
 * Copyright 2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */


#include "OldUnpackingNodeAttributes.h"

#include "PackageNode.h"


OldUnpackingNodeAttributes::OldUnpackingNodeAttributes(
	PackageNode* packageNode)
	:
	fPackageNode(packageNode),
	fModifiedTime(),
	fHasModifiedTime(false)
{
}


/*!	For a directory, whose time is not its head package node's but the
	latest of all its packages (UnpackingDirectory::ModifiedTime()).
*/
OldUnpackingNodeAttributes::OldUnpackingNodeAttributes(
	PackageNode* packageNode, const timespec& modifiedTime)
	:
	fPackageNode(packageNode),
	fModifiedTime(modifiedTime),
	fHasModifiedTime(true)
{
}


timespec
OldUnpackingNodeAttributes::ModifiedTime() const
{
	if (fHasModifiedTime)
		return fModifiedTime;

	if (fPackageNode != NULL)
		return fPackageNode->ModifiedTime();

	timespec time = { 0, 0 };
	return time;
}


off_t
OldUnpackingNodeAttributes::FileSize() const
{
	return fPackageNode != NULL ? fPackageNode->FileSize() : 0;
}


void*
OldUnpackingNodeAttributes::IndexCookieForAttribute(const StringKey& name) const
{
	return fPackageNode != NULL
		? fPackageNode->IndexCookieForAttribute(name) : NULL;
}
