/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "UpdateWorker.h"

#include <OS.h>

#include "Release.h"
#include "SystemState.h"


UpdateWorker::UpdateWorker(const BMessenger& target)
	:
	BLooper("update worker"),
	fTarget(target),
	fUpdater(this),
	fLastProgress(0)
{
}


void
UpdateWorker::Cancel()
{
	fUpdater.Cancel();
}


void
UpdateWorker::MessageReceived(BMessage* message)
{
	fUpdater.Reset();
	switch (message->what) {
		case kMsgCheck:
			_Check();
			break;
		case kMsgInstall:
			_Install(message->GetString("id", NULL));
			break;
		case kMsgGoBack:
			_GoBack(message->GetString("state", ""));
			break;
		case kMsgRemoveFailed:
			_RemoveFailed();
			break;
		default:
			BLooper::MessageReceived(message);
	}
}


void
UpdateWorker::UpdaterStatus(const char* text)
{
	fStatusText = text;
	BMessage status(kMsgStatus);
	status.AddString("text", text);
	status.AddFloat("fraction", -1);
	fTarget.SendMessage(&status);
}


void
UpdateWorker::UpdaterProgress(float fraction)
{
	// at most ten reports a second
	bigtime_t now = system_time();
	if (fraction >= 0 && fraction < 1 && now - fLastProgress < 100000)
		return;
	fLastProgress = now;
	BMessage status(kMsgStatus);
	status.AddString("text", fStatusText);
	status.AddFloat("fraction", fraction);
	fTarget.SendMessage(&status);
}


/*static*/ BString
UpdateWorker::FormatChanges(const BMessage& changes, const char* title)
{
	BString text;
	text << title << "\n";

	BMessage packages;
	if (json::Object(changes, "packages", packages)
		&& json::Count(packages) > 0) {
		text << "\nUpdated applications and packages:\n";
		BMessage entry;
		for (int32 i = 0; json::At(packages, i, entry); i++) {
			BString from = json::String(entry, "from");
			BString to = json::String(entry, "to");
			text << "  • " << json::String(entry, "name");
			if (from.IsEmpty())
				text << " " << to << " (new)";
			else if (to.IsEmpty())
				text << " (removed)";
			else
				text << " " << from << " → " << to;
			text << "\n";
		}
	}

	BMessage apps;
	if (json::Object(changes, "apps", apps)) {
		BMessage app;
		for (int32 i = 0; json::At(apps, i, app); i++) {
			BMessage commits;
			if (!json::Object(app, "commits", commits)
				|| json::Count(commits) == 0) {
				continue;
			}
			text << "\n" << json::String(app, "name") << ":\n";
			BMessage commit;
			for (int32 j = 0; json::At(commits, j, commit); j++)
				text << "  • " << json::String(commit, "subject") << "\n";
		}
	}

	BMessage haiku;
	if (json::Object(changes, "haiku", haiku) && json::Count(haiku) > 0) {
		text << "\nSystem:\n";
		BMessage commit;
		int32 count = json::Count(haiku);
		for (int32 i = 0; json::At(haiku, i, commit); i++)
			text << "  • " << json::String(commit, "subject") << "\n";
		int32 more = (int32)json::Number(changes, "haiku_more", 0);
		if (more > 0)
			text << "  … and " << more << " more\n";
		(void)count;
	}

	int32 skipped = (int32)json::Number(changes, "skipped", 0);
	if (skipped > 0)
		text << "\n(" << skipped << " documentation and build changes)\n";
	return text;
}


void
UpdateWorker::_Check()
{
	BMessage reply(kMsgCheckDone);
	BString error;
	if (fUpdater.Check(fFeed, error) != B_OK) {
		reply.AddString("error", error);
		fTarget.SendMessage(&reply);
		return;
	}

	// everything newer than what runs, newest first
	BString changes;
	int32 newer = 0;
	const FeedBuild* latest = fFeed.Latest();
	for (int32 i = 0; const FeedBuild* build = fFeed.BuildAt(i); i++) {
		if (!fUpdater.IsNewer(*build))
			break;
		newer++;
		if (!changes.IsEmpty())
			changes << "\n";
		changes << FormatChanges(build->changes,
			Release::TitleForVersion(build->version));
	}

	reply.AddBool("available", latest != NULL && newer > 0);
	reply.AddInt32("count", newer);
	if (latest != NULL) {
		reply.AddString("id", latest->id);
		reply.AddString("title", Release::TitleForVersion(latest->version));
		reply.AddInt64("size", latest->size);
		reply.AddString("revision", latest->haikuRevision);
	}
	reply.AddString("changes", changes);
	fTarget.SendMessage(&reply);
}


void
UpdateWorker::_Install(const char* id)
{
	BMessage reply(kMsgInstallDone);
	BString error;
	const FeedBuild* build = id != NULL ? fFeed.FindBuild(id) : NULL;
	if (build == NULL) {
		if (fUpdater.Check(fFeed, error) == B_OK)
			build = id != NULL ? fFeed.FindBuild(id) : fFeed.Latest();
	}
	if (build == NULL) {
		reply.AddString("error", error.IsEmpty()
			? BString("The build server no longer offers that release.")
			: error);
		fTarget.SendMessage(&reply);
		return;
	}

	Release release;
	SystemState state;
	UpdatePlan plan;
	status_t result = fUpdater.FetchRelease(*build, release, error);
	if (result == B_OK)
		result = state.Load();
	if (result == B_OK)
		result = fUpdater.CheckPreconditions(state, error);
	if (result == B_OK)
		result = fUpdater.PlanUpdate(release, state, plan, error, &fFeed);
	if (result == B_OK)
		result = fUpdater.Download(plan, error);
	if (result == B_OK)
		result = fUpdater.Apply(plan, error);

	if (result != B_OK)
		reply.AddString("error", error.IsEmpty() ? strerror(result) : error);
	reply.AddString("title", release.Title());
	fTarget.SendMessage(&reply);
}


void
UpdateWorker::_GoBack(const char* stateName)
{
	BMessage reply(kMsgGoBackDone);
	BString error;
	SystemState state;
	UpdatePlan plan;
	status_t result = state.Load();
	const InstalledVersion* version = state.FindState(stateName);
	if (result == B_OK && version == NULL) {
		error = "That version is no longer installed.";
		result = B_ENTRY_NOT_FOUND;
	}
	if (result == B_OK)
		result = fUpdater.CheckPreconditions(state, error);
	if (result == B_OK)
		result = fUpdater.PlanGoBack(*version, state, plan, error);
	if (result == B_OK)
		result = fUpdater.Apply(plan, error);
	if (result != B_OK)
		reply.AddString("error", error.IsEmpty() ? strerror(result) : error);
	if (version != NULL)
		reply.AddString("title", version->Title());
	fTarget.SendMessage(&reply);
}


void
UpdateWorker::_RemoveFailed()
{
	BMessage reply(kMsgRemoveFailedDone);
	BString error;
	if (fUpdater.RemoveFailedUpdate(error) != B_OK)
		reply.AddString("error", error);
	fTarget.SendMessage(&reply);
}
