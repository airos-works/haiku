/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef PVR_BENCH_AREAS_H
#define PVR_BENCH_AREAS_H

// Memory areas for pvr_glbench and pvr_vkbench: the process's own (its
// "powervr buffer" areas are the kernel driver's MAP_BO clones of buffer
// objects) and the kernel's "powervr ..." areas (buffer objects, pages and
// vmaps of the driver; listing the kernel team needs root, as with
// listarea). Each snapshot also counts areas by name, so the end of a run
// can list the names whose count changed.
// On Linux (build.sh shim) the process's mappings from /proc/self/maps stand
// in, by path; there is no kernel side.


#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __HAIKU__
#	include <OS.h>
#	define AREA_NAME_LENGTH	B_OS_NAME_LENGTH
#else
#	define AREA_NAME_LENGTH	64
#endif

#define AREA_MAX_NAMES		256


struct area_name {
	char		name[AREA_NAME_LENGTH];
	int			count;
};

struct area_snapshot {
	bool		valid;
	int			count;
	int			powervr;	// named "powervr..."
	uint64_t	bytes;
	int			names;
	struct area_name byName[AREA_MAX_NAMES];
};


static inline void
area_add(struct area_snapshot* snapshot, const char* name, uint64_t size)
{
	snapshot->count++;
	snapshot->bytes += size;
	if (strncmp(name, "powervr", 7) == 0)
		snapshot->powervr++;
	for (int i = 0; i < snapshot->names; i++) {
		if (strcmp(snapshot->byName[i].name, name) == 0) {
			snapshot->byName[i].count++;
			return;
		}
	}
	if (snapshot->names == AREA_MAX_NAMES)
		return;
	struct area_name* entry = &snapshot->byName[snapshot->names++];
	size_t length = strnlen(name, AREA_NAME_LENGTH - 1);
	memcpy(entry->name, name, length);
	entry->name[length] = '\0';
	entry->count = 1;
}


// The process's own areas.
static inline void
area_snapshot_own(struct area_snapshot* snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
#ifdef __HAIKU__
	area_info info;
	ssize_t cookie = 0;
	while (get_next_area_info(B_CURRENT_TEAM, &cookie, &info) == B_OK)
		area_add(snapshot, info.name, info.size);
	snapshot->valid = true;
#else
	FILE* maps = fopen("/proc/self/maps", "r");
	if (maps == NULL)
		return;
	char line[512];
	while (fgets(line, sizeof(line), maps) != NULL) {
		unsigned long start, end;
		int pathOffset = 0;
		if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %n", &start, &end,
				&pathOffset) < 2) {
			continue;
		}
		char* path = line + pathOffset;
		path[strcspn(path, "\n")] = '\0';
		area_add(snapshot, path[0] != '\0' ? path : "[anonymous]",
			end - start);
	}
	fclose(maps);
	snapshot->valid = true;
#endif
}


// The kernel's areas named "powervr...". Not valid when not allowed (not
// root) or not on Haiku.
static inline void
area_snapshot_kernel(struct area_snapshot* snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
#ifdef __HAIKU__
	area_info info;
	ssize_t cookie = 0;
	status_t status;
	while ((status = get_next_area_info(B_SYSTEM_TEAM, &cookie, &info))
			== B_OK) {
		if (strncmp(info.name, "powervr", 7) == 0)
			area_add(snapshot, info.name, info.size);
	}
	snapshot->valid = status != B_NOT_ALLOWED;
#endif
}


// " areas N (powervr P, M MiB)", or " areas n/a"
static inline void
area_print(FILE* out, const char* label, const struct area_snapshot* snapshot)
{
	if (!snapshot->valid) {
		fprintf(out, "; %s n/a", label);
		return;
	}
	fprintf(out, "; %s %d (powervr %d, %.1f MiB)", label, snapshot->count,
		snapshot->powervr, snapshot->bytes / 1048576.0);
}


static inline int
area_count_of(const struct area_snapshot* snapshot, const char* name)
{
	for (int i = 0; i < snapshot->names; i++) {
		if (strcmp(snapshot->byName[i].name, name) == 0)
			return snapshot->byName[i].count;
	}
	return 0;
}


// Every name whose count differs between the two snapshots.
static inline void
area_print_changes(FILE* out, const char* label,
	const struct area_snapshot* before, const struct area_snapshot* after)
{
	if (!before->valid || !after->valid)
		return;
	fprintf(out, "%s: %d -> %d areas, %.1f -> %.1f MiB\n", label,
		before->count, after->count, before->bytes / 1048576.0,
		after->bytes / 1048576.0);
	int changed = 0;
	for (int i = 0; i < after->names; i++) {
		const struct area_name* entry = &after->byName[i];
		int was = area_count_of(before, entry->name);
		if (was != entry->count) {
			fprintf(out, "  %-40s %5d -> %5d\n", entry->name, was,
				entry->count);
			changed++;
		}
	}
	for (int i = 0; i < before->names; i++) {
		const struct area_name* entry = &before->byName[i];
		if (area_count_of(after, entry->name) == 0) {
			fprintf(out, "  %-40s %5d ->     0\n", entry->name,
				entry->count);
			changed++;
		}
	}
	if (changed == 0)
		fprintf(out, "  no name changed its count\n");
}


#endif	// PVR_BENCH_AREAS_H
