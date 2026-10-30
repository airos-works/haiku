/*
 * Copyright 2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef OLD_UNPACKING_NODE_ATTRIBUTES_H
#define OLD_UNPACKING_NODE_ATTRIBUTES_H


#include "NodeListener.h"


class PackageNode;


class OldUnpackingNodeAttributes : public OldNodeAttributes {
public:
								OldUnpackingNodeAttributes(
									PackageNode* packageNode);
								OldUnpackingNodeAttributes(
									PackageNode* packageNode,
									const timespec& modifiedTime);

	virtual	timespec			ModifiedTime() const;
	virtual	off_t				FileSize() const;
	virtual	void*				IndexCookieForAttribute(const StringKey& name)
									const;

private:
			PackageNode*		fPackageNode;
			timespec			fModifiedTime;
			bool				fHasModifiedTime;
};


#endif	// OLD_UNPACKING_NODE_ATTRIBUTES_H
