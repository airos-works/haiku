/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	media_dead_team_test: what a program that dies while it plays or records
	leaves behind (#164). Needs running media services and a sound card with
	an input.

	For each of six children (a BSoundPlayer, a BMediaRecorder on the default
	audio input and one on the mixer's monitor output, each with and without
	a BApplication) the test starts the child, waits until sound flows, kills
	it with SIGKILL and checks that
	- the connection is gone from the node at the other end (the system
	  mixer's input; the capture output of the sound card or the mixer's
	  monitor, which must be free again),
	- the sound card stops when nothing plays or records any more (its
	  "multi_audio audio output" thread ends),
	- afterwards a recorder of the test itself still gets buffers and a
	  player of the test itself is still asked for sound.
	A last child records and is stopped with SIGSTOP (as behind a crash
	alert): the sound card's thread, which plays as well, must not wait for
	the buffers that child holds.

	media_dead_team_test hang       only the last case
	media_dead_team_test list       prints the nodes and their connections
	media_dead_team_test play|record|monitor [noapp] <port>   the children
*/


#include <Application.h>
#include <MediaAddOn.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <OS.h>
#include <Roster.h>
#include <SoundPlayer.h>

#include <image.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>


extern char** environ;

static const bigtime_t kChildDeadline = 20 * 1000000LL;
static const bigtime_t kCleanupDeadline = 10 * 1000000LL;
static const bigtime_t kBufferDeadline = 5 * 1000000LL;

static const int32 kReady = 'redy';
static const int32 kFailed = 'fail';

static const char* const kCardThreadName = "multi_audio audio output";
static const char* const kAddOnServerSignature
	= "application/x-vnd.Be.addon-host";

static int32 sPlayed;
static int32 sRecorded;
static double sPhase;
static int sFailures;


static void
play_tone(void* cookie, void* data, size_t size,
	const media_raw_audio_format& format)
{
	memset(data, 0, size);
	if (format.format == media_raw_audio_format::B_AUDIO_FLOAT) {
		float* samples = (float*)data;
		size_t frames = size / sizeof(float) / format.channel_count;
		for (size_t i = 0; i < frames; i++) {
			float value = 0.25 * sin(sPhase);
			sPhase += 2 * M_PI * 440 / format.frame_rate;
			for (uint32 c = 0; c < format.channel_count; c++)
				*samples++ = value;
		}
	}
	atomic_add(&sPlayed, 1);
}


static void
record(void* cookie, bigtime_t timestamp, void* data, size_t size,
	const media_format& format)
{
	atomic_add(&sRecorded, 1);
}


static bool
wait_for(int32* counter, int32 above, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while (atomic_get(counter) <= above) {
		if (system_time() > end)
			return false;
		snooze(20000);
	}
	return true;
}


static void
check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	fflush(stdout);
	if (!ok)
		sFailures++;
}


// #pragma mark - children


enum child_kind {
	kPlay,
	kRecord,
	kMonitor
};

static const char* const kKindNames[] = {"play", "record", "monitor"};
static const char* const kKindTitles[] = {"player",
	"recorder on the sound card", "recorder on the mixer's monitor"};


static int
child(child_kind kind, bool withApplication, port_id port)
{
	if (withApplication)
		new BApplication("application/x-vnd.test-media-dead-team-child");

	BSoundPlayer* player = NULL;
	BMediaRecorder* recorder = NULL;
	status_t status;
	if (kind == kPlay) {
		player = new BSoundPlayer("dead team test", play_tone);
		status = player->InitCheck();
		if (status == B_OK) {
			player->SetHasData(true);
			status = player->Start();
		}
		if (status == B_OK && !wait_for(&sPlayed, 2, kChildDeadline))
			status = B_TIMED_OUT;
	} else {
		recorder = new BMediaRecorder("dead team test", B_MEDIA_RAW_AUDIO);
		status = recorder->InitCheck();
		if (status == B_OK)
			status = recorder->SetHooks(record, NULL, NULL);
		media_node node;
		if (status == B_OK) {
			status = kind == kRecord
				? BMediaRoster::Roster()->GetAudioInput(&node)
				: BMediaRoster::Roster()->GetAudioMixer(&node);
		}
		if (status == B_OK) {
			media_format format;
			format.type = B_MEDIA_RAW_AUDIO;
			format.u.raw_audio = media_raw_audio_format::wildcard;
			recorder->SetAcceptedFormat(format);
			status = recorder->Connect(node, NULL, &format);
		}
		if (status == B_OK)
			status = recorder->Start();
		// the monitor has buffers only while something plays
		if (status == B_OK && kind == kRecord
			&& !wait_for(&sRecorded, 2, kChildDeadline)) {
			status = B_TIMED_OUT;
		}
	}

	write_port(port, status == B_OK ? kReady : kFailed, &status,
		sizeof(status));
	if (status != B_OK)
		return 1;

	// The parent kills us in the middle of it.
	snooze(60 * 1000000LL);
	return 1;
}


// #pragma mark - the test


static team_id
port_team(port_id port)
{
	port_info info;
	if (get_port_info(port, &info) != B_OK)
		return -1;
	return info.team;
}


static bool
card_thread_runs()
{
	team_id team = be_roster->TeamFor(kAddOnServerSignature);
	if (team < 0)
		return false;

	int32 cookie = 0;
	thread_info info;
	while (get_next_thread_info(team, &cookie, &info) == B_OK) {
		if (strcmp(info.name, kCardThreadName) == 0)
			return true;
	}
	return false;
}


/*!	How often, of 200 looks in two seconds, the sound card's thread waits
	for a buffer of a buffer group; -1 when it does not run.
*/
static int32
count_card_buffer_waits()
{
	team_id team = be_roster->TeamFor(kAddOnServerSignature);
	thread_id thread = -1;
	int32 cookie = 0;
	thread_info info;
	while (team >= 0 && get_next_thread_info(team, &cookie, &info) == B_OK) {
		if (strcmp(info.name, kCardThreadName) == 0)
			thread = info.thread;
	}
	if (thread < 0)
		return -1;

	int32 waits = 0;
	for (int32 i = 0; i < 200; i++) {
		sem_info semInfo;
		if (get_thread_info(thread, &info) == B_OK
			&& info.state == B_THREAD_WAITING
			&& get_sem_info(info.sem, &semInfo) == B_OK
			&& strcmp(semInfo.name, "buffer reclaim sem") == 0) {
			waits++;
		}
		snooze(10000);
	}
	return waits;
}


static bool
wait_for_card(bool running, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while (card_thread_runs() != running) {
		if (system_time() > end)
			return false;
		snooze(100000);
	}
	return true;
}


/*!	The mixer input that the given team's node is connected to. */
static bool
find_mixer_input(const media_node& mixer, team_id team, port_id sourcePort,
	media_input* _input)
{
	media_input inputs[64];
	int32 count = 0;
	if (BMediaRoster::Roster()->GetConnectedInputsFor(mixer, inputs, 64,
			&count) != B_OK)
		return false;
	for (int32 i = 0; i < count; i++) {
		if ((team >= 0 && port_team(inputs[i].source.port) == team)
			|| (sourcePort >= 0 && inputs[i].source.port == sourcePort)) {
			*_input = inputs[i];
			return true;
		}
	}
	return false;
}


/*!	The output of the sound card (or of the mixer: its monitor) that the
	given team's node is connected to.
*/
static bool
find_output(const media_node& card, team_id team, port_id destinationPort,
	media_output* _output)
{
	media_output outputs[64];
	int32 count = 0;
	if (BMediaRoster::Roster()->GetConnectedOutputsFor(card, outputs, 64,
			&count) != B_OK)
		return false;
	for (int32 i = 0; i < count; i++) {
		if ((team >= 0 && port_team(outputs[i].destination.port) == team)
			|| (destinationPort >= 0
				&& outputs[i].destination.port == destinationPort)) {
			*_output = outputs[i];
			return true;
		}
	}
	return false;
}


static int32
count_free_outputs(const media_node& card)
{
	media_output outputs[64];
	int32 count = 0;
	if (BMediaRoster::Roster()->GetFreeOutputsFor(card, outputs, 64, &count,
			B_MEDIA_RAW_AUDIO) != B_OK)
		return -1;
	return count;
}


static int32
count_team_nodes(team_id team)
{
	live_node_info nodes[256];
	int32 count = 256;
	if (BMediaRoster::Roster()->GetLiveNodes(nodes, &count) != B_OK)
		return -1;
	int32 found = 0;
	for (int32 i = 0; i < count; i++) {
		// a dead team's ports are gone: such nodes have no team any more
		if (port_team(nodes[i].node.port) == team)
			found++;
	}
	return found;
}


static team_id
start_child(const char* path, child_kind kind, bool withApplication,
	port_id port)
{
	char portString[32];
	snprintf(portString, sizeof(portString), "%" B_PRId32, port);
	const char* args[6];
	int argCount = 0;
	args[argCount++] = path;
	args[argCount++] = kKindNames[kind];
	if (!withApplication)
		args[argCount++] = "noapp";
	args[argCount++] = portString;
	args[argCount] = NULL;

	thread_id thread = load_image(argCount, args, (const char**)environ);
	if (thread < 0)
		return thread;
	resume_thread(thread);

	int32 code;
	status_t childStatus = B_ERROR;
	ssize_t size = read_port_etc(port, &code, &childStatus,
		sizeof(childStatus), B_RELATIVE_TIMEOUT, kChildDeadline);
	if (size < 0 || code != kReady) {
		printf("the child did not get ready: %s\n",
			strerror(size < 0 ? (status_t)size : childStatus));
		kill(thread, SIGKILL);
		return size < 0 ? (status_t)size : B_ERROR;
	}
	return thread;
}


static bool
recorder_gets_buffers(const media_node& node)
{
	BMediaRecorder recorder("dead team test check", B_MEDIA_RAW_AUDIO);
	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio = media_raw_audio_format::wildcard;
	recorder.SetAcceptedFormat(format);
	recorder.SetHooks(record, NULL, NULL);
	status_t status = recorder.Connect(node, NULL, &format);
	if (status != B_OK) {
		printf("     a second recorder cannot connect: %s\n",
			strerror(status));
		return false;
	}
	status = recorder.Start();
	int32 before = atomic_get(&sRecorded);
	bool got = status == B_OK
		&& wait_for(&sRecorded, before + 2, kBufferDeadline);
	if (!got) {
		printf("     a second recorder gets no buffers (Start: %s)\n",
			strerror(status));
	}
	recorder.Stop();
	recorder.Disconnect();
	return got;
}


static bool
player_is_asked()
{
	BSoundPlayer player("dead team test check", play_tone);
	if (player.InitCheck() != B_OK)
		return false;
	player.SetHasData(true);
	int32 before = atomic_get(&sPlayed);
	player.Start();
	bool got = wait_for(&sPlayed, before + 2, kBufferDeadline);
	player.Stop();
	return got;
}


/*!	Whether the sound card is at rest, as it has to be before each case.
	After a recording it runs on until the next sound has ended (the mixer
	stops it then): a sound is played if it runs.
*/
static bool
card_is_stopped()
{
	if (wait_for_card(false, 1000000))
		return true;
	player_is_asked();
	return wait_for_card(false, kCleanupDeadline);
}


static void
run_case(const char* path, child_kind kind, bool withApplication,
	const media_node& mixer, const media_node& card, port_id port)
{
	printf("--- a %s %s a BApplication is killed\n", kKindTitles[kind],
		withApplication ? "with" : "without");

	// Let what the previous case played or recorded end first.
	check(card_is_stopped(),
		"the sound card is stopped before the child starts");
	const media_node& producer = kind == kMonitor ? mixer : card;
	int32 freeBefore = count_free_outputs(producer);

	team_id team = start_child(path, kind, withApplication, port);
	check(team >= 0, "the child plays or records");
	if (team < 0)
		return;

	media_input input;
	media_output output;
	port_id peerPort = -1;
	bool connected;
	if (kind == kPlay) {
		connected = find_mixer_input(mixer, team, -1, &input);
		peerPort = input.source.port;
		check(connected, "the mixer has an input from the child");
		check(card_thread_runs(), "the sound card runs");
	} else {
		connected = find_output(producer, team, -1, &output);
		peerPort = output.destination.port;
		check(connected, "an output is connected to the child");
		check(count_free_outputs(producer) == freeBefore - 1,
			"the output is not free while the child records");
	}
	check(count_team_nodes(team) > 0, "the child's nodes are registered");

	kill(team, SIGKILL);
	status_t exitValue;
	wait_for_thread(team, &exitValue);

	// The media_server learns of the death from the kernel or the registrar:
	// give it a moment.
	bigtime_t killed = system_time();
	bool gone = false;
	while (connected && !gone && system_time() < killed + kCleanupDeadline) {
		if (kind == kPlay)
			gone = !find_mixer_input(mixer, -1, peerPort, &input);
		else
			gone = !find_output(producer, -1, peerPort, &output);
		if (!gone)
			snooze(50000);
	}
	printf("     (%.2f s after the kill)\n",
		(system_time() - killed) / 1000000.0);
	if (kind == kPlay)
		check(gone, "the mixer's input from the dead child is gone");
	else {
		check(gone, "the output to the dead child is gone");
		check(count_free_outputs(producer) == freeBefore,
			"the output is free again");
	}

	if (kind == kPlay) {
		// nothing else plays or records
		check(wait_for_card(false, kCleanupDeadline),
			"the sound card stops");
	}

	check(recorder_gets_buffers(card),
		"a second recorder on the sound card gets buffers");
	check(player_is_asked(), "a second player is asked for sound");

	// After a recording the sound card runs on until the next sound has
	// ended (the mixer stops it): the player above was that.
	check(wait_for_card(false, kCleanupDeadline),
		"the sound card stops when nothing plays or records");
}


static void
run_hang_case(const char* path, const media_node& card, port_id port)
{
	printf("--- a recorder on the sound card hangs\n");

	check(card_is_stopped(),
		"the sound card is stopped before the child starts");

	team_id team = start_child(path, kRecord, true, port);
	check(team >= 0, "the child records");
	if (team < 0)
		return;

	int32 before = count_card_buffer_waits();
	kill(team, SIGSTOP);
	snooze(500000);
		// the few buffers of the connection are all on their way to it now
	int32 during = count_card_buffer_waits();
	printf("     of 200 looks each, the sound card's thread waited for a "
		"buffer %" B_PRId32 " times while the child ran, %" B_PRId32
		" times while it hung\n", before, during);
	check(before >= 0 && during >= 0, "the sound card runs");
	check(during <= 2,
		"the sound card's thread does not wait for the hanging child");

	media_output output;
	bool connected = find_output(card, team, -1, &output);
	kill(team, SIGKILL);
	status_t exitValue;
	wait_for_thread(team, &exitValue);

	bigtime_t killed = system_time();
	bool gone = false;
	while (connected && !gone && system_time() < killed + kCleanupDeadline) {
		gone = !find_output(card, -1, output.destination.port, &output);
		if (!gone)
			snooze(50000);
	}
	check(gone, "the output to the dead child is gone");
	check(recorder_gets_buffers(card),
		"a second recorder on the sound card gets buffers");
	check(player_is_asked(), "a second player is asked for sound");
	check(wait_for_card(false, kCleanupDeadline),
		"the sound card stops when nothing plays or records");
}


static void
list()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	live_node_info nodes[256];
	int32 count = 256;
	if (roster->GetLiveNodes(nodes, &count) != B_OK)
		return;
	for (int32 i = 0; i < count; i++) {
		const media_node& node = nodes[i].node;
		printf("node %" B_PRId32 " \"%s\" port %" B_PRId32 " team %" B_PRId32
			" kind %#" B_PRIx32 "\n", node.node, nodes[i].name, node.port,
			port_team(node.port), node.kind);
		if (port_team(node.port) < 0)
			continue;
		media_input inputs[64];
		int32 inputCount = 0;
		if ((node.kind & B_BUFFER_CONSUMER) != 0
			&& roster->GetConnectedInputsFor(node, inputs, 64, &inputCount)
				== B_OK) {
			for (int32 k = 0; k < inputCount; k++) {
				printf("  input \"%s\" %" B_PRId32 "/%" B_PRId32 " from %"
					B_PRId32 "/%" B_PRId32 " (team %" B_PRId32 ")\n",
					inputs[k].name, inputs[k].destination.port,
					inputs[k].destination.id, inputs[k].source.port,
					inputs[k].source.id, port_team(inputs[k].source.port));
			}
		}
		media_output outputs[64];
		int32 outputCount = 0;
		if ((node.kind & B_BUFFER_PRODUCER) != 0
			&& roster->GetConnectedOutputsFor(node, outputs, 64, &outputCount)
				== B_OK) {
			for (int32 k = 0; k < outputCount; k++) {
				printf("  output \"%s\" %" B_PRId32 "/%" B_PRId32 " to %"
					B_PRId32 "/%" B_PRId32 " (team %" B_PRId32 ")\n",
					outputs[k].name, outputs[k].source.port,
					outputs[k].source.id, outputs[k].destination.port,
					outputs[k].destination.id,
					port_team(outputs[k].destination.port));
			}
		}
	}
	printf("sound card thread: %s\n", card_thread_runs() ? "runs" : "none");
}


int
main(int argc, char** argv)
{
	for (int32 kind = kPlay; argc >= 3 && kind <= kMonitor; kind++) {
		if (strcmp(argv[1], kKindNames[kind]) == 0) {
			return child((child_kind)kind, strcmp(argv[2], "noapp") != 0,
				atoi(argv[argc - 1]));
		}
	}

	BApplication app("application/x-vnd.test-media-dead-team");
	BMediaRoster* roster = BMediaRoster::Roster();
	if (roster == NULL) {
		printf("no media roster\n");
		return 1;
	}

	if (argc >= 2 && strcmp(argv[1], "list") == 0) {
		list();
		return 0;
	}

	media_node mixer;
	media_node card;
	status_t status = roster->GetAudioMixer(&mixer);
	if (status == B_OK)
		status = roster->GetAudioInput(&card);
	if (status != B_OK) {
		printf("no audio mixer or no audio input (%s): nothing to test\n",
			strerror(status));
		return 1;
	}

	port_id port = create_port(4, "dead team test");
	const char* path = argv[0];
	bool onlyHang = argc >= 2 && strcmp(argv[1], "hang") == 0;
	for (int32 withApplication = 1; !onlyHang && withApplication >= 0;
			withApplication--) {
		for (int32 kind = kPlay; kind <= kMonitor; kind++) {
			run_case(path, (child_kind)kind, withApplication != 0, mixer,
				card, port);
		}
	}

	run_hang_case(path, card, port);

	roster->ReleaseNode(mixer);
	roster->ReleaseNode(card);
	delete_port(port);

	printf("%s\n", sFailures == 0 ? "PASSED" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
