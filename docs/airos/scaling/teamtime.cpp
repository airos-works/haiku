/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Prints the CPU time used so far by the teams whose name contains the
 * argument (default app_server), in milliseconds: teamtime [name]
 */


#include <OS.h>

#include <stdio.h>
#include <string.h>


int
main(int argc, char** argv)
{
	const char* name = argc > 1 ? argv[1] : "app_server";
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		if (strstr(info.args, name) == NULL)
			continue;
		team_usage_info usage;
		if (get_team_usage_info(info.team, B_TEAM_USAGE_SELF, &usage) != B_OK)
			continue;
		printf("%" B_PRId32 " %.1f %s\n", info.team,
			(usage.user_time + usage.kernel_time) / 1000.0, info.args);
	}
	return 0;
}
