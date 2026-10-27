/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab tool: how busy the processors are over some seconds, in all and for
	the teams whose name contains one of the given words.

	cpu_usage <seconds> [name ...] */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>


static const int kMaxTeams = 64;


struct TeamSample {
	team_id		team;
	char		name[B_OS_NAME_LENGTH];
	bigtime_t	time;
};


static int
sample_teams(int count, char** names, TeamSample* samples)
{
	int found = 0;
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK && found < kMaxTeams) {
		bool wanted = false;
		for (int i = 0; i < count; i++) {
			if (strstr(info.args, names[i]) != NULL)
				wanted = true;
		}
		if (!wanted)
			continue;
		team_usage_info usage;
		if (get_team_usage_info(info.team, B_TEAM_USAGE_SELF, &usage) != B_OK)
			continue;
		samples[found].team = info.team;
		strlcpy(samples[found].name, info.args, sizeof(samples[found].name));
		samples[found].time = usage.user_time + usage.kernel_time;
		found++;
	}
	return found;
}


static bigtime_t
busy_time(uint32 count, cpu_info* info)
{
	if (get_cpu_info(0, count, info) != B_OK)
		return 0;
	bigtime_t total = 0;
	for (uint32 i = 0; i < count; i++)
		total += info[i].active_time;
	return total;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <seconds> [name ...]\n", argv[0]);
		return 1;
	}
	double seconds = atof(argv[1]);
	system_info system;
	get_system_info(&system);
	uint32 cpus = system.cpu_count;
	cpu_info* info = new cpu_info[cpus];

	TeamSample before[kMaxTeams];
	TeamSample after[kMaxTeams];
	int teamsBefore = sample_teams(argc - 2, argv + 2, before);
	bigtime_t start = system_time();
	bigtime_t busyStart = busy_time(cpus, info);
	snooze((bigtime_t)(seconds * 1000000));
	bigtime_t busy = busy_time(cpus, info) - busyStart;
	bigtime_t elapsed = system_time() - start;
	int teamsAfter = sample_teams(argc - 2, argv + 2, after);

	printf("all: %.1f %% of %" B_PRIu32 " cores (%.2f cores busy) over %.1f s\n",
		100.0 * busy / elapsed / cpus, cpus, (double)busy / elapsed,
		elapsed / 1000000.0);
	for (int i = 0; i < teamsAfter; i++) {
		for (int j = 0; j < teamsBefore; j++) {
			if (before[j].team != after[i].team)
				continue;
			bigtime_t used = after[i].time - before[j].time;
			printf("%s (%" B_PRId32 "): %.1f %% of one core\n", after[i].name,
				after[i].team, 100.0 * used / elapsed);
		}
	}
	delete[] info;
	return 0;
}
