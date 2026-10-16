/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	mediakey: sends a keyboard's media key (play/pause, next, previous,
	stop) to the application that should act on it. The Shortcuts
	preferences bind the consumer-page key codes to it. See
	<private/shared/MediaKeys.h> for the protocol.
*/


#include <MediaKeys.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <List.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <String.h>


static const bigtime_t kQueryTimeout = 300000;
static const bigtime_t kKeyTimeout = 2000000;

static bool sVerbose = false;


struct candidate {
	team_id	team;
	bool	playing;
	bool	active;
};


static void
usage(const char* program)
{
	fprintf(stderr, "usage: %s [-v] play-pause | play | pause | stop | next "
		"| previous\n"
		"Sends the key to the application that is playing, else to the "
		"active application,\nelse to the media application started last.\n"
		"  -v  say which applications answered and which one got the "
		"key\n", program);
	exit(1);
}


static int32
parse_key(const char* name)
{
	for (int32 key = B_MEDIA_KEY_PLAY_PAUSE; key <= B_MEDIA_KEY_PREVIOUS;
			key++) {
		if (strcmp(name, media_key_name(key)) == 0)
			return key;
	}
	if (strcmp(name, "playpause") == 0 || strcmp(name, "toggle") == 0)
		return B_MEDIA_KEY_PLAY_PAUSE;
	if (strcmp(name, "prev") == 0)
		return B_MEDIA_KEY_PREVIOUS;
	return -1;
}


int
main(int argc, char** argv)
{
	int argi = 1;
	if (argi < argc && strcmp(argv[argi], "-v") == 0) {
		sVerbose = true;
		argi++;
	}
	if (argi != argc - 1)
		usage(argv[0]);

	int32 key = parse_key(argv[argi]);
	if (key < 0) {
		fprintf(stderr, "%s: unknown key \"%s\"\n", argv[0], argv[argi]);
		usage(argv[0]);
	}

	// Replies need a looper of our own.
	BApplication app("application/x-vnd.Haiku-mediakey");

	app_info activeInfo;
	team_id activeTeam = -1;
	if (be_roster->GetActiveAppInfo(&activeInfo) == B_OK)
		activeTeam = activeInfo.team;

	BList teams;
	be_roster->GetAppList(&teams);

	BMessage query(kMsgMediaKey);
	query.AddInt32("key", key);
	query.AddBool("query", true);

	// The query goes to every application; most answer at once with
	// "not understood" or with no reply at all.
	candidate candidates[64];
	int32 candidateCount = 0;
	team_id ownTeam = app.Team();

	for (int32 i = 0; i < teams.CountItems(); i++) {
		team_id team = (team_id)(addr_t)teams.ItemAt(i);
		if (team == ownTeam)
			continue;

		BMessenger messenger(NULL, team);
		if (!messenger.IsValid())
			continue;

		BMessage reply;
		status_t status = messenger.SendMessage(&query, &reply, kQueryTimeout,
			kQueryTimeout);
		if (status != B_OK || reply.what != kMsgMediaKey)
			continue;

		candidate& found = candidates[candidateCount];
		found.team = team;
		found.playing = reply.GetBool("playing", false);
		found.active = team == activeTeam;
		if (sVerbose) {
			app_info info;
			be_roster->GetRunningAppInfo(team, &info);
			printf("%s (%" B_PRId32 "): can handle %s, playing %s%s\n",
				info.signature, team,
				reply.GetBool("can_handle", false) ? "yes" : "no",
				found.playing ? "yes" : "no", found.active ? ", active" : "");
		}
		if (!reply.GetBool("can_handle", false))
			continue;
		if (++candidateCount == (int32)B_COUNT_OF(candidates))
			break;
	}

	if (candidateCount == 0) {
		if (sVerbose)
			printf("no application takes %s\n", media_key_name(key));
		return 1;
	}

	// Choose: playing and active, else playing, else active, else the one
	// started last (the roster lists applications in the order they
	// registered).
	candidate* chosen = NULL;
	for (int32 pass = 0; pass < 3 && chosen == NULL; pass++) {
		for (int32 i = candidateCount - 1; i >= 0; i--) {
			candidate& found = candidates[i];
			bool matches = pass == 0 ? found.playing && found.active
				: pass == 1 ? found.playing : found.active;
			if (matches) {
				chosen = &found;
				break;
			}
		}
	}
	if (chosen == NULL)
		chosen = &candidates[candidateCount - 1];

	BMessage message(kMsgMediaKey);
	message.AddInt32("key", key);
	BMessage reply;
	status_t status = BMessenger(NULL, chosen->team).SendMessage(&message,
		&reply, kKeyTimeout, kKeyTimeout);
	bool handled = status == B_OK && reply.what == kMsgMediaKey
		&& reply.GetBool("handled", false);
	if (sVerbose) {
		app_info info;
		be_roster->GetRunningAppInfo(chosen->team, &info);
		printf("%s -> %s (%" B_PRId32 "): %s\n", media_key_name(key),
			info.signature, chosen->team, status != B_OK ? strerror(status)
				: handled ? "handled" : "not handled");
	}
	return handled ? 0 : 1;
}
