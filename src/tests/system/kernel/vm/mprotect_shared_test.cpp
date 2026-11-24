/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	mprotect() and set_area_protection() change the access rights of a
	mapping and nothing else (airos-works/haiku#157).

	Both assigned the new rights to the area's whole flags word, which also
	says that the area is shared (and cloneable, overcommitting, a stack).
	A MAP_SHARED mapping that had been through mprotect() over its whole
	length was copied by the next fork() like private memory: parent and
	child no longer shared it, and what was written to a shared file mapping
	afterwards went to anonymous memory and never reached the file.

	Besides, both moved the area onto a new anonymous cache whenever its
	cache had consumers, shared or not: an mprotect() on a shared mapping of a
	file that anyone had mapped privately as well cut the mapping off from the
	file without any fork().

	mprotect_shared_test [directory]   (default: /tmp)
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>

#include <syscalls.h>


static const char* sDirectory = "/tmp";
static int sFailures = 0;


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("FAILED %s:%d: ", __func__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			sFailures++; \
		} \
	} while (false)


enum protect_how {
	NO_CHANGE,				// control
	MPROTECT_WHOLE,			// read-only at first, then mprotect() to writable
	MPROTECT_SAME,			// mprotect() to what it has already
	MPROTECT_PART,			// one page read-only and back
	SET_AREA_PROTECTION		// read-only at first, then set_area_protection()
};

static const char* const kHowNames[] = {
	"no change", "mprotect() of the whole mapping",
	"mprotect() to the same protection", "mprotect() of one page",
	"set_area_protection()"
};

static const size_t kSize = 4 * B_PAGE_SIZE;


static bool
change_protection(void* address, protect_how how)
{
	switch (how) {
		case NO_CHANGE:
			return true;
		case MPROTECT_WHOLE:
		case MPROTECT_SAME:
			return mprotect(address, kSize, PROT_READ | PROT_WRITE) == 0;
		case MPROTECT_PART:
			return mprotect(address, B_PAGE_SIZE, PROT_READ) == 0
				&& mprotect(address, B_PAGE_SIZE, PROT_READ | PROT_WRITE) == 0;
		case SET_AREA_PROTECTION:
			return set_area_protection(area_for(address),
				B_READ_AREA | B_WRITE_AREA) == B_OK;
	}
	return false;
}


static int
initial_protection(protect_how how)
{
	return how == MPROTECT_WHOLE || how == SET_AREA_PROTECTION
		? PROT_READ : PROT_READ | PROT_WRITE;
}


/*!	Shared anonymous memory stays shared with a child forked after the
	change.
*/
static void
test_shared_anonymous(protect_how how)
{
	volatile int* data = (volatile int*)mmap(NULL, kSize,
		initial_protection(how), MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (data == MAP_FAILED) {
		CHECK(false, "mmap: %s", strerror(errno));
		return;
	}
	CHECK(change_protection((void*)data, how), "%s: %s", kHowNames[how],
		strerror(errno));

	pid_t child = fork();
	if (child == 0) {
		data[0] = 42;
		data[B_PAGE_SIZE / sizeof(int)] = 43;
		_exit(0);
	}
	waitpid(child, NULL, 0);

	const int first = data[0];
	const int second = data[B_PAGE_SIZE / sizeof(int)];
	printf("shared anonymous memory, %s, fork(): the parent reads %d and %d "
		"of the child's 42 and 43\n", kHowNames[how], first, second);
	CHECK(first == 42 && second == 43, "the memory is no longer shared");

	munmap((void*)data, kSize);
}


static int
create_file(char* path, size_t pathSize, char fill)
{
	snprintf(path, pathSize, "%s/mprotect_shared_test.%d", sDirectory,
		(int)getpid());
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;

	char buffer[kSize];
	memset(buffer, fill, kSize);
	if (write(fd, buffer, kSize) != (ssize_t)kSize || fsync(fd) != 0) {
		close(fd);
		unlink(path);
		return -1;
	}
	return fd;
}


/*!	What is written to a shared file mapping reaches the file: after a
	fork(), and while another mapping of the file is a private one (which
	gives the file's cache a consumer).
*/
static void
test_shared_file(protect_how how, bool withFork, bool withPrivateMapping)
{
	char path[B_PATH_NAME_LENGTH];
	int fd = create_file(path, sizeof(path), 'A');
	if (fd < 0) {
		CHECK(false, "creating %s: %s", path, strerror(errno));
		return;
	}

	char* privateData = NULL;
	if (withPrivateMapping) {
		privateData = (char*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
			MAP_PRIVATE, fd, 0);
		CHECK(privateData != MAP_FAILED, "private mmap: %s", strerror(errno));
	}

	char* data = (char*)mmap(NULL, kSize, initial_protection(how), MAP_SHARED,
		fd, 0);
	if (data == MAP_FAILED) {
		CHECK(false, "mmap: %s", strerror(errno));
		close(fd);
		unlink(path);
		return;
	}
	CHECK(change_protection(data, how), "%s: %s", kHowNames[how],
		strerror(errno));

	if (withFork) {
		pid_t child = fork();
		if (child == 0)
			_exit(0);
		waitpid(child, NULL, 0);
	}

	memset(data, 'B', kSize);
	const int syncResult = msync(data, kSize, MS_SYNC);
	munmap(data, kSize);

	char back[kSize];
	memset(back, 0, sizeof(back));
	pread(fd, back, kSize, 0);
	printf("shared file mapping%s, %s%s, 'B' written, msync() %d: the file "
		"has '%c' and '%c'\n",
		withPrivateMapping ? " (the file is mapped privately too)" : "",
		kHowNames[how], withFork ? ", fork()" : "", syncResult, back[0],
		back[kSize - 1]);
	CHECK(back[0] == 'B' && back[kSize - 1] == 'B',
		"what was written to the mapping did not reach the file");

	if (privateData != NULL && privateData != MAP_FAILED)
		munmap(privateData, kSize);
	close(fd);
	unlink(path);
}


/*!	The other flags of an area's protection stay as well. */
static void
test_flags_kept()
{
	void* address;
	area_id area = create_area("mprotect test", &address, B_ANY_ADDRESS,
		kSize, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (area < 0) {
		CHECK(false, "create_area: %s", strerror(area));
		return;
	}

	CHECK(mprotect(address, kSize, PROT_READ) == 0, "mprotect: %s",
		strerror(errno));

	area_info info;
	CHECK(get_area_info(area, &info) == B_OK, "get_area_info");
	printf("cloneable area after mprotect(PROT_READ): protection %#" B_PRIx32
		"\n", info.protection);
	CHECK((info.protection & B_CLONEABLE_AREA) != 0,
		"B_CLONEABLE_AREA is gone");
	CHECK((info.protection & (B_READ_AREA | B_WRITE_AREA)) == B_READ_AREA,
		"access rights %#" B_PRIx32, info.protection);

	delete_area(area);
}


/*!	What has to keep working: private memory that was read-only at the
	fork() and is made writable afterwards must not show the child what the
	parent writes.
*/
static void
test_private_stays_private(bool useSetAreaProtection)
{
	volatile int* data = (volatile int*)mmap(NULL, kSize,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (data == MAP_FAILED) {
		CHECK(false, "mmap: %s", strerror(errno));
		return;
	}
	data[0] = 1;
	CHECK(mprotect((void*)data, kSize, PROT_READ) == 0, "mprotect");

	int go[2];
	int answer[2];
	if (pipe(go) != 0 || pipe(answer) != 0) {
		CHECK(false, "pipe");
		return;
	}

	pid_t child = fork();
	if (child == 0) {
		close(go[1]);
		char c;
		read(go[0], &c, 1);
		int value = data[0];
		write(answer[1], &value, sizeof(value));
		_exit(0);
	}
	close(go[0]);
	close(answer[1]);

	if (useSetAreaProtection) {
		CHECK(set_area_protection(area_for((void*)data),
			B_READ_AREA | B_WRITE_AREA) == B_OK, "set_area_protection");
	} else {
		CHECK(mprotect((void*)data, kSize, PROT_READ | PROT_WRITE) == 0,
			"mprotect");
	}
	data[0] = 2;
	close(go[1]);

	int value = -1;
	read(answer[0], &value, sizeof(value));
	close(answer[0]);
	waitpid(child, NULL, 0);

	printf("private memory, read-only at fork(), %s, parent writes 2: the "
		"child reads %d\n",
		useSetAreaProtection ? "set_area_protection()" : "mprotect()", value);
	CHECK(value == 1, "the child sees what the parent wrote");
	CHECK(data[0] == 2, "the parent lost what it wrote");

	munmap((void*)data, kSize);
}


/*!	transfer_area() works with a clone, which is B_SHARED_AREA, and resets
	the protection afterwards: the area it hands over has to stay private.
*/
static void
test_transfer_stays_private()
{
	volatile int* data;
	area_id area = create_area("mprotect test transfer", (void**)&data,
		B_ANY_ADDRESS, kSize, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < 0) {
		CHECK(false, "create_area: %s", strerror(area));
		return;
	}
	data[0] = 1;

	void* address;
	area_id transferred = _kern_transfer_area(area, &address, B_ANY_ADDRESS,
		getpid());
	if (transferred < 0) {
		CHECK(false, "transfer_area: %s", strerror(transferred));
		delete_area(area);
		return;
	}
	data = (volatile int*)address;

	pid_t child = fork();
	if (child == 0) {
		data[0] = 2;
		_exit(0);
	}
	waitpid(child, NULL, 0);

	printf("private area after transfer_area(), fork(), the child writes 2: "
		"the parent reads %d\n", data[0]);
	CHECK(data[0] == 1, "the transferred area is shared with the child");

	delete_area(transferred);
}


/*!	Another team's clone_area() of an area that is not cloneable fails, and
	must not leave the area marked shared: set_area_protection() and
	mprotect() keep that mark now, and fork() would share the area.
*/
static void
test_failed_clone_stays_private()
{
	volatile int* data;
	area_id area = create_area("mprotect test not cloneable", (void**)&data,
		B_ANY_ADDRESS, kSize, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < 0) {
		CHECK(false, "create_area: %s", strerror(area));
		return;
	}
	data[0] = 1;

	pid_t child = fork();
	if (child == 0) {
		void* address;
		area_id clone = clone_area("mprotect test clone", &address,
			B_ANY_ADDRESS, B_READ_AREA, area);
		_exit(clone < 0 ? 0 : 1);
	}
	int status;
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"another team cloned an area that is not cloneable");

	CHECK(set_area_protection(area, B_READ_AREA | B_WRITE_AREA) == B_OK,
		"set_area_protection");
	child = fork();
	if (child == 0) {
		data[0] = 2;
		_exit(0);
	}
	waitpid(child, NULL, 0);

	printf("private area after another team's failed clone_area(), fork(), "
		"the child writes 2: the parent reads %d\n", data[0]);
	CHECK(data[0] == 1, "the area is shared with the child");

	delete_area(area);
}


int
main(int argc, char** argv)
{
	if (argc > 1)
		sDirectory = argv[1];

	setvbuf(stdout, NULL, _IONBF, 0);

	static const protect_how kHows[] = { NO_CHANGE, MPROTECT_WHOLE,
		MPROTECT_SAME, MPROTECT_PART, SET_AREA_PROTECTION };
	const size_t howCount = sizeof(kHows) / sizeof(kHows[0]);

	for (size_t i = 0; i < howCount; i++)
		test_shared_anonymous(kHows[i]);
	for (size_t i = 0; i < howCount; i++)
		test_shared_file(kHows[i], true, false);
	for (size_t i = 0; i < howCount; i++)
		test_shared_file(kHows[i], false, true);

	test_flags_kept();
	test_private_stays_private(false);
	test_private_stays_private(true);
	test_transfer_stays_private();
	test_failed_clone_stays_private();

	if (sFailures != 0) {
		printf("%d check(s) FAILED\n", sFailures);
		return 1;
	}

	printf("all checks passed\n");
	return 0;
}
