/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "UpdateWorker.h"

#include <OS.h>

#include <RegistrarDefs.h>
#include <RosterPrivate.h>

#include "Desktop.h"
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
		case kMsgCloseApps:
			_CloseApps();
			break;
		case kMsgCloseDesktop:
			_CloseDesktop();
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
		for (int32 i = 0; json::At(haiku, i, commit); i++)
			text << "  • " << json::String(commit, "subject") << "\n";
		int32 more = (int32)json::Number(changes, "haiku_more", 0);
		if (more > 0)
			text << "  … and " << more << " more\n";
	}

	int32 skipped = (int32)json::Number(changes, "skipped", 0);
	if (skipped > 0)
		text << "\n(" << skipped << " documentation and build changes)\n";

	// a release that changed packages only has no notes
	if (!text.IsEmpty())
		text.Prepend(BString(title) << "\n");
	return text;
}


/*!	One row per package the plan changes: what is installed now and what
	the update brings. A package new to this system has no "from", one the
	update removes no "to".
*/
void
UpdateWorker::_AddPackageRows(const UpdatePlan& plan, BMessage& reply)
{
	BStringList removed(plan.deactivate);
	for (int32 i = 0; const PlannedPackage* package = plan.activate.ItemAt(i);
			i++) {
		BString name = Updater::PackageNameOf(package->file);
		BString from;
		for (int32 j = 0; j < removed.CountStrings(); j++) {
			if (Updater::PackageNameOf(removed.StringAt(j)) == name) {
				from = Updater::VersionOf(removed.StringAt(j));
				removed.Remove(j);
				break;
			}
		}
		BMessage row;
		row.AddString("name", name);
		row.AddString("from", from);
		row.AddString("to", Updater::VersionOf(package->file));
		row.AddInt64("size", package->download ? package->size : -1);
		reply.AddMessage("package", &row);
	}
	for (int32 i = 0; i < removed.CountStrings(); i++) {
		BMessage row;
		row.AddString("name", Updater::PackageNameOf(removed.StringAt(i)));
		row.AddString("from", Updater::VersionOf(removed.StringAt(i)));
		row.AddString("to", "");
		row.AddInt64("size", -1);
		reply.AddMessage("package", &row);
	}
}


/*!	Without a plan, what the feed says each newer release changed, merged:
	the oldest "from" and the newest "to" of every package.
*/
void
UpdateWorker::_AddFeedPackageRows(BMessage& reply)
{
	BStringList names;
	BStringList froms;
	BStringList tos;
	for (int32 i = 0; const FeedBuild* build = fFeed.BuildAt(i); i++) {
		if (!fUpdater.IsNewer(*build))
			break;
		// newest first: the first "to" seen stays, "from" goes back in time
		BMessage packages;
		if (!json::Object(build->changes, "packages", packages))
			continue;
		BMessage entry;
		for (int32 j = 0; json::At(packages, j, entry); j++) {
			BString name = json::String(entry, "name");
			int32 index = names.IndexOf(name);
			if (index < 0) {
				names.Add(name);
				froms.Add(json::String(entry, "from"));
				tos.Add(json::String(entry, "to"));
			} else
				froms.Replace(index, json::String(entry, "from"));
		}
	}
	for (int32 i = 0; i < names.CountStrings(); i++) {
		if (froms.StringAt(i) == tos.StringAt(i))
			continue;
		BMessage row;
		row.AddString("name", names.StringAt(i));
		row.AddString("from", froms.StringAt(i));
		row.AddString("to", tos.StringAt(i));
		row.AddInt64("size", -1);
		reply.AddMessage("package", &row);
	}
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
		BString notes = FormatChanges(build->changes,
			Release::TitleForVersion(build->version));
		if (notes.IsEmpty())
			continue;
		if (!changes.IsEmpty())
			changes << "\n";
		changes << notes;
	}

	reply.AddBool("available", latest != NULL && newer > 0);
	reply.AddInt32("count", newer);
	if (latest != NULL) {
		reply.AddString("id", latest->id);
		reply.AddString("title", Release::TitleForVersion(latest->version));
		reply.AddString("revision", latest->haikuRevision);

		// what installing it would download: only what changed
		off_t size = latest->size;
		int32 packages = -1;
		Release release;
		SystemState state;
		UpdatePlan plan;
		BString planError;
		if (newer > 0 && fUpdater.FetchRelease(*latest, release, planError)
				== B_OK
			&& state.Load() == B_OK
			&& fUpdater.PlanUpdate(release, state, plan, planError, &fFeed)
				== B_OK) {
			size = plan.downloadBytes;
			packages = plan.activate.CountItems();
			_AddPackageRows(plan, reply);
		} else if (newer > 0) {
			fUpdater.Log("could not work out what %s would download: %s",
				latest->id.String(), planError.String());
			_AddFeedPackageRows(reply);
		}
		reply.AddInt64("size", size);
		reply.AddInt32("packages", packages);
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
	if (result == B_OK && !fUpdater.IsCancelled()) {
		fTarget.SendMessage(kMsgInstallCommitting);
		result = fUpdater.Apply(plan, error);
	}

	if (result != B_OK)
		reply.AddString("error", error.IsEmpty() ? strerror(result) : error);
	reply.AddBool("cancelled", result == B_CANCELED || fUpdater.IsCancelled());
	reply.AddString("title", release.Title().IsEmpty()
		? Release::TitleForVersion(build->version) : release.Title());
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


/*!	Asks the registrar to quit the user's applications the way a shutdown
	does, but for this one: each may ask to save its work or refuse, which
	stops it all. Blocks until they are gone or one refused.
*/
void
UpdateWorker::_CloseApps()
{
	BMessage request(BPrivate::B_REG_QUIT_USER_APPS);
	BMessage answer;
	status_t error = BRoster::Private().SendTo(&request, &answer, false);
	BMessage reply(kMsgCloseAppsDone);
	if (error == B_OK && answer.what == B_MESSAGE_NOT_UNDERSTOOD) {
		// a registrar from before this request
		error = B_NOT_SUPPORTED;
	} else if (error == B_OK && answer.what != BPrivate::B_REG_SUCCESS) {
		error = answer.GetInt32("error", B_ERROR);
		reply.AddString("app", answer.GetString("app", ""));
	}
	reply.AddInt32("error", error);
	fTarget.SendMessage(&reply);
}


/*!	Deskbar and Tracker quit too, as in a shutdown: what stays on the screen
	is the install window over the desktop color.
*/
void
UpdateWorker::_CloseDesktop()
{
	Desktop::Close();
	fTarget.SendMessage(kMsgCloseDesktopDone);
}
