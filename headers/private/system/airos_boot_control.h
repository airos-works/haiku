/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYSTEM_AIROS_BOOT_CONTROL_H
#define _SYSTEM_AIROS_BOOT_CONTROL_H


#include <string.h>

#include <StorageDefs.h>
#include <SupportDefs.h>


/*!	Which package state to start, as System Updater asked for it.

	System Updater (src/apps/systemupdater) keeps these files in
	system/packages/administrative/airos/ of the boot volume:

	next-boot		a state to start once: its directory name ("state_...")
					or "latest". The kernel renames it to next-boot.done
					when that start has reached it.
	trial			an installed update that has not been confirmed yet:
					"key value" lines: "fallback <state>" (the state to
					return to), "attempts <n>" (starts allowed, default 2),
					"release <id>".
	trial-attempts	one line per start of the update on trial, appended by
					the kernel.
	last-boot		"<mode> <state>": how the loader chose the running
					state, written by the kernel; removed by an ordinary
					start of the latest state.

	The boot loader (and, where the loader cannot read the boot volume, the
	kernel) decides with airos_choose_boot_state(). The result goes to the
	kernel as the boot volume's "airos boot" field, so that it can record the
	start. See docs/airos/SYSTEM-UPDATER.md.
*/


#define AIROS_BOOT_CONTROL_DIRECTORY	"administrative/airos"
	// relative to the system packages directory
#define AIROS_BOOT_NEXT_FILE			"next-boot"
#define AIROS_BOOT_NEXT_DONE_FILE		"next-boot.done"
#define AIROS_BOOT_TRIAL_FILE			"trial"
#define AIROS_BOOT_TRIAL_ATTEMPTS_FILE	"trial-attempts"
#define AIROS_BOOT_LAST_FILE			"last-boot"

#define AIROS_BOOT_DEFAULT_ATTEMPTS		2

// values of the boot volume's "airos boot" field
#define AIROS_BOOT_MODE_NEXT			"next"
#define AIROS_BOOT_MODE_TRIAL			"trial"
#define AIROS_BOOT_MODE_FALLBACK		"fallback"
#define AIROS_BOOT_MODE_RECOVERY		"recovery"
#define AIROS_BOOT_MODE_MANUAL			"manual"


enum airos_boot_mode {
	AIROS_BOOT_DEFAULT = 0,
		// the latest state, nothing to record
	AIROS_BOOT_NEXT,
		// a state requested for one start
	AIROS_BOOT_TRIAL,
		// the latest state, an update on trial: count the start
	AIROS_BOOT_FALLBACK,
		// the update on trial used up its starts: its fallback state
	AIROS_BOOT_RECOVERY
		// the latest state cannot start (a package transaction was cut
		// short): the newest old state
};


static inline const char*
airos_boot_mode_name(airos_boot_mode mode)
{
	switch (mode) {
		case AIROS_BOOT_NEXT:
			return AIROS_BOOT_MODE_NEXT;
		case AIROS_BOOT_TRIAL:
			return AIROS_BOOT_MODE_TRIAL;
		case AIROS_BOOT_FALLBACK:
			return AIROS_BOOT_MODE_FALLBACK;
		case AIROS_BOOT_RECOVERY:
			return AIROS_BOOT_MODE_RECOVERY;
		default:
			return NULL;
	}
}


/*!	Copies the first word of \a text (up to \a length bytes) into \a word.
	Returns the position behind the word.
*/
static inline const char*
airos_boot_copy_word(const char* text, const char* end, char* word,
	size_t wordSize)
{
	while (text < end && (*text == ' ' || *text == '\t'))
		text++;
	size_t length = 0;
	while (text < end && *text != ' ' && *text != '\t' && *text != '\n'
		&& *text != '\r') {
		if (length + 1 < wordSize)
			word[length++] = *text;
		text++;
	}
	if (wordSize > 0)
		word[length] = '\0';
	return text;
}


/*!	Looks up \a key in "key value" lines and copies its value into \a value.
*/
static inline bool
airos_boot_find_value(const char* text, size_t length, const char* key,
	char* value, size_t valueSize)
{
	const char* end = text + length;
	while (text < end) {
		char word[64];
		const char* rest = airos_boot_copy_word(text, end, word, sizeof(word));
		const char* lineEnd = rest;
		while (lineEnd < end && *lineEnd != '\n')
			lineEnd++;
		if (strcmp(word, key) == 0) {
			airos_boot_copy_word(rest, lineEnd, value, valueSize);
			return value[0] != '\0';
		}
		text = lineEnd + 1;
	}
	return false;
}


/*!	Decides which state to start. \a Reader provides
	- ssize_t ReadFile(const char* name, char* buffer, size_t size): reads a
	  file of the boot control directory, < 0 if there is none;
	- bool StateExists(const char* stateName): whether an old state of that
	  name can be started;
	- bool LatestBootable(): whether the latest state can be started;
	- const char* NewestOldState(): the newest old state that can be started,
	  or NULL.
	On return \a state holds the chosen old state's name, or is empty for the
	latest state.
*/
template<typename Reader>
static airos_boot_mode
airos_choose_boot_state(Reader& reader, char* state, size_t stateSize)
{
	char buffer[512];
	state[0] = '\0';

	// a start requested once
	ssize_t length = reader.ReadFile(AIROS_BOOT_NEXT_FILE, buffer,
		sizeof(buffer) - 1);
	if (length > 0) {
		char requested[B_FILE_NAME_LENGTH];
		airos_boot_copy_word(buffer, buffer + length, requested,
			sizeof(requested));
		if (strcmp(requested, "latest") == 0 || requested[0] == '\0') {
			if (reader.LatestBootable())
				return AIROS_BOOT_NEXT;
		} else if (reader.StateExists(requested)) {
			strlcpy(state, requested, stateSize);
			return AIROS_BOOT_NEXT;
		}
	}

	bool latestBootable = reader.LatestBootable();

	// an update on trial
	length = reader.ReadFile(AIROS_BOOT_TRIAL_FILE, buffer, sizeof(buffer) - 1);
	if (length >= 0) {
		buffer[length] = '\0';
		char value[B_FILE_NAME_LENGTH];
		int allowed = AIROS_BOOT_DEFAULT_ATTEMPTS;
		if (airos_boot_find_value(buffer, length, "attempts", value,
				sizeof(value))) {
			allowed = 0;
			for (const char* digit = value; *digit >= '0' && *digit <= '9';
					digit++) {
				allowed = allowed * 10 + (*digit - '0');
			}
			if (allowed < 1)
				allowed = 1;
		}

		char fallback[B_FILE_NAME_LENGTH];
		if (!airos_boot_find_value(buffer, length, "fallback", fallback,
				sizeof(fallback))) {
			fallback[0] = '\0';
		}
		if (strcmp(fallback, "newest") == 0 || fallback[0] == '\0') {
			const char* newest = reader.NewestOldState();
			strlcpy(fallback, newest != NULL ? newest : "", sizeof(fallback));
		}

		int attempts = 0;
		char attemptsBuffer[256];
		ssize_t attemptsLength = reader.ReadFile(
			AIROS_BOOT_TRIAL_ATTEMPTS_FILE, attemptsBuffer,
			sizeof(attemptsBuffer));
		for (ssize_t i = 0; i < attemptsLength; i++) {
			if (attemptsBuffer[i] == '\n')
				attempts++;
		}
		if (attemptsLength == (ssize_t)sizeof(attemptsBuffer))
			attempts = allowed;

		bool fallbackExists = fallback[0] != '\0'
			&& reader.StateExists(fallback);
		if ((attempts >= allowed || !latestBootable) && fallbackExists) {
			strlcpy(state, fallback, stateSize);
			return AIROS_BOOT_FALLBACK;
		}
		if (latestBootable)
			return AIROS_BOOT_TRIAL;
	}

	if (!latestBootable) {
		const char* newest = reader.NewestOldState();
		if (newest != NULL) {
			strlcpy(state, newest, stateSize);
			return AIROS_BOOT_RECOVERY;
		}
	}

	return AIROS_BOOT_DEFAULT;
}


#endif	// _SYSTEM_AIROS_BOOT_CONTROL_H
