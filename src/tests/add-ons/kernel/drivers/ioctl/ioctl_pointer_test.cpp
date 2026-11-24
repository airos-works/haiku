/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Device ioctls with wrong pointers and lengths in what the caller passes:
	the pointers inside the structures of /dev/acpi/call and of the disk
	drivers' B_GET_VECTOR_ICON, the result pointer of the thermal drivers,
	the command length of B_RAW_DEVICE_COMMAND. Each must fail with an error
	(haiku#184); a kernel without the fix panics or writes into kernel
	memory, so this only belongs on a machine that may crash.

	usage: ioctl_pointer_test [acpi | icon | thermal | raw]...
		(all of them without an argument)

	In QEMU, "-acpitable file=ioctl_test.aml" (make_test_ssdt.py) adds a
	thermal zone and the methods the ACPI part evaluates, and a CD drive on
	the IDE bus gives the raw command part its ATAPI device; what has no
	device is skipped.
*/


#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Drivers.h>
#include <OS.h>
#include <device/scsi.h>


// as the drivers have them (there is no public header)

enum {
	kACPITypeInteger = 1,
	kACPITypeString = 2,
	kACPITypeBuffer = 3,
	kACPITypePackage = 4
};

union acpi_object {
	uint32 type;
	struct {
		uint32 type;
		uint64 value;
	} integer;
	struct {
		uint32 type;
		uint32 length;
		char* pointer;
	} string;
	struct {
		uint32 type;
		uint32 length;
		uint8* pointer;
	} buffer;
	struct {
		uint32 type;
		uint32 count;
		acpi_object* objects;
	} package;
	uint8 size[24];
};

struct acpi_call_desc {
	const char*		path;
	struct {
		uint32			count;
		acpi_object*	pointer;
	} args;
	uint32			retval;
	struct {
		size_t			length;
		void*			pointer;
	} result;
	size_t			reslen;
};

static const uint32 kACPICall = 'ACCA';

struct thermal_type {
	uint32	critical_temp;
	uint32	current_temp;
	uint32	hot_temp;
	void*	passive_package;
	uint32	active_count;
	void*	active_devices;
};

static const uint32 kGetThermalType = B_DEVICE_OP_CODES_END + 10001;

static const uint8 kCAMRequestInvalid = 0x06;


static int sFailures = 0;
static int sChecks = 0;
static int sSkipped = 0;

// a page that cannot be accessed, with an accessible one in front of it
static uint8* sGuardPage;
static uint8* sPageBefore;

// kernel memory that may be written to without harm, should a driver do it
static uint8* sKernelTarget;
static uint8* sKernelTargetView;
static const size_t kKernelTargetSize = 64 * 1024;


static void
check(bool ok, const char* format, ...)
{
	char text[256];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);

	sChecks++;
	if (!ok)
		sFailures++;
	printf("%s: %s\n", ok ? "ok" : "FAILED", text);
	fflush(stdout);
}


static void
skip(const char* what)
{
	sSkipped++;
	printf("skipped: %s\n", what);
	fflush(stdout);
}


static void
crashed(int signal)
{
	static const char kMessage[] = "FAILED: the test itself crashed\n";
	write(STDOUT_FILENO, kMessage, sizeof(kMessage) - 1);
	_exit(1);
}


static status_t
do_ioctl(int fd, uint32 op, void* buffer, size_t length)
{
	return ioctl(fd, op, buffer, length) < 0 ? errno : B_OK;
}


/*!	The frame buffer of the boot screen is mapped into the kernel, and
	nobody minds what is in it. Where it can be cloned, the checks also look
	at it; elsewhere a screen shot shows what a driver wrote (QEMU's
	"screendump": the first pixels of the second 4 KiB of the frame buffer).
*/
static void
find_kernel_target()
{
	area_info info;
	ssize_t cookie = 0;
	while (get_next_area_info(B_SYSTEM_TEAM, &cookie, &info) == B_OK) {
		if (strstr(info.name, "frame buffer") == NULL
			|| info.size < kKernelTargetSize
			|| (addr_t)info.address < 0x8000000000000000ULL) {
			continue;
		}

		sKernelTarget = (uint8*)info.address;
		void* view;
		if (clone_area("ioctl test view", &view, B_ANY_ADDRESS, B_READ_AREA,
				info.area) >= 0) {
			sKernelTargetView = (uint8*)view;
		}
		printf("kernel memory to aim at: area \"%s\" at %p%s\n", info.name,
			info.address, sKernelTargetView != NULL ? ""
				: " (cannot be looked at from here)");
		return;
	}
	printf("no kernel area to aim at (a frame buffer): the checks with "
		"kernel addresses are left out\n");
}


/*!	Whether the kernel memory aimed at holds \a data now (and did not
	before); false where it cannot be looked at.
*/
static bool
kernel_target_holds(size_t offset, const void* data, size_t size)
{
	return sKernelTargetView != NULL
		&& memcmp(sKernelTargetView + offset, data, size) == 0;
}


// #pragma mark - /dev/acpi/call


static status_t
acpi_call(int fd, acpi_call_desc& call)
{
	return do_ioctl(fd, kACPICall, &call, sizeof(call));
}


static void
test_acpi()
{
	printf("--- /dev/acpi/call\n");
	int fd = open("/dev/acpi/call", O_RDWR);
	if (fd < 0) {
		skip("no /dev/acpi/call");
		return;
	}

	static uint8 result[1024];
	acpi_object args[8];
	acpi_call_desc call;

	// Is the test table there?
	memset(&call, 0, sizeof(call));
	memset(result, 0xee, sizeof(result));
	call.path = "\\TST0";
	call.result.length = sizeof(result);
	call.result.pointer = result;
	status_t status = acpi_call(fd, call);
	bool methods = status == B_OK
		&& ((acpi_object*)result)->type == kACPITypeInteger
		&& ((acpi_object*)result)->integer.value == 7;
	if (!methods) {
		skip("the test methods (\\TST0...) are not in the ACPI tables: no "
			"evaluation with arguments");
	}

	if (methods) {
		// integer arguments
		memset(&call, 0, sizeof(call));
		memset(args, 0, sizeof(args));
		memset(result, 0xee, sizeof(result));
		args[0].integer.type = kACPITypeInteger;
		args[0].integer.value = 20;
		args[1].integer.type = kACPITypeInteger;
		args[1].integer.value = 22;
		call.path = "\\TST2";
		call.args.count = 2;
		call.args.pointer = args;
		call.result.length = sizeof(result);
		call.result.pointer = result;
		status = acpi_call(fd, call);
		check(status == B_OK
				&& ((acpi_object*)result)->type == kACPITypeInteger
				&& ((acpi_object*)result)->integer.value == 42,
			"two integer arguments: %s, result %" B_PRIu64, strerror(status),
			((acpi_object*)result)->integer.value);

		// a string argument, returned as it is
		char text[] = "from user memory";
		memset(&call, 0, sizeof(call));
		memset(args, 0, sizeof(args));
		memset(result, 0xee, sizeof(result));
		args[0].string.type = kACPITypeString;
		args[0].string.length = strlen(text);
		args[0].string.pointer = text;
		call.path = "\\TST1";
		call.args.count = 1;
		call.args.pointer = args;
		call.result.length = sizeof(result);
		call.result.pointer = result;
		status = acpi_call(fd, call);
		acpi_object* object = (acpi_object*)result;
		bool same = status == B_OK && object->type == kACPITypeString
			&& object->string.length == strlen(text)
			&& (uint8*)object->string.pointer > result
			&& (uint8*)object->string.pointer + strlen(text)
				<= result + sizeof(result)
			&& memcmp(object->string.pointer, text, strlen(text)) == 0;
		check(same, "a string argument comes back: %s", strerror(status));

		// a buffer argument
		uint8 bytes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
		memset(&call, 0, sizeof(call));
		memset(args, 0, sizeof(args));
		memset(result, 0xee, sizeof(result));
		args[0].buffer.type = kACPITypeBuffer;
		args[0].buffer.length = sizeof(bytes);
		args[0].buffer.pointer = bytes;
		call.path = "\\TST1";
		call.args.count = 1;
		call.args.pointer = args;
		call.result.length = sizeof(result);
		call.result.pointer = result;
		status = acpi_call(fd, call);
		same = status == B_OK && object->type == kACPITypeBuffer
			&& object->buffer.length == sizeof(bytes)
			&& object->buffer.pointer > result
			&& object->buffer.pointer + sizeof(bytes)
				<= result + sizeof(result)
			&& memcmp(object->buffer.pointer, bytes, sizeof(bytes)) == 0;
		check(same, "a buffer argument comes back: %s", strerror(status));

		// a path that ends where its mapping ends
		const char kPath[] = "\\TST0";
		char* path = (char*)sGuardPage - sizeof(kPath);
		memcpy(path, kPath, sizeof(kPath));
		memset(&call, 0, sizeof(call));
		memset(result, 0xee, sizeof(result));
		call.path = path;
		call.result.length = sizeof(result);
		call.result.pointer = result;
		status = acpi_call(fd, call);
		check(status == B_OK && object->type == kACPITypeInteger
				&& object->integer.value == 7,
			"a path at the end of its mapping: %s", strerror(status));
	}

	// The rest must fail before anything is evaluated.

	memset(args, 0, sizeof(args));
	for (int i = 0; i < 8; i++)
		args[i].integer.type = kACPITypeInteger;

	memset(&call, 0, sizeof(call));
	call.path = "\\TST2";
	call.args.count = 8;
	call.args.pointer = args;
	status = acpi_call(fd, call);
	check(status == B_BAD_VALUE, "eight arguments: %s", strerror(status));

	memset(&call, 0, sizeof(call));
	call.path = "\\TST2";
	call.args.count = 0x40000000;
	call.args.pointer = args;
	status = acpi_call(fd, call);
	check(status == B_BAD_VALUE, "a huge argument count: %s",
		strerror(status));

	memset(&call, 0, sizeof(call));
	call.path = "\\TST2";
	call.args.count = 2;
	call.args.pointer = (acpi_object*)sGuardPage;
	status = acpi_call(fd, call);
	check(status == B_BAD_ADDRESS, "arguments in unmapped memory: %s",
		strerror(status));

	if (sKernelTarget != NULL) {
		memset(&call, 0, sizeof(call));
		call.path = "\\TST2";
		call.args.count = 2;
		call.args.pointer = (acpi_object*)sKernelTarget;
		status = acpi_call(fd, call);
		check(status == B_BAD_ADDRESS, "arguments in kernel memory: %s",
			strerror(status));
	}

	memset(&call, 0, sizeof(call));
	memset(args, 0, sizeof(args));
	args[0].string.type = kACPITypeString;
	args[0].string.length = 16;
	args[0].string.pointer = (char*)sGuardPage;
	call.path = "\\TST1";
	call.args.count = 1;
	call.args.pointer = args;
	status = acpi_call(fd, call);
	check(status == B_BAD_ADDRESS, "a string argument in unmapped memory: %s",
		strerror(status));

	if (sKernelTarget != NULL) {
		args[0].buffer.type = kACPITypeBuffer;
		args[0].buffer.length = 16;
		args[0].buffer.pointer = sKernelTarget;
		status = acpi_call(fd, call);
		check(status == B_BAD_ADDRESS,
			"a buffer argument in kernel memory: %s", strerror(status));
	}

	args[0].buffer.type = kACPITypeBuffer;
	args[0].buffer.length = 0xfffffff0;
	args[0].buffer.pointer = (uint8*)args;
	status = acpi_call(fd, call);
	check(status == B_BAD_VALUE, "a 4 GiB buffer argument: %s",
		strerror(status));

	acpi_object element;
	element.integer.type = kACPITypeInteger;
	element.integer.value = 1;
	args[0].package.type = kACPITypePackage;
	args[0].package.count = 1;
	args[0].package.objects = &element;
	status = acpi_call(fd, call);
	check(status == B_BAD_VALUE, "a package argument: %s", strerror(status));

	// the path
	memset(&call, 0, sizeof(call));
	call.path = (char*)sGuardPage;
	status = acpi_call(fd, call);
	check(status == B_BAD_ADDRESS, "a path in unmapped memory: %s",
		strerror(status));

	if (sKernelTarget != NULL) {
		call.path = (char*)sKernelTarget;
		status = acpi_call(fd, call);
		check(status == B_BAD_ADDRESS, "a path in kernel memory: %s",
			strerror(status));
	}

	static char longPath[4096];
	memset(longPath, 'A', sizeof(longPath) - 1);
	longPath[0] = '\\';
	call.path = longPath;
	status = acpi_call(fd, call);
	check(status == B_NAME_TOO_LONG, "a path of 4095 characters: %s",
		strerror(status));

	// the result
	if (sKernelTarget != NULL) {
		uint8 before[64] = {};
		if (sKernelTargetView != NULL)
			memcpy(before, sKernelTargetView, sizeof(before));
		memset(&call, 0, sizeof(call));
		call.path = methods ? "\\TST0" : "\\_SB";
		call.result.length = 1024;
		call.result.pointer = sKernelTarget;
		status = acpi_call(fd, call);
		bool written = sKernelTargetView != NULL
			&& memcmp(before, sKernelTargetView, sizeof(before)) != 0;
		check(status == B_BAD_ADDRESS && !written,
			"a result buffer in kernel memory: %s%s", strerror(status),
			written ? ", the memory was OVERWRITTEN" : "");
	}

	if (methods) {
		memset(&call, 0, sizeof(call));
		call.path = "\\TST0";
		call.result.length = 1024;
		call.result.pointer = sGuardPage;
		status = acpi_call(fd, call);
		check(status == B_BAD_ADDRESS, "a result buffer in unmapped memory: "
			"%s", strerror(status));
	}

	close(fd);
}


// #pragma mark - B_GET_VECTOR_ICON


static void
test_icon(const char* path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("skipped: %s: %s\n", path, strerror(errno));
		return;
	}

	// how large is it?
	device_icon icon;
	icon.icon_size = 0;
	icon.icon_data = NULL;
	status_t status = do_ioctl(fd, B_GET_VECTOR_ICON, &icon, sizeof(icon));
	if (status != B_OK || icon.icon_size <= 4
		|| (size_t)icon.icon_size > kKernelTargetSize) {
		printf("skipped: %s has no vector icon (%s)\n", path,
			strerror(status));
		close(fd);
		return;
	}
	int32 size = icon.icon_size;

	uint8* data = (uint8*)malloc(size);
	icon.icon_data = data;
	status = do_ioctl(fd, B_GET_VECTOR_ICON, &icon, sizeof(icon));
	check(status == B_OK && memcmp(data, "ncif", 4) == 0,
		"%s: the icon, %" B_PRId32 " bytes: %s", path, size,
		strerror(status));

	icon.icon_size = size;
	icon.icon_data = sGuardPage;
	status = do_ioctl(fd, B_GET_VECTOR_ICON, &icon, sizeof(icon));
	check(status == B_BAD_ADDRESS, "%s: icon into unmapped memory: %s", path,
		strerror(status));

	if (sKernelTarget != NULL) {
		// (the second page: the ACPI part aims at the first)
		bool heldBefore = kernel_target_holds(B_PAGE_SIZE, data, size);
		icon.icon_size = size;
		icon.icon_data = sKernelTarget + B_PAGE_SIZE;
		status = do_ioctl(fd, B_GET_VECTOR_ICON, &icon, sizeof(icon));
		bool written = !heldBefore
			&& kernel_target_holds(B_PAGE_SIZE, data, size);
		check(status == B_BAD_ADDRESS && !written,
			"%s: icon into kernel memory: %s%s", path, strerror(status),
			written ? ", the memory NOW HOLDS THE ICON" : "");
	}

	free(data);
	close(fd);
}


static void
test_icons_in(const char* directory, int depth)
{
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return;

	while (dirent* entry = readdir(dir)) {
		if (entry->d_name[0] == '.')
			continue;
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		struct stat st;
		if (lstat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 6)
				test_icons_in(path, depth + 1);
		} else if (strcmp(entry->d_name, "raw") == 0)
			test_icon(path);
	}
	closedir(dir);
}


// #pragma mark - thermal drivers


static void
test_thermal_device(const char* path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("skipped: %s: %s\n", path, strerror(errno));
		return;
	}

	thermal_type type;
	memset(&type, 0xee, sizeof(type));
	status_t status = do_ioctl(fd, kGetThermalType, &type, sizeof(type));
	check(status == B_OK && type.critical_temp != 0xeeeeeeee,
		"%s: thermal type: %s (critical %" B_PRIu32 ", current %" B_PRIu32
		")", path, strerror(status), type.critical_temp, type.current_temp);

	status = do_ioctl(fd, kGetThermalType, sGuardPage, sizeof(type));
	check(status == B_BAD_ADDRESS, "%s: thermal type into unmapped memory: "
		"%s", path, strerror(status));

	// across the end of a mapping
	status = do_ioctl(fd, kGetThermalType, sGuardPage - 4, sizeof(type));
	check(status == B_BAD_ADDRESS, "%s: thermal type across the end of a "
		"mapping: %s", path, strerror(status));

	close(fd);
}


static void
test_thermal()
{
	printf("--- thermal drivers\n");
	int found = 0;
	const char* kDirectories[] = { "/dev/power/acpi_thermal",
		"/dev/power/pch_thermal", "/dev/power/amd_thermal" };
	for (size_t i = 0; i < B_COUNT_OF(kDirectories); i++) {
		DIR* dir = opendir(kDirectories[i]);
		if (dir == NULL)
			continue;
		while (dirent* entry = readdir(dir)) {
			if (entry->d_name[0] == '.')
				continue;
			char path[B_PATH_NAME_LENGTH];
			snprintf(path, sizeof(path), "%s/%s", kDirectories[i],
				entry->d_name);
			test_thermal_device(path);
			found++;
		}
		closedir(dir);
	}
	if (found == 0)
		skip("no thermal device");
}


// #pragma mark - B_RAW_DEVICE_COMMAND


static void
test_raw_device(const char* path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("skipped: %s: %s\n", path, strerror(errno));
		return;
	}

	// INQUIRY, six bytes: what a raw command normally looks like
	uint8 data[96];
	uint8 sense[32];
	raw_device_command command;
	memset(&command, 0, sizeof(command));
	command.command[0] = 0x12;
	command.command[4] = 36;
	command.command_length = 6;
	command.flags = B_RAW_DEVICE_DATA_IN;
	command.data = data;
	command.data_length = 36;
	command.sense_data = sense;
	command.sense_data_length = sizeof(sense);
	command.timeout = 5000000;
	status_t status = do_ioctl(fd, B_RAW_DEVICE_COMMAND, &command,
		sizeof(command));
	check(status == B_OK && (command.cam_status & 0x3f) == 0x01,
		"%s: INQUIRY: %s, status %#x, \"%.8s\"", path, strerror(status),
		command.cam_status, status == B_OK ? (char*)data + 8 : "");

	// READ CAPACITY (16): sixteen bytes do not fit an ATAPI packet
	memset(&command, 0, sizeof(command));
	command.command[0] = 0x9e;
	command.command[1] = 0x10;
	command.command[13] = 32;
	command.command_length = 16;
	command.flags = B_RAW_DEVICE_DATA_IN;
	command.data = data;
	command.data_length = 32;
	command.sense_data = sense;
	command.sense_data_length = sizeof(sense);
	command.timeout = 5000000;
	status = do_ioctl(fd, B_RAW_DEVICE_COMMAND, &command, sizeof(command));
	check(status == B_OK && (command.cam_status & 0x3f) == kCAMRequestInvalid,
		"%s: a 16 byte command is refused: %s, status %#x", path,
		strerror(status), command.cam_status);

	// a command length that is no command's
	memset(&command, 0, sizeof(command));
	command.command[0] = 0x12;
	command.command[4] = 36;
	command.command_length = 255;
	command.flags = B_RAW_DEVICE_DATA_IN;
	command.data = data;
	command.data_length = 36;
	command.timeout = 5000000;
	status = do_ioctl(fd, B_RAW_DEVICE_COMMAND, &command, sizeof(command));
	check(status == B_BAD_VALUE, "%s: a command length of 255: %s", path,
		strerror(status));

	close(fd);
}


static void
test_raw_in(const char* directory, int depth, int& found)
{
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return;

	while (dirent* entry = readdir(dir)) {
		if (entry->d_name[0] == '.')
			continue;
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		struct stat st;
		if (lstat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 6)
				test_raw_in(path, depth + 1, found);
		} else if (strcmp(entry->d_name, "raw") == 0) {
			test_raw_device(path);
			found++;
		}
	}
	closedir(dir);
}


int
main(int argc, char** argv)
{
	signal(SIGSEGV, crashed);
	signal(SIGBUS, crashed);
	alarm(120);
		// nothing here waits, but a hanging ioctl must not hang the test

	uint8* pages = (uint8*)mmap(NULL, 2 * B_PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (pages == MAP_FAILED
		|| mprotect(pages + B_PAGE_SIZE, B_PAGE_SIZE, PROT_NONE) != 0) {
		printf("FAILED: no guard page: %s\n", strerror(errno));
		return 1;
	}
	sPageBefore = pages;
	sGuardPage = pages + B_PAGE_SIZE;

	find_kernel_target();

	bool all = argc < 2;
	bool acpi = all, icon = all, thermal = all, raw = all;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "acpi") == 0)
			acpi = true;
		else if (strcmp(argv[i], "icon") == 0)
			icon = true;
		else if (strcmp(argv[i], "thermal") == 0)
			thermal = true;
		else if (strcmp(argv[i], "raw") == 0)
			raw = true;
		else {
			fprintf(stderr, "usage: %s [acpi | icon | thermal | raw]...\n",
				argv[0]);
			return 2;
		}
	}

	if (icon) {
		printf("--- B_GET_VECTOR_ICON\n");
		test_icons_in("/dev/disk", 0);
	}
	if (raw) {
		// only ATAPI devices have the 12 byte limit; SCSI disks of other
		// busses take 16 byte commands
		printf("--- B_RAW_DEVICE_COMMAND\n");
		int found = 0;
		test_raw_in("/dev/disk/atapi", 0, found);
		if (found == 0)
			skip("no ATAPI device");
	}
	if (thermal)
		test_thermal();
	if (acpi)
		test_acpi();

	printf("%d checks, %d failed, %d parts skipped\n", sChecks, sFailures,
		sSkipped);
	return sFailures == 0 ? 0 : 1;
}
