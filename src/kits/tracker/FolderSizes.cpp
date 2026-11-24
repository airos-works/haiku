/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "FolderSizes.h"

#include <Autolock.h>
#include <Directory.h>
#include <Locker.h>
#include <Message.h>
#include <OS.h>

#include <fs_info.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <vector>


namespace BPrivate {


struct NodeKey {
	dev_t	device;
	ino_t	node;

	NodeKey(const node_ref& ref) : device(ref.device), node(ref.node) {}

	bool operator<(const NodeKey& other) const
	{
		if (device != other.device)
			return device < other.device;
		return node < other.node;
	}
};


struct CachedSize {
	off_t		size;
	bigtime_t	when;
};


struct Request {
	entry_ref	ref;
	node_ref	node;
	BMessenger	target;
	bigtime_t	maxAge;
	bigtime_t	notBefore;
	bool		refresh;
		// the target has the size already: only a different one is sent
	off_t		known;
		// the size cached when a refresh was asked for, -1 if none
};


static const int32 kMaxEntriesPerFolder = 2000000;
static const size_t kMaxCachedFolders = 20000;
static const int32 kEntriesBetweenChecks = 1000;

static BLocker sLock("folder sizes");
static std::map<NodeKey, CachedSize>* sCache;
static std::deque<Request>* sQueue;
static std::multiset<NodeKey>* sQueuedNodes;
	// the folders in sQueue, to find a request that is there quickly
static sem_id sQueueSem = -1;
static thread_id sThread = -1;
static BMessenger sWalkTarget;
	// whom the walk that is running is for
static volatile bool sCancelWalk;


static void
SendSize(const Request& request, off_t size)
{
	BMessage message(kFolderSizeReady);
	message.AddInt32("device", request.node.device);
	message.AddInt64("node", request.node.node);
	message.AddInt64("size", size);
	request.target.SendMessage(&message, (BHandler*)NULL, 0);
}


static off_t
WalkFolder(const entry_ref& ref, dev_t device, const BMessenger& target)
{
	// iterative, so deep trees cannot exhaust the stack; links are not
	// followed and other volumes mounted below are skipped. The folders
	// still to be read are kept as entry_refs, not as open BDirectorys: a
	// folder with hundreds of subfolders would hold a file descriptor for
	// each of them (Tracker has 512 for everything) and leave out the ones
	// that could not be opened. Returns -1 when the requester went away.
	off_t total = 0;
	int32 entries = 0;

	std::vector<entry_ref> stack;
	stack.push_back(ref);

	while (!stack.empty()) {
		BDirectory dir(&stack.back());
		stack.pop_back();
		if (dir.InitCheck() != B_OK)
			continue;

		BEntry entry;
		while (dir.GetNextEntry(&entry, false) == B_OK) {
			if (++entries > kMaxEntriesPerFolder)
				break;

			if (sCancelWalk) {
				// folder sizes were turned off in the window
				return -1;
			}
			if ((entries % kEntriesBetweenChecks) == 0 && !target.IsValid()) {
				// the window was closed: nobody waits for the size
				return -1;
			}

			struct stat st;
			if (entry.GetStat(&st) != B_OK || st.st_dev != device)
				continue;

			if (S_ISREG(st.st_mode))
				total += st.st_size;
			else if (S_ISDIR(st.st_mode)) {
				entry_ref subRef;
				if (entry.GetRef(&subRef) == B_OK)
					stack.push_back(subRef);
			}
		}

		if (entries > kMaxEntriesPerFolder)
			break;
	}

	return total;
}


static status_t
CalculatorThread(void*)
{
	while (true) {
		Request request;
		bigtime_t wait = 60000000;
		bool found = false;
		{
			BAutolock locker(sLock);
			// the first request that is due; one that waits for a folder's
			// changes to settle lets the others go first
			bigtime_t now = system_time();
			for (std::deque<Request>::iterator it = sQueue->begin();
					it != sQueue->end(); it++) {
				if (it->notBefore <= now) {
					request = *it;
					sQueue->erase(it);
					sQueuedNodes->erase(
						sQueuedNodes->find(NodeKey(request.node)));
					found = true;
					break;
				}
				wait = std::min(wait, it->notBefore - now);
			}
			// set with the request taken, so that a cancel cannot fall
			// between the two
			sWalkTarget = found ? request.target : BMessenger();
			sCancelWalk = false;
		}

		if (!found) {
			if (acquire_sem_etc(sQueueSem, 1, B_RELATIVE_TIMEOUT, wait)
					!= B_OK) {
				// idle for a minute: let the thread go, a request starts a
				// new one
				BAutolock locker(sLock);
				if (sQueue->empty()) {
					sThread = -1;
					return B_OK;
				}
			}
			continue;
		}

		{
			BAutolock locker(sLock);
			// asked for again meanwhile, or by another window
			std::map<NodeKey, CachedSize>::iterator cached
				= sCache->find(NodeKey(request.node));
			if (cached != sCache->end() && (request.maxAge < 0
					|| system_time() - cached->second.when
						<= request.maxAge)) {
				off_t size = cached->second.size;
				locker.Unlock();
				if (!request.refresh || size != request.known)
					SendSize(request, size);
				continue;
			}
		}

		if (!request.target.IsValid())
			continue;

		off_t size = WalkFolder(request.ref, request.node.device,
			request.target);
		if (size < 0)
			continue;

		{
			BAutolock locker(sLock);
			if (sCache->size() >= kMaxCachedFolders)
				sCache->clear();
			CachedSize cached = { size, system_time() };
			(*sCache)[NodeKey(request.node)] = cached;
		}

		if (!request.refresh || size != request.known)
			SendSize(request, size);
	}

	return B_OK;
}


static void
InitOnce()
{
	if (sCache != NULL)
		return;

	sCache = new std::map<NodeKey, CachedSize>;
	sQueue = new std::deque<Request>;
	sQueuedNodes = new std::multiset<NodeKey>;
	sQueueSem = create_sem(0, "folder size requests");
}


bool
GetFolderSize(const node_ref& node, off_t* size)
{
	BAutolock locker(sLock);
	InitOnce();

	std::map<NodeKey, CachedSize>::iterator found = sCache->find(NodeKey(node));
	if (found == sCache->end())
		return false;

	*size = found->second.size;
	return true;
}


static void
AskForFolderSize(const entry_ref& ref, const node_ref& node,
	const BMessenger& target, bigtime_t maxAge, bigtime_t delay,
	bool sendCached)
{
	BAutolock locker(sLock);
	InitOnce();

	Request request;
	request.ref = ref;
	request.node = node;
	request.target = target;
	request.maxAge = maxAge;
	request.refresh = !sendCached;
	request.known = -1;

	std::map<NodeKey, CachedSize>::iterator found = sCache->find(NodeKey(node));
	if (found != sCache->end()) {
		if (maxAge < 0 || system_time() - found->second.when <= maxAge) {
			off_t size = found->second.size;
			locker.Unlock();
			if (sendCached)
				SendSize(request, size);
			return;
		}
		request.known = found->second.size;
	}

	// One pending request per folder and requester: a folder that changes
	// all the time (files being copied into it) is not walked again for each
	// change that was reported while it waited. The request that is there
	// keeps its time, so the size still comes while the changes go on.
	bool merged = false;
	for (std::deque<Request>::iterator it = sQueue->begin();
			sQueuedNodes->count(NodeKey(node)) > 0 && it != sQueue->end();
			it++) {
		if (it->node != node || it->target != target)
			continue;
		if (maxAge >= 0 && (it->maxAge < 0 || maxAge < it->maxAge))
			it->maxAge = maxAge;
		if (!request.refresh)
			it->refresh = false;
		merged = true;
		break;
	}
	if (merged)
		return;

	request.notBefore = system_time() + delay;
	sQueue->push_back(request);
	sQueuedNodes->insert(NodeKey(node));

	if (sThread < 0) {
		sThread = spawn_thread(CalculatorThread, "folder sizes",
			B_LOW_PRIORITY, NULL);
		if (sThread >= 0)
			resume_thread(sThread);
	}
	release_sem(sQueueSem);
}


void
RequestFolderSize(const entry_ref& ref, const node_ref& node,
	const BMessenger& target, bigtime_t maxAge, bigtime_t delay)
{
	AskForFolderSize(ref, node, target, maxAge, delay, true);
}


void
RefreshFolderSize(const entry_ref& ref, const node_ref& node,
	const BMessenger& target, bigtime_t maxAge)
{
	AskForFolderSize(ref, node, target, maxAge, 0, false);
}


void
CancelFolderSizes(const BMessenger& target)
{
	BAutolock locker(sLock);
	InitOnce();

	for (std::deque<Request>::iterator it = sQueue->begin();
			it != sQueue->end();) {
		if (it->target != target) {
			it++;
			continue;
		}
		sQueuedNodes->erase(sQueuedNodes->find(NodeKey(it->node)));
		it = sQueue->erase(it);
	}

	if (sWalkTarget == target)
		sCancelWalk = true;
}


void
ForgetFolderSize(const node_ref& node)
{
	BAutolock locker(sLock);
	InitOnce();
	sCache->erase(NodeKey(node));
}


bool
CanCalculateFolderSizes(dev_t device)
{
	fs_info info;
	if (fs_stat_dev(device, &info) != 0)
		return false;

	static const char* kNetworkFileSystems[] = {
		"smbfs", "nfs", "nfs4", "netfs", "userlandfs", "webdavfs", NULL
	};
	for (int32 i = 0; kNetworkFileSystems[i] != NULL; i++) {
		if (strcmp(info.fsh_name, kNetworkFileSystems[i]) == 0)
			return false;
	}

	return true;
}


} // namespace BPrivate
