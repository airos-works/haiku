/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


// Tracker with large folders (#179), through libtracker.so: folder sizes and
// thumbnails must not hold a file descriptor per pending item (Tracker has
// 512, the test sets the same limit), and a folder that is reported changed
// over and over is not walked again for every report. A walk stops when its
// window turns folder sizes off; thumbnails of icons that were scrolled out
// of view are not made; a file whose thumbnail failed is not opened and
// tried again at every redraw.
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
#include <View.h>
#include <Window.h>

#include <dlfcn.h>
#include <fs_attr.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "Attributes.h"
#include "FolderSizes.h"
#include "IconCache.h"
#include "Model.h"
#include "Thumbnails.h"

#include <vector>


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


static bigtime_t
ThreadTime(const char* name)
{
	// CPU time of this team's threads of that name
	bigtime_t total = 0;
	int32 cookie = 0;
	thread_info info;
	while (get_next_thread_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		if (strcmp(info.name, name) == 0)
			total += info.user_time + info.kernel_time;
	}
	return total;
}


static bigtime_t
WaitForIdleThread(const char* name, bigtime_t quiet, bigtime_t deadline)
{
	// until the threads of that name used no CPU for quiet; returns when
	// they last did
	bigtime_t last = ThreadTime(name);
	bigtime_t lastChange = system_time();
	while (system_time() < deadline) {
		snooze(100000);
		bigtime_t now = ThreadTime(name);
		if (now != last) {
			last = now;
			lastChange = system_time();
		} else if (system_time() - lastChange >= quiet)
			break;
	}
	return lastChange;
}


static void
FolderSizeCancel()
{
	// A walk of 300,000 files is running for a window when that window
	// turns folder sizes off (BPoseView::SetShowFolderSizes(false) calls
	// CancelFolderSizes()): the walk must stop, and send nothing. A
	// libtracker.so without CancelFolderSizes() cannot stop it.
	typedef void (*CancelFunction)(const BMessenger&);
	CancelFunction cancel = (CancelFunction)dlsym(RTLD_DEFAULT,
		"_ZN8BPrivate17CancelFolderSizesERK10BMessenger");
	printf("    CancelFolderSizes() %s\n", cancel != NULL
		? "is there" : "is not in this libtracker.so: nothing to call");

	BString root(sFolder);
	root << "/bigtree";
	BEntry rootEntry(root.String());
	if (!rootEntry.Exists()) {
		// kept between runs: making it takes a while
		BString making(root);
		making << ".making";
		RemoveTree(making.String());
		create_directory(making.String(), 0755);
		for (int32 i = 0; i < 300; i++) {
			BString sub(making);
			sub << "/folder" << i;
			create_directory(sub.String(), 0755);
			for (int32 j = 0; j < 1000; j++) {
				BString file(sub);
				file << "/f" << j;
				MakeFile(file, 10);
			}
		}
		BEntry(making.String()).Rename(root.String());
	}

	BEntry entry(root.String());
	entry_ref ref;
	node_ref node;
	entry.GetRef(&ref);
	entry.GetNodeRef(&node);

	// once to have the tree in the cache, timed
	sReceiver->Reset();
	ForgetFolderSize(node);
	bigtime_t start = system_time();
	RequestFolderSize(ref, node, BMessenger(sReceiver), 0);
	int32 replies = 0;
	off_t size = -1;
	bigtime_t last = 0;
	while (system_time() < start + 300000000LL) {
		sReceiver->Get(&replies, &size, &last);
		if (replies > 0)
			break;
		snooze(10000);
	}
	bigtime_t walkTime = last - start;
	printf("    a whole walk: %.2f s, %" B_PRIdOFF " bytes\n",
		walkTime / 1e6, size);

	// again, and cancelled 200 ms into it
	sReceiver->Reset();
	ForgetFolderSize(node);
	bigtime_t cpuBefore = ThreadTime("folder sizes");
	RequestFolderSize(ref, node, BMessenger(sReceiver), 0);
	snooze(200000);
	bigtime_t cancelled = system_time();
	if (cancel != NULL)
		cancel(BMessenger(sReceiver));
	bigtime_t idle = WaitForIdleThread("folder sizes", 1000000,
		cancelled + 300000000LL);
	snooze(500000);
	sReceiver->Get(&replies, &size, &last);
	bigtime_t cpu = ThreadTime("folder sizes") - cpuBefore;
	printf("    cancelled 0.2 s into the walk: the thread was busy %.2f s "
		"after, %.2f s CPU in all, %" B_PRId32 " sizes sent\n",
		(idle - cancelled) / 1e6, cpu / 1e6, replies);
	Check(replies == 0, "no size was sent after the cancel");
	Check(idle - cancelled < 300000 || idle - cancelled < walkTime / 4,
		"the walk stopped at once");
}


// A window with a view of 400 by 400 pixels, the size of a folder window's
// pose view, to draw icons into as Tracker does.
class IconWindow : public BWindow {
public:
	IconWindow()
		:
		BWindow(BRect(50, 50, 449, 449), "icons", B_TITLED_WINDOW,
			B_NOT_RESIZABLE)
	{
		fView = new BView(Bounds(), "poses", B_FOLLOW_ALL, B_WILL_DRAW);
		AddChild(fView);
	}

	BView* View() { return fView; }

private:
	BView*	fView;
};


static const int32 kIconCell = 80;
static const int32 kIconColumns = 5;


static BPoint
IconPlace(int32 index)
{
	return BPoint((index % kIconColumns) * kIconCell,
		(index / kIconColumns) * kIconCell);
}


static int32
CountThumbnails(const std::vector<entry_ref>& refs, int32 first, int32 count)
{
	int32 made = 0;
	for (int32 i = first; i < first + count && i < (int32)refs.size(); i++) {
		BNode node(&refs[i]);
		attr_info info;
		if (node.GetAttrInfo(kAttrThumbnail, &info) == B_OK)
			made++;
	}
	return made;
}


static void
ThumbnailsOutOfView()
{
	// 600 pictures in an icon view, paged through in a second from the top
	// to the bottom: each page asks for 25 thumbnails, and only the last
	// page is still in view when the worker gets to them. Each picture is
	// read whole (2,000 by 2,000 pixels): the ones that were scrolled away
	// should not be.
	BBitmap picture(BRect(0, 0, 1999, 1999), B_RGB32);
	memset(picture.Bits(), 0x80, picture.BitsLength());
	BBitmapStream pictureStream(&picture);
	BMallocIO png;
	BBitmap* detached;
	status_t status = BTranslatorRoster::Default()->Translate(&pictureStream,
		NULL, NULL, &png, B_PNG_FORMAT);
	pictureStream.DetachBitmap(&detached);
	if (status != B_OK) {
		Fail("no PNG translator");
		return;
	}

	BString root(sFolder);
	root << "/paged";
	RemoveTree(root.String());
	create_directory(root.String(), 0755);
	const int32 kCount = 600;
	std::vector<entry_ref> refs;
	std::vector<Model*> models;
	for (int32 i = 0; i < kCount; i++) {
		BString path(root);
		path << "/picture" << i << ".png";
		BFile file(path.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		file.Write(png.Buffer(), png.BufferLength());
		BNodeInfo(&file).SetType("image/png");
		file.Unset();
		entry_ref ref;
		get_ref_for_path(path.String(), &ref);
		refs.push_back(ref);
		models.push_back(new Model(&ref, false, false));
	}

	IconWindow* window = new IconWindow;
	window->Show();
	BView* view = window->View();

	bigtime_t start = system_time();
	const int32 kPerPage = 400 / kIconCell * kIconColumns;
	int32 pages = kCount / kPerPage;
	for (int32 page = 0; page < pages; page++) {
		window->Lock();
		view->ScrollTo(0, page * 400);
		for (int32 i = page * kPerPage; i < (page + 1) * kPerPage; i++) {
			IconCache::sIconCache->Draw(models[i], view, IconPlace(i),
				kNormalIcon, BSize(63, 63));
		}
		view->Sync();
		window->Unlock();
		snooze(1000000 / pages);
	}
	bigtime_t paged = system_time();

	// done when no thumbnail was made for 3 s
	int32 made = 0;
	bigtime_t lastMade = paged;
	while (system_time() < paged + 300000000LL) {
		int32 now = CountThumbnails(refs, 0, kCount);
		if (now != made) {
			made = now;
			lastMade = system_time();
		} else if (system_time() - lastMade > 3000000)
			break;
		snooze(200000);
	}
	int32 lastPage = CountThumbnails(refs, (pages - 1) * kPerPage, kPerPage);
	typedef bool (*FailedFunction)(const Model*);
	FailedFunction failed = (FailedFunction)dlsym(RTLD_DEFAULT,
		"_ZN8BPrivate15ThumbnailFailedEPKNS_5ModelE");
	for (int32 i = (pages - 1) * kPerPage; i < pages * kPerPage; i++) {
		BNode node(&refs[i]);
		attr_info info;
		if (node.GetAttrInfo(kAttrThumbnail, &info) == B_OK)
			continue;
		printf("    no thumbnail: %s (node %" B_PRIdINO "), failed before: "
			"%s\n", refs[i].name, models[i]->NodeRef()->node,
			failed == NULL ? "?" : failed(models[i]) ? "yes" : "no");
	}

	printf("    %" B_PRId32 " pages of %" B_PRId32 " icons in %.1f s; "
		"thumbnails made: %" B_PRId32 " of %" B_PRId32 ", %" B_PRId32 " of "
		"the %" B_PRId32 " in view; the last %.1f s after the paging\n",
		pages, kPerPage, (paged - start) / 1e6, made, kCount, lastPage,
		kPerPage, (lastMade - paged) / 1e6);
	Check(lastPage == kPerPage, "the icons in view got their thumbnails");
	Check(made < kPerPage * 2, "few thumbnails of icons out of view "
		"(fewer than 50)");

	window->Lock();
	window->Quit();
	for (size_t i = 0; i < models.size(); i++)
		delete models[i];
	RemoveTree(root.String());
}


static void
FailedThumbnailRedraws()
{
	// 500 files that say they are PNG pictures but are not: their
	// thumbnails fail. Once that is known, drawing them again must not
	// open them and try again (Tracker redraws a window for many reasons).
	BString root(sFolder);
	root << "/notpictures";
	RemoveTree(root.String());
	create_directory(root.String(), 0755);
	const int32 kCount = 500;
	std::vector<Model*> models;
	for (int32 i = 0; i < kCount; i++) {
		BString path(root);
		path << "/broken" << i << ".png";
		MakeFile(path, 4096, "image/png");
		entry_ref ref;
		get_ref_for_path(path.String(), &ref);
		models.push_back(new Model(&ref, false, false));
	}

	IconWindow* window = new IconWindow;
	window->Show();
	BView* view = window->View();

	// the first time: their jobs are queued and fail
	window->Lock();
	for (int32 i = 0; i < kCount; i++) {
		IconCache::sIconCache->Draw(models[i], view, BPoint(0, 0),
			kNormalIcon, BSize(63, 63));
	}
	view->Sync();
	window->Unlock();
	WaitForIdleThread("thumbnail worker", 2000000,
		system_time() + 120000000LL);

	// then ten redraws
	bigtime_t workerBefore = ThreadTime("thumbnail worker");
	bigtime_t drawTime = 0;
	for (int32 pass = 0; pass < 10; pass++) {
		window->Lock();
		bigtime_t start = system_time();
		for (int32 i = 0; i < kCount; i++) {
			IconCache::sIconCache->Draw(models[i], view, BPoint(0, 0),
				kNormalIcon, BSize(63, 63));
		}
		view->Sync();
		drawTime += system_time() - start;
		window->Unlock();
	}
	WaitForIdleThread("thumbnail worker", 2000000,
		system_time() + 120000000LL);
	bigtime_t worker = ThreadTime("thumbnail worker") - workerBefore;

	printf("    10 redraws of %" B_PRId32 " icons whose thumbnail failed: "
		"%.2f ms each; the thumbnail workers used %.1f ms meanwhile\n",
		kCount, drawTime / 10 / 1e3, worker / 1e3);
	Check(worker < 50000, "no thumbnail was tried again (workers used "
		"less than 50 ms)");

	window->Lock();
	window->Quit();
	for (size_t i = 0; i < models.size(); i++)
		delete models[i];
	RemoveTree(root.String());
}


struct Scenario {
	const char*	name;
	void		(*function)();
};


static const Scenario kScenarios[] = {
	{ "folder-size-many-subfolders", FolderSizeManySubfolders },
	{ "folder-size-burst", FolderSizeBurst },
	{ "folder-size-cancel", FolderSizeCancel },
	// before thumbnail-jobs: after it, 1 to 3 of the 600 pictures got no
	// thumbnail at all, with the libtracker.so of before and after these
	// changes alike (not explained yet); on their own, every one does
	{ "thumbnails-out-of-view", ThumbnailsOutOfView },
	{ "failed-thumbnail-redraws", FailedThumbnailRedraws },
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
