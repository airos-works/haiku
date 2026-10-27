/*
 * airos_mcp - small helpers shared by the tools.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include "Util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <FindDirectory.h>
#include <Path.h>


static const char kBase64Chars[]
	= "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


std::string
Base64Encode(const void* data, size_t length)
{
	const uint8_t* in = (const uint8_t*)data;
	std::string out;
	out.reserve((length + 2) / 3 * 4);
	for (size_t i = 0; i < length; i += 3) {
		uint32_t triple = in[i] << 16;
		if (i + 1 < length)
			triple |= in[i + 1] << 8;
		if (i + 2 < length)
			triple |= in[i + 2];
		out += kBase64Chars[(triple >> 18) & 0x3f];
		out += kBase64Chars[(triple >> 12) & 0x3f];
		out += i + 1 < length ? kBase64Chars[(triple >> 6) & 0x3f] : '=';
		out += i + 2 < length ? kBase64Chars[triple & 0x3f] : '=';
	}
	return out;
}


bool
Base64Decode(const std::string& text, std::string& out)
{
	out.clear();
	uint32_t buffer = 0;
	int bits = 0;
	for (size_t i = 0; i < text.size(); i++) {
		char c = text[i];
		if (c == '=' || c == '\n' || c == '\r' || c == ' ')
			continue;
		const char* p = strchr(kBase64Chars, c);
		if (p == NULL || *p == 0)
			return false;
		buffer = (buffer << 6) | (uint32_t)(p - kBase64Chars);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += (char)((buffer >> bits) & 0xff);
		}
	}
	return true;
}


bool
ReadFileToString(const std::string& path, std::string& out, size_t maxBytes,
	off_t offset)
{
	out.clear();
	int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	if (offset > 0 && lseek(fd, offset, SEEK_SET) < 0) {
		close(fd);
		return false;
	}
	char buffer[65536];
	while (true) {
		ssize_t got = read(fd, buffer, sizeof(buffer));
		if (got <= 0)
			break;
		out.append(buffer, got);
		if (maxBytes != 0 && out.size() >= maxBytes) {
			out.resize(maxBytes);
			break;
		}
	}
	close(fd);
	return true;
}


bool
WriteStringToFile(const std::string& path, const std::string& data, mode_t mode)
{
	int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return false;
	size_t done = 0;
	while (done < data.size()) {
		ssize_t written = write(fd, data.data() + done, data.size() - done);
		if (written <= 0) {
			close(fd);
			return false;
		}
		done += written;
	}
	close(fd);
	return true;
}


std::vector<std::string>
SplitLines(const std::string& text)
{
	std::vector<std::string> lines;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) {
			lines.push_back(text.substr(start));
			break;
		}
		lines.push_back(text.substr(start, end - start));
		start = end + 1;
	}
	return lines;
}


std::string
TrimString(const std::string& s)
{
	size_t start = 0;
	while (start < s.size() && isspace((unsigned char)s[start]))
		start++;
	size_t end = s.size();
	while (end > start && isspace((unsigned char)s[end - 1]))
		end--;
	return s.substr(start, end - start);
}


bool
StartsWith(const std::string& s, const char* prefix)
{
	return s.compare(0, strlen(prefix), prefix) == 0;
}


bool
EndsWith(const std::string& s, const char* suffix)
{
	size_t length = strlen(suffix);
	return s.size() >= length && s.compare(s.size() - length, length, suffix) == 0;
}


std::string
TailBytes(const std::string& s, size_t maxBytes)
{
	if (maxBytes == 0 || s.size() <= maxBytes)
		return s;
	size_t start = s.size() - maxBytes;
	size_t newline = s.find('\n', start);
	if (newline != std::string::npos && newline + 1 < s.size())
		start = newline + 1;
	return s.substr(start);
}


std::string
Format(const char* format, ...)
{
	char buffer[4096];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}


std::string
StrError(status_t error)
{
	return Format("%s (0x%x)", strerror(error), (unsigned)error);
}


std::string
ThreadStateName(thread_state state)
{
	switch (state) {
		case B_THREAD_RUNNING: return "running";
		case B_THREAD_READY: return "ready";
		case B_THREAD_RECEIVING: return "receiving";
		case B_THREAD_ASLEEP: return "asleep";
		case B_THREAD_SUSPENDED: return "suspended";
		case B_THREAD_WAITING: return "waiting";
	}
	return Format("state %d", (int)state);
}


std::string
ExpandPath(const std::string& path)
{
	if (path.empty() || path[0] != '~')
		return path;
	BPath home;
	if (find_directory(B_USER_DIRECTORY, &home) != B_OK)
		return path;
	if (path.size() == 1)
		return home.Path();
	if (path[1] == '/')
		return std::string(home.Path()) + path.substr(1);
	return path;
}


bigtime_t
SecondsToMicro(double seconds)
{
	if (seconds <= 0)
		return 0;
	return (bigtime_t)(seconds * 1000000.0);
}
