/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// durable_write <dir> <name> <bytes>: writes <dir>/<name>.tmp, fsync()s it,
// renames it to <dir>/<name> and fsync()s the directory -- the sequence an
// application uses to replace a file safely. durable_write --check <dir>
// <name> <bytes> verifies the file after a power cut.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned char pattern(size_t i) { return (unsigned char)(i * 131 + 7); }

int main(int argc, char** argv)
{
	bool check = argc > 1 && strcmp(argv[1], "--check") == 0;
	if (argc != (check ? 5 : 4)) {
		fprintf(stderr, "usage: durable_write [--check] <dir> <name> <bytes>\n");
		return 2;
	}
	const char* dir = argv[check ? 2 : 1];
	const char* name = argv[check ? 3 : 2];
	size_t size = strtoul(argv[check ? 4 : 3], NULL, 0);
	char path[1024], temp[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	snprintf(temp, sizeof(temp), "%s/%s.tmp", dir, name);

	unsigned char* buffer = (unsigned char*)malloc(size);
	if (check) {
		int fd = open(path, O_RDONLY);
		if (fd < 0) {
			printf("%s: MISSING (%s)\n", path, strerror(errno));
			return 1;
		}
		struct stat st;
		fstat(fd, &st);
		ssize_t n = read(fd, buffer, size);
		size_t bad = 0;
		for (ssize_t i = 0; i < n; i++)
			bad += buffer[i] != pattern(i);
		size_t zero = 0;
		for (ssize_t i = 0; i < n; i++)
			zero += buffer[i] == 0;
		printf("%s: size %lld (expected %zu), %zu wrong bytes, %zu zero bytes\n",
			path, (long long)st.st_size, size, bad, zero);
		return st.st_size == (off_t)size && bad == 0 ? 0 : 1;
	}

	for (size_t i = 0; i < size; i++)
		buffer[i] = pattern(i);
	int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, buffer, size) != (ssize_t)size || fsync(fd) != 0) {
		perror("write");
		return 1;
	}
	close(fd);
	if (rename(temp, path) != 0) {
		perror("rename");
		return 1;
	}
	int dirFD = open(dir, O_RDONLY);
	if (dirFD < 0 || fsync(dirFD) != 0) {
		perror("fsync dir");
		return 1;
	}
	close(dirFD);
	printf("wrote %s\n", path);
	return 0;
}
