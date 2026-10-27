/*
 * airos_mcp - small helpers shared by the tools.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#ifndef AIROS_MCP_UTIL_H
#define AIROS_MCP_UTIL_H

#include <stdint.h>
#include <sys/types.h>
#include <string>
#include <vector>

#include <OS.h>


std::string Base64Encode(const void* data, size_t length);
bool Base64Decode(const std::string& text, std::string& out);

bool ReadFileToString(const std::string& path, std::string& out,
	size_t maxBytes = 0, off_t offset = 0);
bool WriteStringToFile(const std::string& path, const std::string& data,
	mode_t mode = 0644);

std::vector<std::string> SplitLines(const std::string& text);
std::string TrimString(const std::string& s);
bool StartsWith(const std::string& s, const char* prefix);
bool EndsWith(const std::string& s, const char* suffix);
std::string TailBytes(const std::string& s, size_t maxBytes);
std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string StrError(status_t error);
std::string ThreadStateName(thread_state state);
std::string ExpandPath(const std::string& path);
	// ~ and ~/ become the home directory

bigtime_t SecondsToMicro(double seconds);

// A mutex that unlocks itself.
class AutoLocker {
public:
	AutoLocker(sem_id sem) : fSem(sem) { acquire_sem(fSem); }
	~AutoLocker() { release_sem(fSem); }
private:
	sem_id fSem;
};

#endif // AIROS_MCP_UTIL_H
