/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Says how a 2-in-1 is folded: a laptop, or a tablet with its keyboard out
	of reach. See <accelerometer.h>.
*/


#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <OS.h>

#include <accelerometer.h>


extern const char* __progname;

static const char* const kModeNames[] = {
	"unknown", "laptop", "stand", "tent", "tablet"
};


static void
usage(int status)
{
	fprintf(status == 0 ? stdout : stderr,
		"Usage: %s [-a] [-q] [-w] [--set <mode>]\n"
		"Prints what the machine is folded into: laptop, stand, tent or "
		"tablet,\nand the angle of its hinge in degrees. In all but the "
		"first the keyboard\nis out of reach.\n\n"
		"  -a, --accelerometers  print what the accelerometers measure, too\n"
		"  -q, --quiet           print nothing, just exit with the status\n"
		"  -w, --watch           keep running, and print every change\n"
		"      --set <mode>      report this mode whatever the hinge does "
		"(to test\n"
		"                        what depends on it); \"auto\" measures "
		"again\n\n"
		"Exit status: 0 in a tablet mode (stand, tent, tablet), 1 as a "
		"laptop,\n2 when the machine has no hinge sensor, or on errors.\n",
		__progname);
	exit(status);
}


static void
print_state(const hinge_state& state, bool timestamp)
{
	if (timestamp) {
		time_t now = time(NULL);
		struct tm local;
		localtime_r(&now, &local);
		char buffer[32];
		strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
		printf("%s  ", buffer);
	}

	printf("%s", state.mode <= HINGE_MODE_TABLET
		? kModeNames[state.mode] : "?");
	if (state.angle >= 0)
		printf(", hinge at %" B_PRId32 " degrees", state.angle);
	else
		printf(", hinge angle unknown");
	if ((state.flags & HINGE_STATE_ANGLE_VALID) == 0 && state.angle >= 0)
		printf(" (not measured right now)");
	if ((state.flags & HINGE_STATE_OVERRIDDEN) != 0)
		printf(" (mode set by hand)");
	printf("\n");
	fflush(stdout);
}


static void
print_accelerometers()
{
	DIR* directory = opendir(ACCELEROMETER_DEVICE_DIRECTORY);
	if (directory == NULL) {
		printf("no accelerometers\n");
		return;
	}

	while (struct dirent* entry = readdir(directory)) {
		if (entry->d_name[0] == '.')
			continue;

		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", ACCELEROMETER_DEVICE_DIRECTORY,
			entry->d_name);
		int fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;

		accelerometer_info info;
		accelerometer_sample sample;
		if (ioctl(fd, ACCELEROMETER_GET_INFO, &info, sizeof(info)) == 0
			&& ioctl(fd, ACCELEROMETER_GET_SAMPLE, &sample, sizeof(sample))
				== 0) {
			const char* where = "";
			if (info.location == ACCELEROMETER_LOCATION_DISPLAY)
				where = " behind the screen";
			else if (info.location == ACCELEROMETER_LOCATION_BASE)
				where = " under the keyboard";
			printf("%s: %s%s: x %" B_PRId32 ", y %" B_PRId32 ", z %" B_PRId32
				" mg\n", path, info.name, where, sample.x, sample.y,
				sample.z);
		} else
			printf("%s: %s\n", path, strerror(errno));
		close(fd);
	}
	closedir(directory);
}


int
main(int argc, char** argv)
{
	bool accelerometers = false;
	bool quiet = false;
	bool watch = false;
	const char* set = NULL;

	for (int i = 1; i < argc; i++) {
		const char* argument = argv[i];
		if (strcmp(argument, "-a") == 0
			|| strcmp(argument, "--accelerometers") == 0) {
			accelerometers = true;
		} else if (strcmp(argument, "-q") == 0
			|| strcmp(argument, "--quiet") == 0) {
			quiet = true;
		} else if (strcmp(argument, "-w") == 0
			|| strcmp(argument, "--watch") == 0) {
			watch = true;
		} else if (strcmp(argument, "--set") == 0 && i + 1 < argc)
			set = argv[++i];
		else if (strcmp(argument, "-h") == 0
			|| strcmp(argument, "--help") == 0) {
			usage(0);
		} else
			usage(2);
	}

	if (accelerometers && !quiet)
		print_accelerometers();

	int fd = open(HINGE_DEVICE_PATH, O_RDONLY);
	if (fd < 0) {
		if (!quiet) {
			fprintf(stderr, "%s: this machine has no hinge sensor (%s: %s)\n",
				__progname, HINGE_DEVICE_PATH, strerror(errno));
		}
		return 2;
	}

	if (set != NULL) {
		uint32 mode = HINGE_MODE_UNKNOWN;
		if (strcmp(set, "auto") != 0) {
			for (mode = HINGE_MODE_LAPTOP; mode <= HINGE_MODE_TABLET; mode++) {
				if (strcmp(set, kModeNames[mode]) == 0)
					break;
			}
			if (mode > HINGE_MODE_TABLET)
				usage(2);
		}
		if (ioctl(fd, HINGE_SET_OVERRIDE, &mode, sizeof(mode)) != 0) {
			fprintf(stderr, "%s: setting the mode failed: %s\n", __progname,
				strerror(errno));
			return 2;
		}
		if (mode == HINGE_MODE_UNKNOWN) {
			// what it measures now
			snooze(1200000);
		}
	}

	hinge_state state;
	while (true) {
		if (ioctl(fd, HINGE_GET_STATE, &state, sizeof(state)) != 0) {
			fprintf(stderr, "%s: %s\n", __progname, strerror(errno));
			return 2;
		}
		if (!quiet)
			print_state(state, watch);
		if (!watch)
			break;

		// readable once the mode changed
		object_wait_info info;
		info.object = fd;
		info.type = B_OBJECT_TYPE_FD;
		info.events = B_EVENT_READ;
		if (wait_for_objects(&info, 1) < 0)
			return 2;
	}

	close(fd);
	return is_tablet_mode(state.mode) ? 0 : 1;
}
