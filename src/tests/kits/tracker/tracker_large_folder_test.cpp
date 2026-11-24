/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


// Tracker with large folders (#179), through libtracker.so: folder sizes and
// thumbnails must not hold a file descriptor per pending item (Tracker has
// 512, the test sets the same limit), and a folder that is reported changed
// over and over is not walked again for every report.
//
//	tracker_large_folder_test <folder> [-only <name>]
//
// The folder is emptied and filled with some thousands of entries. Every
// wait has a deadline. Exits 0 when everything is as it must be.


#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <DataIO.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <NodeInfo.h>
#include <OS.h>
#include <Path.h>
#include <String.h>
#include <TranslationDefs.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "FolderSizes.h"
#include "Model.h"
#include "Thumbnails.h"


static const int kFileLimit = 512;
	// as Tracker sets it (Tracker.cpp)

static BString sFolder;
static const char* sOnly = NULL;
static int sFailures = 0;
static int sChecks = 0;


static void
Fail(const char* what)
{
	sFailures++;
	printf("    FAIL: %s\n", what);
}


static void
Check(bool ok, const char* what)
{
	sChecks++;
	if (ok)
		printf("    ok: %s\n", what);
	else
		Fail(what);
}


static int
OpenFileDescriptors()
{
	int count = 0;
	for (int fd = 0; fd < kFileLimit; fd++) {
		if (fcntl(fd, F_GETFD) != -1)
			count++;
	}
	return count;
}


static void
RemoveTree(const char* path)
{
	BEntry entry(path);
	if (!entry.Exists())
		return;
	if (entry.IsDirectory()) {
		BDirectory dir(path);
		BEntry child;
		while (dir.GetNextEntry(&child) == B_OK) {
			BPath childPath(&child);
			RemoveTree(childPath.Path());
			dir.Rewind();
		}
	}
	entry.Remove();
}


static void
MakeFile(const BString& path, size_t size, const char* type = NULL)
{
	BFile file(path.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (size > 0) {
		char* buffer = (char*)malloc(size);
		memset(buffer, 'x', size);
		file.Write(buffer, size);
		free(buffer);
	}
	if (type != NULL)
		BNodeInfo(&file).SetType(type);
}


// Receives kFolderSizeReady and keeps count.
class SizeReceiver : public BLooper {
public:
	SizeReceiver()
		:
		BLooper("size receiver"),
		fReplies(0),
		fLastSize(-1),
		fLastReply(0)
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != kFolderSizeReady) {
			BLooper::MessageReceived(message);
			return;
		}
		BAutolock locker(fLock);
		fReplies++;
		fLastSize = message->GetInt64("size", -1);
		fLastReply = system_time();
	}

	void Get(int32* replies, off_t* size, bigtime_t* last)
	{
		BAutolock locker(fLock);
		*replies = fReplies;
		*size = fLastSize;
		*last = fLastReply;
	}

	void Reset()
	{
		BAutolock locker(fLock);
		fReplies = 0;
		fLastSize = -1;
		fLastReply = 0;
	}

private:
	BLocker		fLock;
	int32		fReplies;
	off_t		fLastSize;
	bigtime_t	fLastReply;
};


static SizeReceiver* sReceiver;
static volatile bool sSampling;
static volatile int sPeakDescriptors;


static status_t
SampleDescriptors(void*)
{
	while (sSampling) {
		int count = OpenFileDescriptors();
		if (count > sPeakDescriptors)
			sPeakDescriptors = count;
		snooze(500);
	}
	return B_OK;
}


static void
FolderSizeManySubfolders()
{
	// a folder with 1,000 subfolders that each hold a file of 1,000 bytes:
	// the size must be all of it, and the walk must not take a descriptor
	// for each subfolder that waits to be read
	BString root(sFolder);
	root << "/many";
	RemoveTree(root.String());
	create_directory(root.String(), 0755);
	for (int32 i = 0; i < 1000; i++) {
		BString sub(root);
		sub << "/folder" << i;
		create_directory(sub.String(), 0755);
		sub << "/file";
		MakeFile(sub, 1000);
	}

	BEntry entry(root.String());
	entry_ref ref;
	node_ref node;
	entry.GetRef(&ref);
	entry.GetNodeRef(&node);

	int before = OpenFileDescriptors();
	sPeakDescriptors = before;
	sSampling = true;
	thread_id sampler = spawn_thread(SampleDescriptors, "sampler",
		B_NORMAL_PRIORITY, NULL);
	resume_thread(sampler);

	sReceiver->Reset();
	ForgetFolderSize(node);
	RequestFolderSize(ref, node, BMessenger(sReceiver), 0);

	bigtime_t end = system_time() + 120000000LL;
	int32 replies = 0;
	off_t size = -1;
	bigtime_t last;
	while (system_time() < end) {
		sReceiver->Get(&replies, &size, &last);
		if (replies > 0)
			break;
		snooze(10000);
	}
	sSampling = false;
	status_t result;
	wait_for_thread(sampler, &result);

	printf("    descriptors open: %d before, at most %d during the walk\n",
		before, sPeakDescriptors);
	if (replies == 0) {
		Fail("no size within 120 s");
		return;
	}

	BString what;
	what << "the size is all of it: " << size << " bytes of 1000000";
	Check(size == 1000000, what.String());
	Check(sPeakDescriptors - before < 50,
		"the walk held few descriptors (fewer than 50)");

	RemoveTree(root.String());
}


static void
FolderSizeBurst()
{
	// A folder that files are copied into is reported changed for every
	// file; each report forgets its size and asks for it again, as
	// BPoseView::AttributeChanged() does. 1,000 reports in a row must not
	// mean 1,000 walks of 5,000 entries.
	BString root(sFolder);
	root << "/busy";
	RemoveTree(root.String());
	create_directory(root.String(), 0755);
	for (int32 i = 0; i < 5000; i++) {
		BString file(root);
		file << "/file" << i;
		MakeFile(file, 100);
	}

	BEntry entry(root.String());
	entry_ref ref;
	node_ref node;
	entry.GetRef(&ref);
	entry.GetNodeRef(&node);

	sReceiver->Reset();
	bigtime_t start = system_time();
	for (int32 i = 0; i < 1000; i++) {
		ForgetFolderSize(node);
		RequestFolderSize(ref, node, BMessenger(sReceiver), 0);
	}

	// done when no reply came for 3 s
	bigtime_t end = start + 300000000LL;
	int32 replies = 0;
	off_t size = -1;
	bigtime_t last = 0;
	while (system_time() < end) {
		sReceiver->Get(&replies, &size, &last);
		if (replies > 0 && system_time() - last > 3000000)
			break;
		snooze(50000);
	}

	printf("    1000 reports: %" B_PRId32 " sizes sent (walks), the last "
		"%.1f s after the first report\n", replies,
		last > 0 ? (last - start) / 1e6 : 0.0);
	BString what;
	what << "the size is right: " << size << " bytes of 500000";
	Check(size == 500000, what.String());
	Check(replies > 0 && replies <= 3, "at most 3 walks");

	RemoveTree(root.String());
}


static void
ThumbnailJobs()
{
	// 1,000 pictures in an icon view: each asks for a thumbnail, and the
	// jobs wait for the worker. They must not hold the files open while
	// they wait. The pictures are 2,000 by 2,000 pixels of one colour: a
	// few KiB on disk, and some tens of milliseconds to read each.
	BBitmap picture(BRect(0, 0, 1999, 1999), B_RGB32);
	memset(picture.Bits(), 0x80, picture.BitsLength());
	BBitmapStream pictureStream(&picture);
	BMallocIO png;
	BBitmap* detached;
	if (BTranslatorRoster::Default()->Translate(&pictureStream, NULL, NULL,
			&png, B_PNG_FORMAT) != B_OK) {
		Fail("no PNG translator");
		pictureStream.DetachBitmap(&detached);
		return;
	}
	pictureStream.DetachBitmap(&detached);

	BString root(sFolder);
	root << "/pictures";
	RemoveTree(root.String());
	create_directory(root.String(), 0755);
	for (int32 i = 0; i < 1000; i++) {
		BString path(root);
		path << "/picture" << i << ".png";
		BFile file(path.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		file.Write(png.Buffer(), png.BufferLength());
		BNodeInfo(&file).SetType("image/png");
	}

	int before = OpenFileDescriptors();
	BBitmap icon(BRect(0, 0, 63, 63), B_RGBA32);
	int32 asked = 0;
	BDirectory dir(root.String());
	entry_ref ref;
	while (dir.GetNextRef(&ref) == B_OK) {
		Model model(&ref, false, true);
		if (model.InitCheck() != B_OK)
			continue;
		if (GetThumbnailFromAttr(&model, &icon, BSize(63, 63)) == B_BUSY)
			asked++;
	}
	int after = OpenFileDescriptors();

	// is there a descriptor left for anything else?
	BString probe(sFolder);
	probe << "/probe";
	int fd = open(probe.String(), O_CREAT | O_WRONLY, 0644);
	bool canOpen = fd >= 0;
	if (fd >= 0)
		close(fd);
	unlink(probe.String());

	printf("    %" B_PRId32 " thumbnails asked for; descriptors open: %d "
		"before, %d right after\n", asked, before, after);
	Check(asked >= 990, "the thumbnails were queued");
	Check(after - before < 50, "the queued jobs hold few descriptors "
		"(fewer than 50)");
	Check(canOpen, "a file can still be opened");

	// the jobs that are left fail when their files are gone

	RemoveTree(root.String());
}


struct Scenario {
	const char*	name;
	void		(*function)();
};


static const Scenario kScenarios[] = {
	{ "folder-size-many-subfolders", FolderSizeManySubfolders },
	{ "folder-size-burst", FolderSizeBurst },
	{ "thumbnail-jobs", ThumbnailJobs },
	{ NULL, NULL }
};


static void
CrashHandler(int signal)
{
	printf("crashed with signal %d\n", signal);
	fflush(stdout);
	_exit(3);
}


int
main(int argc, char** argv)
{
	signal(SIGSEGV, CrashHandler);
	signal(SIGBUS, CrashHandler);
	signal(SIGILL, CrashHandler);
	signal(SIGFPE, CrashHandler);
	setvbuf(stdout, NULL, _IOLBF, 0);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-only") == 0 && i + 1 < argc)
			sOnly = argv[++i];
		else
			sFolder = argv[i];
	}
	if (sFolder.Length() == 0) {
		fprintf(stderr, "usage: %s <folder> [-only <name>]\n", argv[0]);
		return 2;
	}
	create_directory(sFolder.String(), 0755);

	struct rlimit limit;
	limit.rlim_cur = kFileLimit;
	limit.rlim_max = RLIM_SAVED_MAX;
	if (setrlimit(RLIMIT_NOFILE, &limit) != 0)
		printf("could not set the file descriptor limit to %d\n", kFileLimit);

	BApplication app("application/x-vnd.airos-tracker-large-folder-test");
	IconCache::sIconCache = new IconCache();
		// as Tracker makes it: Models and thumbnails use it
	sReceiver = new SizeReceiver;
	sReceiver->Run();

	for (int32 i = 0; kScenarios[i].name != NULL; i++) {
		if (sOnly != NULL && strcmp(sOnly, kScenarios[i].name) != 0)
			continue;
		printf("%s\n", kScenarios[i].name);
		int failures = sFailures;
		kScenarios[i].function();
		printf("  %s\n", sFailures == failures ? "PASSED" : "FAILED");
	}

	printf("%d checks, %d failed\n", sChecks, sFailures);
	fflush(stdout);
	// the folder size and thumbnail threads of libtracker keep running
	_exit(sFailures == 0 ? 0 : 1);
}
