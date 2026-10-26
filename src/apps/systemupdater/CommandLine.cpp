/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "CommandLine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <Notification.h>
#include <Roster.h>

#include <airos_boot_control.h>

#include "Constants.h"
#include "Feed.h"
#include "Release.h"
#include "Settings.h"
#include "SystemState.h"
#include "Updater.h"


static const char* const kUsage =
	"Usage: SystemUpdater [--feed <url>] <command>\n"
	"Without a command, System Updater opens its window.\n"
	"\n"
	"  --status              what runs, what waits, the installed versions\n"
	"  --check               the releases the feed offers\n"
	"  --plan [<id>]         what installing a release would change\n"
	"  --update [<id>]       download and install a release (default: the\n"
	"                        newest); it starts with the next restart\n"
	"  --restart-into <state>\n"
	"                        start an installed version once at the next\n"
	"                        restart (\"latest\" for the current one)\n"
	"  --cancel-restart-into\n"
	"  --go-back <state>     make an installed version the current one again\n"
	"  --retry               try an update that did not start once more\n"
	"  --remove-failed       remove an update that did not start\n"
	"  --boot-check [--no-wait]\n"
	"                        what the desktop runs at each start: confirm a\n"
	"                        new update, report a fallback\n"
	"  --confirm             confirm the update on trial now\n"
	"  --prune [<n>]         keep only <n> older versions (default 2)\n";


namespace {

class PrintingListener : public UpdaterListener {
public:
	PrintingListener()
		:
		fLastPercent(-1)
	{
	}

	virtual void UpdaterStatus(const char* text)
	{
		printf("%s\n", text);
		fflush(stdout);
	}

	virtual void UpdaterProgress(float fraction)
	{
		// every tenth, not after each status line of a long list
		int percent = fraction < 0 ? -1 : (int)(fraction * 100);
		if (percent >= 0 && (fLastPercent < 0
				|| percent / 10 != fLastPercent / 10)) {
			printf("  %d%%\n", percent);
			fflush(stdout);
		}
		fLastPercent = percent;
	}

private:
	int		fLastPercent;
};


void
print_versions(const SystemState& state)
{
	BString next = SystemState::NextBoot();
	for (int32 i = 0; const InstalledVersion* version = state.VersionAt(i);
			i++) {
		BString flags;
		if (version->latest)
			flags << " current";
		if (version->running)
			flags << " running";
		if (!version->bootable)
			flags << " cannot-start";
		if (!next.IsEmpty() && (next == version->state
				|| (next == "latest" && version->latest))) {
			flags << " starts-next";
		}
		printf("  %-28s %s (%s)%s\n",
			version->latest ? "latest" : version->state.String(),
			version->Title().String(), version->Revision().String(),
			flags.String());
	}
}


int
command_status(Updater& updater)
{
	Release installed;
	if (updater.LoadInstalledRelease(installed) == B_OK) {
		printf("Release: %s\n  %s, %s, %s\n", installed.Id().String(),
			installed.Title().String(), installed.ShortRevision().String(),
			installed.Target().String());
	} else
		printf("Release: unknown (no %s)\n", kReleaseFile);
	printf("Feed: %s\n", updater.FeedURL().String());

	SystemState state;
	if (state.Load() != B_OK) {
		fprintf(stderr, "cannot read the package states\n");
		return 1;
	}
	if (!state.RunningLatest())
		printf("Running the older state %s\n", state.RunningState().String());
	if (state.RestartPending())
		printf("Changes wait for a restart.\n");

	TrialInfo trial;
	if (SystemState::ReadTrial(trial) == B_OK) {
		printf("Update on trial: %s, %" B_PRId32 " of %" B_PRId32
			" starts used, falls back to %s\n", trial.release.String(),
			trial.attempts, trial.allowed, trial.fallback.String());
	}
	LastBoot lastBoot = SystemState::ReadLastBoot();
	if (!lastBoot.mode.IsEmpty()) {
		printf("This start: %s %s\n", lastBoot.mode.String(),
			lastBoot.state.String());
	}
	printf("Versions:\n");
	print_versions(state);
	return 0;
}


int
command_check(Updater& updater)
{
	Feed feed;
	BString error;
	if (updater.Check(feed, error) != B_OK) {
		fprintf(stderr, "Cannot check for updates: %s\n", error.String());
		return 1;
	}
	Release installed;
	updater.LoadInstalledRelease(installed);
	int newer = 0;
	for (int32 i = 0; const FeedBuild* build = feed.BuildAt(i); i++) {
		bool isNewer = updater.IsNewer(*build);
		bool isInstalled = build->id == installed.Id();
		printf("%s %s  %s (%s)%s\n", isNewer ? "*" : " ", build->id.String(),
			Release::TitleForVersion(build->version).String(),
			Updater::FormatSize(build->size).String(),
			isInstalled ? "  installed" : "");
		if (isNewer) {
			newer++;
			BMessage commits;
			if (json::Object(build->changes, "haiku", commits)) {
				BMessage commit;
				for (int32 j = 0; json::At(commits, j, commit) && j < 20; j++) {
					printf("      %s %s\n",
						json::String(commit, "commit").String(),
						json::String(commit, "subject").String());
				}
			}
		}
	}
	printf(newer > 0 ? "%d newer release(s).\n" : "air/OS is up to date.\n",
		newer);
	return 0;
}


const FeedBuild*
choose_build(Feed& feed, const char* id)
{
	if (id == NULL)
		return feed.Latest();
	return feed.FindBuild(id);
}


void
print_plan(const UpdatePlan& plan)
{
	printf("To install (%" B_PRId32 "):\n", plan.activate.CountItems());
	for (int32 i = 0; const PlannedPackage* package = plan.activate.ItemAt(i);
			i++) {
		printf("  + %s%s\n", package->file.String(),
			package->download ? "" : " (from an old state)");
	}
	printf("To remove (%" B_PRId32 "):\n", plan.deactivate.CountStrings());
	for (int32 i = 0; i < plan.deactivate.CountStrings(); i++)
		printf("  - %s\n", plan.deactivate.StringAt(i).String());
	if (!plan.kept.IsEmpty()) {
		printf("Kept, installed by the user (%" B_PRId32 "):\n",
			plan.kept.CountStrings());
		for (int32 i = 0; i < plan.kept.CountStrings(); i++)
			printf("    %s\n", plan.kept.StringAt(i).String());
	}
	if (plan.downloadBytes > 0) {
		printf("Download: %s\n",
			Updater::FormatSize(plan.downloadBytes).String());
	}
}


int
command_update(Updater& updater, const char* id, bool apply)
{
	Feed feed;
	BString error;
	if (updater.Check(feed, error) != B_OK) {
		fprintf(stderr, "Cannot check for updates: %s\n", error.String());
		return 1;
	}
	const FeedBuild* build = choose_build(feed, id);
	if (build == NULL) {
		fprintf(stderr, "The feed has no release %s.\n", id != NULL ? id : "");
		return 1;
	}

	Release release;
	if (updater.FetchRelease(*build, release, error) != B_OK) {
		fprintf(stderr, "Cannot read the release: %s\n", error.String());
		return 1;
	}

	SystemState state;
	if (state.Load() != B_OK) {
		fprintf(stderr, "cannot read the package states\n");
		return 1;
	}
	if (apply && updater.CheckPreconditions(state, error) != B_OK) {
		fprintf(stderr, "%s\n", error.String());
		return 1;
	}

	UpdatePlan plan;
	if (updater.PlanUpdate(release, state, plan, error, &feed) != B_OK) {
		fprintf(stderr, "Cannot plan the update: %s\n", error.String());
		return 1;
	}
	printf("%s (%s)\n", release.Title().String(), release.Id().String());
	print_plan(plan);
	if (!apply)
		return 0;
	if (plan.IsEmpty()) {
		printf("Nothing to install: this system has every package of %s.\n",
			release.Id().String());
		return 0;
	}

	if (updater.Download(plan, error) != B_OK
		|| updater.Apply(plan, error) != B_OK) {
		fprintf(stderr, "The update failed: %s\nNothing was changed; it can "
			"be tried again.\n", error.String());
		return 1;
	}
	printf("Installed. %s starts with the next restart.\n",
		release.Title().String());
	return 0;
}


int
command_go_back(Updater& updater, const char* stateName)
{
	SystemState state;
	BString error;
	if (state.Load() != B_OK) {
		fprintf(stderr, "cannot read the package states\n");
		return 1;
	}
	const InstalledVersion* version = state.FindState(stateName);
	if (version == NULL) {
		fprintf(stderr, "There is no state %s.\n", stateName);
		return 1;
	}
	if (updater.CheckPreconditions(state, error) != B_OK) {
		fprintf(stderr, "%s\n", error.String());
		return 1;
	}
	UpdatePlan plan;
	if (updater.PlanGoBack(*version, state, plan, error) != B_OK) {
		fprintf(stderr, "Cannot go back: %s\n", error.String());
		return 1;
	}
	printf("Going back to %s (%s)\n", version->Title().String(),
		version->state.String());
	print_plan(plan);
	if (updater.Apply(plan, error) != B_OK) {
		fprintf(stderr, "Going back failed: %s\nNothing was changed.\n",
			error.String());
		return 1;
	}
	printf("Done. %s starts with the next restart.\n",
		version->Title().String());
	return 0;
}


void
notify(const char* title, const char* text)
{
	BApplication* application = be_app;
	if (application == NULL)
		application = new BApplication(kAppSignature);
	BNotification notification(B_INFORMATION_NOTIFICATION);
	notification.SetGroup("System Updater");
	notification.SetTitle(title);
	notification.SetContent(text);
	notification.SetOnClickApp(kAppSignature);
	notification.Send(10 * 1000000);
}


int
command_boot_check(Updater& updater, bool wait)
{
	BString report;
	status_t error = updater.BootCheck(report, wait);
	if (!report.IsEmpty())
		printf("%s\n", report.String());

	LastBoot lastBoot = SystemState::ReadLastBoot();
	if (lastBoot.mode == AIROS_BOOT_MODE_FALLBACK) {
		notify("air/OS update did not start",
			"air/OS started the version before it. Open System Updater to "
			"try the update again or to remove it.");
	} else if (lastBoot.mode == AIROS_BOOT_MODE_RECOVERY) {
		notify("air/OS recovered from an update",
			"An update was cut short, so air/OS started the version before "
			"it. Open System Updater to try again.");
	} else if (lastBoot.mode == AIROS_BOOT_MODE_NEXT
		&& !lastBoot.state.IsEmpty()) {
		notify("An older air/OS version runs",
			"This start runs an older version, as you asked. The next start "
			"is the current version again.");
	}

	// the daily look for a new release
	Settings settings;
	settings.Load();
	SystemState state;
	if (error == B_OK && settings.checkAutomatically && state.Load() == B_OK
		&& state.RunningLatest() && !state.RestartPending()) {
		// Wi-Fi may join a while after the desktop is up: try for two minutes
		Feed feed;
		BString checkError;
		status_t checkResult = B_ERROR;
		for (int attempt = 0; attempt < 7; attempt++) {
			if (attempt > 0)
				snooze(20000000);
			checkResult = updater.Check(feed, checkError);
			if (checkResult == B_OK || checkResult == B_ENTRY_NOT_FOUND
				|| checkResult == B_NOT_SUPPORTED
				|| checkResult == B_MISMATCHED_VALUES) {
				break;
			}
		}
		if (checkResult != B_OK)
			Updater::Log("could not check for a new release: %s",
				checkError.String());
		if (checkResult == B_OK && feed.Latest() != NULL
			&& updater.IsNewer(*feed.Latest())) {
			BString text;
			text.SetToFormat("%s is available. Open System Updater to see what "
				"changed and to install it.",
				Release::TitleForVersion(feed.Latest()->version).String());
			notify("air/OS update available", text);
		}
	}
	return error == B_OK ? 0 : 1;
}

}	// namespace


bool
is_command_line(int argc, char** argv)
{
	return argc > 1 && strncmp(argv[1], "--", 2) == 0;
}


int
run_command_line(int argc, char** argv)
{
	Updater updater;
	PrintingListener listener;
	updater.SetListener(&listener);

	int index = 1;
	if (index + 1 < argc && strcmp(argv[index], "--feed") == 0) {
		updater.SetFeedOverride(argv[index + 1]);
		index += 2;
	}
	if (index >= argc) {
		fputs(kUsage, stderr);
		return 1;
	}

	const char* command = argv[index];
	const char* argument = index + 1 < argc ? argv[index + 1] : NULL;

	if (strcmp(command, "--status") == 0)
		return command_status(updater);
	if (strcmp(command, "--check") == 0)
		return command_check(updater);
	if (strcmp(command, "--plan") == 0)
		return command_update(updater, argument, false);
	if (strcmp(command, "--update") == 0)
		return command_update(updater, argument, true);
	if (strcmp(command, "--restart-into") == 0 && argument != NULL) {
		SystemState state;
		state.Load();
		const InstalledVersion* version = state.FindState(argument);
		if (version == NULL || !version->bootable) {
			fprintf(stderr, "There is no state %s that can start.\n", argument);
			return 1;
		}
		status_t error = SystemState::RequestNextBoot(version->state);
		if (error != B_OK) {
			fprintf(stderr, "Cannot write the request: %s\n", strerror(error));
			return 1;
		}
		Updater::Log("asked to start %s once",
			version->latest ? "the latest state" : version->state.String());
		BString report;
		updater.SyncBootFiles(report);
		printf("%s starts once at the next restart.\n",
			version->Title().String());
		return 0;
	}
	if (strcmp(command, "--cancel-restart-into") == 0) {
		status_t error = SystemState::CancelNextBoot();
		BString report;
		updater.SyncBootFiles(report);
		return error == B_OK ? 0 : 1;
	}
	if (strcmp(command, "--go-back") == 0 && argument != NULL)
		return command_go_back(updater, argument);
	if (strcmp(command, "--retry") == 0) {
		TrialInfo trial;
		if (SystemState::ReadTrial(trial) != B_OK) {
			fprintf(stderr, "No update is on trial.\n");
			return 1;
		}
		SystemState::ResetTrialAttempts();
		Updater::Log("asked to try %s again", trial.release.String());
		BString report;
		updater.SyncBootFiles(report);
		printf("%s gets %" B_PRId32 " more starts.\n", trial.release.String(),
			trial.allowed);
		return 0;
	}
	if (strcmp(command, "--remove-failed") == 0) {
		BString error;
		if (updater.RemoveFailedUpdate(error) != B_OK) {
			fprintf(stderr, "%s\n", error.String());
			return 1;
		}
		printf("Removed. The running version stays after the next restart.\n");
		return 0;
	}
	if (strcmp(command, "--boot-check") == 0) {
		bool wait = argument == NULL || strcmp(argument, "--no-wait") != 0;
		return command_boot_check(updater, wait);
	}
	if (strcmp(command, "--confirm") == 0) {
		BString report;
		status_t error = updater.Confirm(report);
		printf("%s\n", report.String());
		return error == B_OK ? 0 : 1;
	}
	if (strcmp(command, "--prune") == 0) {
		int32 keep = argument != NULL ? atoi(argument) : kDefaultVersionsToKeep;
		BString report;
		status_t error = updater.Prune(keep < 1 ? 1 : keep, report);
		printf("%s\n", report.IsEmpty() ? "Nothing to remove."
			: report.String());
		return error == B_OK ? 0 : 1;
	}

	fputs(kUsage, stderr);
	return 1;
}
