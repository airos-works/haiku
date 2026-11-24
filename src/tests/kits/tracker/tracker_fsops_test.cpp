/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


// Tracker's copy and move engine, driven through libtracker.so the way the
// windows drive it (FSMoveToFolder(), FSMoveToTrash(), FSUndo(), FSRedo()),
// between two volumes and within one. After each operation the test compares
// what is where, with which contents, to what must be there: files that were
// skipped, files that were there before, files a folder of the same name
// would have replaced.
//
//	tracker_fsops_test <folder on one volume> <folder on another volume>
//		[-fat <folder on a FAT volume>] [-only <name>]
//
// The folders are emptied. The FAT volume is for copies that fail (it takes
// no links and not every name).
//
// The window for name clashes and the alerts are answered by the test. Every
// wait has a deadline. Exits 0 when everything is as it must be.


#include <Alert.h>
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <ListView.h>
#include <ObjectList.h>
#include <Path.h>
#include <String.h>
#include <TextView.h>
#include <Volume.h>
#include <Window.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Commands.h"
#include "FSUndoRedo.h"
#include "FSUtils.h"


enum Answer {
	kAnswerDefault,		// Continue with what the window proposes
	kAnswerSkip,		// "Skip" for all, then Continue
	kAnswerReplace,
	kAnswerKeepBoth,
	kAnswerCancel
};

static const bigtime_t kDeadline = 60 * 1000000LL;

static BString sA, sB, sFat;
static const char* sOnly = NULL;
static int sFailures = 0;
static int sChecks = 0;
static int32 sConflictWindows = 0;
static int32 sConflictItems = 0;
static std::vector<std::string> sAlerts;


static void
Fail(const char* what)
{
	printf("    FAIL: %s\n", what);
	sFailures++;
}


//	#pragma mark - trees


static BString
P(const BString& base, const char* relative)
{
	BString path(base);
	path << '/' << relative;
	return path;
}


static void
RemoveTree(const char* path)
{
	BEntry entry(path);
	if (!entry.Exists())
		return;
	if (entry.IsDirectory()) {
		BDirectory dir(&entry);
		BEntry child;
		while (dir.GetNextEntry(&child) == B_OK) {
			BPath childPath(&child);
			RemoveTree(childPath.Path());
		}
	}
	entry.Remove();
}


static void
MakeDir(const BString& path)
{
	create_directory(path.String(), 0755);
}


// A file with the given contents, changed "age" seconds ago
static void
MakeFile(const BString& path, const char* contents, int age = 100)
{
	BPath parent;
	BPath(path.String()).GetParent(&parent);
	create_directory(parent.Path(), 0755);

	BFile file(path.String(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	file.Write(contents, strlen(contents));
	file.Unset();

	struct utimbuf times;
	times.actime = times.modtime = time(NULL) - age;
	utime(path.String(), &times);
}


static void
ListTree(const BString& root, const BString& relative,
	std::vector<std::string>& lines)
{
	BString path(root);
	if (relative.Length() > 0)
		path << '/' << relative;

	BDirectory dir(path.String());
	BEntry entry;
	while (dir.GetNextEntry(&entry) == B_OK) {
		BString name(relative);
		if (name.Length() > 0)
			name << '/';
		name << entry.Name();

		if (entry.IsDirectory()) {
			lines.push_back(std::string(name.String()) + "/");
			ListTree(root, name, lines);
		} else if (entry.IsSymLink()) {
			lines.push_back(std::string(name.String()) + "@");
		} else {
			char buffer[256];
			BPath filePath(&entry);
			BFile file(filePath.Path(), B_READ_ONLY);
			off_t size = 0;
			file.GetSize(&size);
			ssize_t bytes = file.Read(buffer, sizeof(buffer) - 1);
			if (bytes < 0)
				bytes = 0;
			buffer[bytes] = '\0';
			BString line(name);
			if (size > (off_t)sizeof(buffer) - 1)
				line << "=<" << size << " bytes>";
			else
				line << '=' << buffer;
			lines.push_back(line.String());
		}
	}
}


// What is below root, sorted, as "name=contents" and "folder/", in one line
static BString
Tree(const BString& root)
{
	std::vector<std::string> lines;
	ListTree(root, "", lines);
	std::sort(lines.begin(), lines.end());

	BString result;
	for (size_t i = 0; i < lines.size(); i++) {
		if (i > 0)
			result << " | ";
		result << lines[i].c_str();
	}
	return result;
}


static void
Expect(const char* what, const BString& root, const char* expected)
{
	sChecks++;
	BString tree = Tree(root);
	if (tree == expected) {
		printf("    ok: %s: %s\n", what, tree.String());
		return;
	}
	printf("    FAIL: %s\n      is:     %s\n      wanted: %s\n", what,
		tree.String(), expected);
	sFailures++;
}


//	#pragma mark - waiting for the engine, answering its windows


static bool
EngineThreadsRunning()
{
	static const char* const kNames[] = { "MoveTask", "DeleteTask", "DupTask",
		"RestoreTask", "undo call", NULL };

	team_info teamInfo;
	get_team_info(B_CURRENT_TEAM, &teamInfo);
	int32 cookie = 0;
	thread_info info;
	while (get_next_thread_info(teamInfo.team, &cookie, &info) == B_OK) {
		for (int32 i = 0; kNames[i] != NULL; i++) {
			if (strcmp(info.name, kNames[i]) == 0)
				return true;
		}
	}
	return false;
}


static void
WaitUntilGone(BWindow* window)
{
	for (int32 tries = 0; tries < 100; tries++) {
		bool found = false;
		for (int32 i = 0; i < be_app->CountWindows(); i++) {
			if (be_app->WindowAt(i) == window)
				found = true;
		}
		if (!found)
			return;
		snooze(20000);
	}
}


// Waits until the copy, move or delete threads have ended; meanwhile answers
// the window for name clashes as told, and alerts with the given button (or
// their last one, when there are fewer).
static bool
WaitForEngine(Answer answer = kAnswerDefault, int32 alertButton = 1)
{
	bigtime_t end = system_time() + kDeadline;
	int32 idle = 0;
	while (system_time() < end) {
		for (int32 i = 0; i < be_app->CountWindows(); i++) {
			BWindow* window = be_app->WindowAt(i);
			if (window == NULL)
				continue;
			if (window->LockWithTimeout(1000000) != B_OK)
				continue;
			BString title(window->Title());
			BAlert* alert = dynamic_cast<BAlert*>(window);
			BListView* list
				= dynamic_cast<BListView*>(window->FindView("conflicts"));
			bool hidden = window->IsHidden();
			if (list != NULL)
				sConflictItems += list->CountItems();
			BString text;
			int32 buttons = 0;
			if (alert != NULL) {
				text = alert->TextView()->Text();
				buttons = alert->CountButtons();
			}
			window->Unlock();
			if (hidden)
				continue;

			if (alert != NULL) {
				text.ReplaceAll("\n", " ");
				text.ReplaceAll("\t", " ");
				int32 which = std::min(alertButton, buttons - 1);
				printf("    alert: \"%s\" -> button %d\n", text.String(),
					(int)which);
				sAlerts.push_back(text.String());
				BMessage press('ALTB');
				press.AddInt32("which", which);
				window->PostMessage(&press);
				WaitUntilGone(window);
			} else if (list != NULL) {
				sConflictWindows++;
				printf("    window \"%s\": %d item(s)\n", title.String(),
					(int)list->CountItems());
				if (answer == kAnswerCancel)
					window->PostMessage(B_QUIT_REQUESTED);
				else {
					if (answer != kAnswerDefault) {
						// CopyConflictDecision, as the window's buttons send it
						BMessage decide('ccdc');
						decide.AddInt32("decision", answer == kAnswerReplace
							? 0 : answer == kAnswerSkip ? 1 : 2);
						window->PostMessage(&decide);
					}
					window->PostMessage('ccok');
				}
				WaitUntilGone(window);
			}
		}

		if (EngineThreadsRunning())
			idle = 0;
		else if (++idle >= 3)
			return true;
		snooze(30000);
	}

	Fail("the operation did not end in time");
	return false;
}


static BObjectList<entry_ref, true>*
RefList(const std::vector<BString>& paths)
{
	BObjectList<entry_ref, true>* list
		= new BObjectList<entry_ref, true>(10);
	for (size_t i = 0; i < paths.size(); i++) {
		entry_ref ref;
		get_ref_for_path(paths[i].String(), &ref);
		list->AddItem(new entry_ref(ref));
	}
	return list;
}


static void
Operate(uint32 mode, const std::vector<BString>& sources, const BString& dest,
	Answer answer = kAnswerDefault, int32 alertButton = 1)
{
	sConflictWindows = 0;
	sConflictItems = 0;
	sAlerts.clear();
	FSMoveToFolder(RefList(sources), new BEntry(dest.String()), mode, NULL);
	WaitForEngine(answer, alertButton);
}


static void
Operate(uint32 mode, const BString& source, const BString& dest,
	Answer answer = kAnswerDefault, int32 alertButton = 1)
{
	std::vector<BString> sources;
	sources.push_back(source);
	Operate(mode, sources, dest, answer, alertButton);
}


//	#pragma mark - undo


static int32 sSentinelCount = 0;
static BString sSentinel;


// Puts an undo item of our own on the list, so that "no undo item was added"
// can be told from "the item of an earlier scenario is on top".
static void
PushSentinel()
{
	BString name;
	name << "sentinel-" << ++sSentinelCount;
	BString path = P(sA, name.String());
	MakeFile(path, "sentinel");
	BEntry entry(path.String());
	{
		RenameUndo undo(entry, "never-renamed");
	}
	sSentinel = "";
	sSentinel << "rename of \"" << name << "\"";
	BEntry(path.String()).Remove();
}


// The name of the operation Undo would undo; empty when it is our sentinel,
// that is, when the operation since PushSentinel() left nothing to undo
static BString
UndoOffered()
{
	BString description;
	if (!FSGetUndoDescription(description) || description == sSentinel)
		return "";
	return description;
}


static status_t
UndoCall(void* redo)
{
	if (redo != NULL)
		FSRedo();
	else
		FSUndo();
	return B_OK;
}


// Undo (or Redo) in a thread of its own: it may ask, and the question is
// answered by WaitForEngine()
static void
UndoOrRedo(bool redo, Answer answer = kAnswerDefault, int32 alertButton = 0)
{
	sConflictWindows = 0;
	sAlerts.clear();
	thread_id thread = spawn_thread(UndoCall, "undo call", B_NORMAL_PRIORITY,
		redo ? (void*)1 : NULL);
	resume_thread(thread);
	WaitForEngine(answer, alertButton);
}


// Undo of the operation of this scenario, if it left an undo item
static void
UndoIfOffered(Answer answer = kAnswerDefault)
{
	BString item = UndoOffered();
	if (item.Length() == 0) {
		printf("    (no undo item was left)\n");
		return;
	}
	printf("    Undo %s\n", item.String());
	UndoOrRedo(false, answer);
}


//	#pragma mark - scenarios


static BString a, b;
	// the folders of the scenario that runs: on the first and second volume


static void
MoveAcrossVolumesSkipped()
{
	// #155 (1): files that are skipped inside a folder that moves to another
	// volume have to stay where they were
	MakeFile(P(a, "proj/a.txt"), "A-a");
	MakeFile(P(a, "proj/b.txt"), "A-b");
	MakeFile(P(a, "proj/sub/c.txt"), "A-c");
	MakeFile(P(a, "proj/sub/d.txt"), "A-d");
	MakeFile(P(b, "proj/a.txt"), "B-a", 10);
	MakeFile(P(b, "proj/sub/c.txt"), "B-c", 10);

	Operate(kMoveSelectionTo, P(a, "proj"), b);
	Expect("destination: its own files kept, the others arrived", b,
		"proj/ | proj/a.txt=B-a | proj/b.txt=A-b | proj/sub/ | "
		"proj/sub/c.txt=B-c | proj/sub/d.txt=A-d");
	Expect("source: the skipped files are still there", a,
		"proj/ | proj/a.txt=A-a | proj/sub/ | proj/sub/c.txt=A-c");
}


static void
MoveAcrossVolumesSkippedTopLevel()
{
	// #155 (2): a move that was skipped must not leave an undo item that
	// moves the file that was there before
	MakeFile(P(a, "s8.txt"), "A");
	MakeFile(P(b, "s8.txt"), "B", 10);

	PushSentinel();
	Operate(kMoveSelectionTo, P(a, "s8.txt"), b);
	Expect("destination after the skipped move", b, "s8.txt=B");
	Expect("source after the skipped move", a, "s8.txt=A");

	UndoIfOffered();
	Expect("destination after Undo", b, "s8.txt=B");
	Expect("source after Undo", a, "s8.txt=A");
}


static void
CopySkippedUndo()
{
	// #155 (2): Undo of a copy that skipped a file deleted the file that was
	// there before
	MakeFile(P(a, "notes2.txt"), "A");
	MakeFile(P(b, "notes2.txt"), "B", 10);

	PushSentinel();
	Operate(kCopySelectionTo, P(a, "notes2.txt"), b);
	Expect("destination after the skipped copy", b, "notes2.txt=B");

	UndoIfOffered();
	Expect("destination after Undo", b, "notes2.txt=B");
	Expect("source after Undo", a, "notes2.txt=A");
}


static void
CopyCanceledUndo()
{
	// #155 (2): a copy cancelled in the window for name clashes left an undo
	// item that deleted the folder in the destination
	MakeFile(P(a, "proj3/x.txt"), "A-x");
	MakeFile(P(b, "proj3/x.txt"), "B-x", 10);
	MakeFile(P(b, "proj3/mine.txt"), "B-mine");

	PushSentinel();
	Operate(kCopySelectionTo, P(a, "proj3"), b, kAnswerCancel);
	Expect("destination after the cancelled copy", b,
		"proj3/ | proj3/mine.txt=B-mine | proj3/x.txt=B-x");

	UndoIfOffered();
	Expect("destination after Undo", b,
		"proj3/ | proj3/mine.txt=B-mine | proj3/x.txt=B-x");
}


static void
CopyMergedUndo()
{
	// #155 (2): Undo of a copy into a folder that was there before deletes
	// what the copy made, not the folder
	MakeFile(P(a, "proj4/n1.txt"), "A-1");
	MakeFile(P(a, "proj4/sub/n2.txt"), "A-2");
	MakeFile(P(a, "proj4/new/n3.txt"), "A-3");
	MakeFile(P(a, "proj4/both.txt"), "A-both", 10);
	MakeFile(P(b, "proj4/mine.txt"), "B-mine");
	MakeFile(P(b, "proj4/sub/mine2.txt"), "B-mine2");
	MakeFile(P(b, "proj4/both.txt"), "B-both");

	PushSentinel();
	Operate(kCopySelectionTo, P(a, "proj4"), b, kAnswerKeepBoth);
	Expect("destination after the copy", b,
		"proj4/ | proj4/both 2.txt=A-both | proj4/both.txt=B-both | "
		"proj4/mine.txt=B-mine | proj4/n1.txt=A-1 | proj4/new/ | "
		"proj4/new/n3.txt=A-3 | proj4/sub/ | proj4/sub/mine2.txt=B-mine2 | "
		"proj4/sub/n2.txt=A-2");

	UndoIfOffered();
	Expect("destination after Undo: what was there before", b,
		"proj4/ | proj4/both.txt=B-both | proj4/mine.txt=B-mine | proj4/sub/ | "
		"proj4/sub/mine2.txt=B-mine2");
}


static void
CopyUndoRedo()
{
	// Undo and Redo of a copy without clashes have to work as before
	MakeFile(P(a, "f5.txt"), "A-f");
	MakeFile(P(a, "dir5/g.txt"), "A-g");
	MakeDir(b);

	std::vector<BString> sources;
	sources.push_back(P(a, "f5.txt"));
	sources.push_back(P(a, "dir5"));

	PushSentinel();
	Operate(kCopySelectionTo, sources, b);
	Expect("destination after the copy", b,
		"dir5/ | dir5/g.txt=A-g | f5.txt=A-f");
	if (UndoOffered().Length() == 0)
		Fail("the copy left no undo item");

	UndoIfOffered();
	Expect("destination after Undo", b, "");

	BString redo;
	if (!FSGetRedoDescription(redo))
		Fail("nothing to redo after Undo");
	UndoOrRedo(true);
	Expect("destination after Redo", b,
		"dir5/ | dir5/g.txt=A-g | f5.txt=A-f");

	UndoIfOffered();
	Expect("destination after the second Undo", b, "");
	Expect("source untouched", a, "dir5/ | dir5/g.txt=A-g | f5.txt=A-f");
}


static void
DuplicateAndLinkUndo()
{
	// Undo of Duplicate and of Create link has to work as before
	MakeFile(P(a, "d9.txt"), "A-d");
	MakeDir(b);

	std::vector<BString> sources;
	sources.push_back(P(a, "d9.txt"));

	PushSentinel();
	FSDuplicate(RefList(sources), NULL);
	WaitForEngine();
	Expect("after Duplicate", a, "d9.txt copy=A-d | d9.txt=A-d");
	UndoIfOffered();
	Expect("after Undo", a, "d9.txt=A-d");

	PushSentinel();
	Operate(kCreateLink, P(a, "d9.txt"), b);
	Expect("after Create link", b, "d9.txt@");
	UndoIfOffered();
	Expect("after Undo", b, "");
}


static void
RedoAfterTheUserMadeTheirOwn()
{
	// a file the user put where an undone copy was is theirs: after a Redo
	// that skips it, Undo must leave it alone
	MakeFile(P(a, "r6.txt"), "A");
	MakeDir(b);

	PushSentinel();
	Operate(kCopySelectionTo, P(a, "r6.txt"), b);
	UndoIfOffered();
	Expect("destination after Undo", b, "");

	MakeFile(P(b, "r6.txt"), "the user's own", 10);
	UndoOrRedo(true, kAnswerSkip);
	Expect("destination after Redo, skipped", b, "r6.txt=the user's own");

	UndoIfOffered();
	Expect("destination after Undo of the Redo", b, "r6.txt=the user's own");
}


static void
MoveKeepBothUndo()
{
	// #155 (2): after "Keep both" Undo of a move took the file that was there
	// before and left the moved one
	MakeFile(P(a, "src/k7.txt"), "moved");
	MakeFile(P(a, "dst/k7.txt"), "was there", 10);

	PushSentinel();
	Operate(kMoveSelectionTo, P(a, "src/k7.txt"), P(a, "dst"),
		kAnswerKeepBoth);
	Expect("after the move", a,
		"dst/ | dst/k7 2.txt=moved | dst/k7.txt=was there | src/");

	UndoIfOffered();
	Expect("after Undo: the moved file is back, the other stayed", a,
		"dst/ | dst/k7.txt=was there | src/ | src/k7 2.txt=moved");
}


static void
MoveUndoRedo(bool acrossVolumes)
{
	// Undo and Redo of a move without clashes have to work as before
	BString dest = acrossVolumes ? b : P(a, "dst");
	MakeFile(P(a, "src/m8.txt"), "A-m");
	MakeFile(P(a, "src/dir8/n.txt"), "A-n");
	MakeDir(dest);

	std::vector<BString> sources;
	sources.push_back(P(a, "src/m8.txt"));
	sources.push_back(P(a, "src/dir8"));

	PushSentinel();
	Operate(kMoveSelectionTo, sources, dest);
	Expect("destination after the move", dest,
		"dir8/ | dir8/n.txt=A-n | m8.txt=A-m");
	Expect("source after the move", P(a, "src"), "");
	if (UndoOffered().Length() == 0)
		Fail("the move left no undo item");

	UndoIfOffered();
	Expect("destination after Undo", dest, "");
	Expect("source after Undo", P(a, "src"),
		"dir8/ | dir8/n.txt=A-n | m8.txt=A-m");

	UndoOrRedo(true);
	Expect("destination after Redo", dest,
		"dir8/ | dir8/n.txt=A-n | m8.txt=A-m");
	Expect("source after Redo", P(a, "src"), "");

	UndoIfOffered();
	Expect("source after the second Undo", P(a, "src"),
		"dir8/ | dir8/n.txt=A-n | m8.txt=A-m");
}


static void
MoveUndoRedoSameVolume()
{
	MoveUndoRedo(false);
}


static void
MoveUndoRedoAcrossVolumes()
{
	MoveUndoRedo(true);
}


static void
FolderOntoFile(uint32 mode, bool sameVolume, Answer answer)
{
	// #155 (3): a folder that meets a file of its name must not remove it
	BString source = P(a, "src");
	BString dest = sameVolume ? P(a, "dst") : b;
	MakeFile(P(source, "data/in.txt"), "A-in");
	MakeFile(P(dest, "data"), "precious file");

	Operate(mode, P(source, "data"), dest, answer);
	if (sConflictWindows != 1 || sConflictItems != 1)
		Fail("the clash was not listed in the window for name clashes");

	if (answer == kAnswerKeepBoth) {
		Expect("destination: the file and the folder", dest,
			"data 2/ | data 2/in.txt=A-in | data=precious file");
		Expect("source", source, mode == kMoveSelectionTo
			? "" : "data/ | data/in.txt=A-in");
	} else {
		Expect("destination: the file is still there", dest,
			"data=precious file");
		Expect("source: the folder is still there", source,
			"data/ | data/in.txt=A-in");
	}
}


static void
FolderOntoFileCopy()
{
	FolderOntoFile(kCopySelectionTo, false, kAnswerDefault);
}


static void
FolderOntoFileCopyReplace()
{
	// "Replace" for all must not reach an item that cannot be replaced
	FolderOntoFile(kCopySelectionTo, false, kAnswerReplace);
}


static void
FolderOntoFileCopyKeepBoth()
{
	FolderOntoFile(kCopySelectionTo, false, kAnswerKeepBoth);
}


static void
FolderOntoFileMoveAcrossVolumes()
{
	FolderOntoFile(kMoveSelectionTo, false, kAnswerDefault);
}


static void
FolderOntoFileMoveAcrossVolumesKeepBoth()
{
	FolderOntoFile(kMoveSelectionTo, false, kAnswerKeepBoth);
}


static void
FolderOntoFileMoveSameVolume()
{
	FolderOntoFile(kMoveSelectionTo, true, kAnswerDefault);
}


static void
FolderOntoFileMoveSameVolumeKeepBoth()
{
	FolderOntoFile(kMoveSelectionTo, true, kAnswerKeepBoth);
}


static void
FolderOntoFileNested(uint32 mode, bool sameVolume, Answer answer)
{
	// #155 (3), inside folders that merge
	BString source = P(a, "src");
	BString dest = sameVolume ? P(a, "dst") : b;
	MakeFile(P(source, "top/data/in.txt"), "A-in");
	MakeFile(P(source, "top/other.txt"), "A-other");
	MakeFile(P(source, "top/thing"), "A-thing");
	MakeFile(P(dest, "top/data"), "precious file");
	MakeFile(P(dest, "top/thing/kept.txt"), "precious folder");

	Operate(mode, P(source, "top"), dest, answer);
	if (sConflictWindows != 1 || sConflictItems != 2)
		Fail("the clashes were not listed in the window for name clashes");

	if (answer == kAnswerKeepBoth) {
		Expect("destination: both of each", dest,
			"top/ | top/data 2/ | top/data 2/in.txt=A-in | "
			"top/data=precious file | top/other.txt=A-other | "
			"top/thing 2=A-thing | top/thing/ | "
			"top/thing/kept.txt=precious folder");
		Expect("source", source, mode == kMoveSelectionTo ? ""
			: "top/ | top/data/ | top/data/in.txt=A-in | "
				"top/other.txt=A-other | top/thing=A-thing");
	} else {
		Expect("destination: what was there, and what did not clash", dest,
			"top/ | top/data=precious file | top/other.txt=A-other | "
			"top/thing/ | top/thing/kept.txt=precious folder");
		Expect("source: what clashed is still there", source,
			mode == kMoveSelectionTo
				? "top/ | top/data/ | top/data/in.txt=A-in | top/thing=A-thing"
				: "top/ | top/data/ | top/data/in.txt=A-in | "
					"top/other.txt=A-other | top/thing=A-thing");
	}
}


static void
FolderOntoFileNestedCopy()
{
	FolderOntoFileNested(kCopySelectionTo, false, kAnswerDefault);
}


static void
FolderOntoFileNestedCopyKeepBoth()
{
	FolderOntoFileNested(kCopySelectionTo, false, kAnswerKeepBoth);
}


static void
FolderOntoFileNestedMoveAcrossVolumes()
{
	FolderOntoFileNested(kMoveSelectionTo, false, kAnswerDefault);
}


static void
FolderOntoFileNestedMoveAcrossVolumesKeepBoth()
{
	FolderOntoFileNested(kMoveSelectionTo, false, kAnswerKeepBoth);
}


static void
FolderOntoFileNestedMoveSameVolume()
{
	FolderOntoFileNested(kMoveSelectionTo, true, kAnswerDefault);
}


static void
FolderOntoFileNestedMoveSameVolumeKeepBoth()
{
	FolderOntoFileNested(kMoveSelectionTo, true, kAnswerKeepBoth);
}


static void
FileOntoFolder()
{
	// #155 (3), the other way round: a file that meets a folder
	MakeFile(P(a, "data"), "A-file");
	MakeFile(P(b, "data/kept.txt"), "precious");

	Operate(kMoveSelectionTo, P(a, "data"), b);
	Expect("destination: the folder is still there", b,
		"data/ | data/kept.txt=precious");
	Expect("source: the file is still there", a, "data=A-file");
}


static void
MoveSameVolumeMerge()
{
	// a move within a volume into a folder that is there: skipped files
	// stay, the others move (as before)
	MakeFile(P(a, "src/proj/a.txt"), "A-a");
	MakeFile(P(a, "src/proj/b.txt"), "A-b");
	MakeFile(P(a, "src/proj/sub/c.txt"), "A-c");
	MakeFile(P(a, "src/proj/both/e.txt"), "A-e");
	MakeDir(P(a, "src/proj/empty"));
	MakeFile(P(a, "dst/proj/a.txt"), "D-a", 10);
	MakeFile(P(a, "dst/proj/mine.txt"), "D-mine");
	MakeFile(P(a, "dst/proj/both/f.txt"), "D-f");

	Operate(kMoveSelectionTo, P(a, "src/proj"), P(a, "dst"));
	Expect("destination", P(a, "dst"),
		"proj/ | proj/a.txt=D-a | proj/b.txt=A-b | proj/both/ | "
		"proj/both/e.txt=A-e | proj/both/f.txt=D-f | proj/empty/ | "
		"proj/mine.txt=D-mine | proj/sub/ | proj/sub/c.txt=A-c");
	Expect("source", P(a, "src"), "proj/ | proj/a.txt=A-a");
}


static void
MoveAcrossVolumesMany()
{
	// a folder with many files and folders moves to another volume: all of
	// it arrives and nothing stays (the source is read while it empties)
	std::vector<std::string> wanted;
	wanted.push_back("many/");
	for (int32 d = 0; d < 4; d++) {
		BString dir("many");
		if (d > 0)
			dir << "/sub" << d;
		if (d > 0)
			wanted.push_back(std::string(dir.String()) + "/");
		for (int32 i = 0; i < 150; i++) {
			BString name(dir);
			name << "/file-" << i << ".txt";
			BString contents;
			contents << "contents " << d << "." << i;
			MakeFile(P(a, name.String()), contents.String());
			wanted.push_back(std::string(name.String()) + "="
				+ contents.String());
		}
	}
	std::sort(wanted.begin(), wanted.end());
	BString expected;
	for (size_t i = 0; i < wanted.size(); i++) {
		if (i > 0)
			expected << " | ";
		expected << wanted[i].c_str();
	}
	MakeDir(b);

	Operate(kMoveSelectionTo, P(a, "many"), b);

	sChecks += 2;
	BString arrived = Tree(b);
	BString left = Tree(a);
	if (arrived == expected)
		printf("    ok: all 600 files in 4 folders arrived\n");
	else {
		std::vector<std::string> lines;
		ListTree(b, "", lines);
		printf("    FAIL: %d of %d entries arrived\n", (int)lines.size(),
			(int)wanted.size());
		sFailures++;
	}
	if (left == "")
		printf("    ok: nothing is left in the source\n");
	else {
		std::vector<std::string> lines;
		ListTree(a, "", lines);
		printf("    FAIL: %d entries are left in the source\n",
			(int)lines.size());
		sFailures++;
	}
}


static void
MoveAcrossVolumesFailedCopy()
{
	// #155 (1): a file whose copy failed, and the user went on, must not be
	// removed by the move
	if (sFat.Length() == 0) {
		printf("    (no FAT volume given: not run)\n");
		return;
	}

	BString dest = P(sFat, "t");
	RemoveTree(dest.String());
	MakeDir(dest);
	MakeFile(P(a, "f/good.txt"), "A-good");
	MakeFile(P(a, "f/what?.txt"), "A-what");
	symlink("good.txt", P(a, "f/link").String());
	MakeFile(P(a, "top?.txt"), "A-top");

	std::vector<BString> sources;
	sources.push_back(P(a, "f"));
	sources.push_back(P(a, "top?.txt"));
	Operate(kMoveSelectionTo, sources, dest, kAnswerDefault, 1);
	if (sAlerts.size() != 3)
		Fail("not three copies failed");
	Expect("destination: what could be copied", dest,
		"f/ | f/good.txt=A-good");
	Expect("source: what could not be copied is still there", a,
		"f/ | f/link@ | f/what?.txt=A-what | top?.txt=A-top");

	RemoveTree(dest.String());
}


static void
TrashSameName()
{
	// two folders of one name moved to the Trash have to be two folders there
	BDirectory trash;
	dev_t device = dev_for_path(sA.String());
	if (FSGetTrashDir(&trash, device) != B_OK) {
		Fail("no Trash on the first volume");
		return;
	}
	BPath trashPath(&trash, NULL);
	RemoveTree(P(trashPath.Path(), "proj14").String());
	RemoveTree(P(trashPath.Path(), "proj14 copy").String());

	MakeFile(P(a, "x/proj14/one.txt"), "one");
	MakeFile(P(a, "y/proj14/two.txt"), "two");
	MakeFile(P(a, "y/proj14/one.txt"), "another one");

	std::vector<BString> sources;
	sources.push_back(P(a, "x/proj14"));
	FSMoveToTrash(RefList(sources), NULL, false);
	WaitForEngine();
	sources.clear();
	sources.push_back(P(a, "y/proj14"));
	PushSentinel();
	FSMoveToTrash(RefList(sources), NULL, false);
	WaitForEngine();

	sChecks++;
	BString first = Tree(P(trashPath.Path(), "proj14"));
	BString second = Tree(P(trashPath.Path(), "proj14 copy"));
	if (first == "one.txt=one"
		&& second == "one.txt=another one | two.txt=two") {
		printf("    ok: two folders in the Trash: [%s] and [%s]\n",
			first.String(), second.String());
	} else {
		printf("    FAIL: in the Trash: proj14 [%s], proj14 copy [%s]\n",
			first.String(), second.String());
		sFailures++;
	}

	UndoIfOffered();
	Expect("Undo brings the second folder back", P(a, "y"),
		"proj14/ | proj14/one.txt=another one | proj14/two.txt=two");

	RemoveTree(P(trashPath.Path(), "proj14").String());
	RemoveTree(P(trashPath.Path(), "proj14 copy").String());
}


struct Scenario {
	const char*	name;
	void		(*function)();
};

static const Scenario kScenarios[] = {
	{ "move-across-volumes-skipped", MoveAcrossVolumesSkipped },
	{ "move-across-volumes-skipped-top-level",
		MoveAcrossVolumesSkippedTopLevel },
	{ "copy-skipped-undo", CopySkippedUndo },
	{ "copy-canceled-undo", CopyCanceledUndo },
	{ "copy-merged-undo", CopyMergedUndo },
	{ "copy-undo-redo", CopyUndoRedo },
	{ "duplicate-and-link-undo", DuplicateAndLinkUndo },
	{ "redo-after-the-user-made-their-own", RedoAfterTheUserMadeTheirOwn },
	{ "move-keep-both-undo", MoveKeepBothUndo },
	{ "move-undo-redo-same-volume", MoveUndoRedoSameVolume },
	{ "move-undo-redo-across-volumes", MoveUndoRedoAcrossVolumes },
	{ "folder-onto-file-copy", FolderOntoFileCopy },
	{ "folder-onto-file-copy-replace", FolderOntoFileCopyReplace },
	{ "folder-onto-file-copy-keep-both", FolderOntoFileCopyKeepBoth },
	{ "folder-onto-file-move-across-volumes",
		FolderOntoFileMoveAcrossVolumes },
	{ "folder-onto-file-move-across-volumes-keep-both",
		FolderOntoFileMoveAcrossVolumesKeepBoth },
	{ "folder-onto-file-move-same-volume", FolderOntoFileMoveSameVolume },
	{ "folder-onto-file-move-same-volume-keep-both",
		FolderOntoFileMoveSameVolumeKeepBoth },
	{ "folder-onto-file-nested-copy", FolderOntoFileNestedCopy },
	{ "folder-onto-file-nested-copy-keep-both",
		FolderOntoFileNestedCopyKeepBoth },
	{ "folder-onto-file-nested-move-across-volumes",
		FolderOntoFileNestedMoveAcrossVolumes },
	{ "folder-onto-file-nested-move-across-volumes-keep-both",
		FolderOntoFileNestedMoveAcrossVolumesKeepBoth },
	{ "folder-onto-file-nested-move-same-volume",
		FolderOntoFileNestedMoveSameVolume },
	{ "folder-onto-file-nested-move-same-volume-keep-both",
		FolderOntoFileNestedMoveSameVolumeKeepBoth },
	{ "file-onto-folder", FileOntoFolder },
	{ "move-same-volume-merge", MoveSameVolumeMerge },
	{ "move-across-volumes-many", MoveAcrossVolumesMany },
	{ "move-across-volumes-failed-copy", MoveAcrossVolumesFailedCopy },
	{ "trash-same-name", TrashSameName },
	{ NULL, NULL }
};


static status_t
TestThread(void*)
{
	for (int32 i = 0; kScenarios[i].name != NULL; i++) {
		if (sOnly != NULL && strcmp(sOnly, kScenarios[i].name) != 0)
			continue;

		BString name;
		name << "t" << i;
		a = P(sA, name.String());
		b = P(sB, name.String());
		RemoveTree(a.String());
		RemoveTree(b.String());

		int failuresBefore = sFailures;
		printf("%s\n", kScenarios[i].name);
		kScenarios[i].function();
		printf("  %s\n", sFailures == failuresBefore ? "PASSED" : "FAILED");
		fflush(stdout);

		RemoveTree(a.String());
		RemoveTree(b.String());
	}

	printf("%d checks, %d failed\n", sChecks, sFailures);
	fflush(stdout);
	be_app->PostMessage(B_QUIT_REQUESTED);
	return B_OK;
}


static void
Crashed(int signal)
{
	printf("FAIL: signal %d\n", signal);
	fflush(stdout);
	_exit(1);
}


int
main(int argc, char** argv)
{
	signal(SIGSEGV, Crashed);
	signal(SIGABRT, Crashed);

	std::vector<const char*> folders;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-only") == 0 && i + 1 < argc)
			sOnly = argv[++i];
		else if (strcmp(argv[i], "-fat") == 0 && i + 1 < argc)
			sFat = argv[++i];
		else
			folders.push_back(argv[i]);
	}
	if (folders.size() < 2) {
		fprintf(stderr, "usage: %s <folder on one volume> <folder on another "
			"volume> [-fat <folder on a FAT volume>] [-only <name>]\n",
			argv[0]);
		return 2;
	}

	sA = folders[0];
	sB = folders[1];
	create_directory(sA.String(), 0755);
	create_directory(sB.String(), 0755);
	if (dev_for_path(sA.String()) == dev_for_path(sB.String())) {
		fprintf(stderr, "%s and %s are on the same volume\n", sA.String(),
			sB.String());
		return 2;
	}

	BApplication app("application/x-vnd.airos-tracker-fsops-test");

	thread_id thread = spawn_thread(TestThread, "test", B_NORMAL_PRIORITY,
		NULL);
	resume_thread(thread);
	app.Run();

	return sFailures == 0 ? 0 : 1;
}
