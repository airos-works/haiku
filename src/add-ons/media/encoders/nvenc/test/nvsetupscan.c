/* nvsetupscan: see what NVIDIA's own driver tells NVENC, on Linux.
 *
 * Preloaded into a program that encodes with NVIDIA's driver (ffmpeg with
 * h264_nvenc or hevc_nvenc), it looks through the program's memory for the
 * two things this add-on has to get right and writes each distinct one it
 * sees to a file:
 *
 * - the driver picture setup buffer (SET_IN_DRV_PIC_SETUP), which starts
 *   with the magic number (class << 16) | 0x0006: setup-NNNN.bin, 16 KB from
 *   the magic on (or to the end of the mapping), so the picture, slice,
 *   motion search, mode decision and quantisation controls are in it
 *   wherever the picture control places them;
 * - the list of methods of a picture, which starts with SET_APPLICATION_ID
 *   (0x200) in the channel's push buffer: methods-NNNN.bin, 2 KB.
 *
 *   gcc -shared -fPIC -O2 -o nvsetupscan.so nvsetupscan.c -lpthread
 *   NVSETUP_OUT=/tmp/nvsetup LD_PRELOAD=./nvsetupscan.so \
 *       ffmpeg -f lavfi -i testsrc2=s=1280x736:r=30 -frames:v 600 \
 *       -c:v hevc_nvenc -preset p4 -bf 0 -g 300 -f null -
 *
 * Settings (environment):
 *   NVSETUP_OUT     the directory to write to (required; nothing runs without)
 *   NVSETUP_CLASS   the encoder class (0xc2b7 for a GTX 1070; the add-on's
 *                   NVENC_DEBUG=1 prints it); unset or "any" takes any NVENC
 *                   class (0x??b7)
 *   NVSETUP_UVM     1: also look in /dev/nvidia-uvm mappings (reading those
 *                   can move memory between the card and the CPU)
 *   NVSETUP_RAW     times in ms after start ("1500,4000"): then copy every
 *                   /dev/nvidia* mapping of at most 64 MB to raw/, for
 *                   looking through afterwards when nothing was found
 *   NVSETUP_RAW_MB  how much raw/ may take in all (default 512)
 *   NVSETUP_MAX     the most files to write (default 4000)
 *   NVSETUP_POLL_US how often the candidates are looked at (default 1000)
 *
 * Run it for H.265 without B pictures (-bf 0), with them, and for H.264 with
 * -bf 2; ffmpeg's -re (pictures at their real rate) leaves each picture's
 * setup in place long enough to be caught whole. The add-on writes its own
 * setup buffers with NVENC_DUMP=<directory>, to compare.
 *
 * Next to the buffers it writes index.txt (one line a file: time, address,
 * the mapping it is in and the first words), maps-*.txt (the program's
 * mappings whenever the driver's change) and summary.txt at exit.
 *
 * The buffers live in memory the driver maps from /dev/nvidia*, which
 * another process cannot read (/proc/<pid>/mem fails on such mappings), so
 * the looking is done from inside: one thread finds candidates every 200 ms
 * (other memory every second), another watches them, by default every
 * millisecond, and writes a copy whenever one changes and holds still.
 * Nothing is written to the program's memory. A mapping that goes away
 * under the scanner is skipped (its SIGSEGV or SIGBUS is caught on the
 * scanner's threads only, all others go on to the program's handlers). */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <link.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_CANDIDATES	256
#define SETUP_SIZE	16384
#define WATCH_SETUP	1024	/* what is compared every poll */
#define WATCH_METHODS	256
#define METHODS_STALE	3000	/* ms unchanged before a method list is dropped */
#define METHODS_SIZE	2048
#define CHUNK		65536
#define DEVICE_LIMIT	(1ul << 30)
#define PLAIN_LIMIT	(1ul << 28)
#define RAW_MAPPING_LIMIT	(64ul << 20)
#define HASHES		16384
#define MAX_RAW_TIMES	8
#define STACK_SIZE	(256 * 1024)
#define SETTLE_MS	50	/* after a change, whole copies are compared this long */

enum { KIND_SETUP, KIND_METHODS };

typedef struct {
	const uint8_t	*address;
	size_t		size;		/* bytes to copy: up to the end of the mapping */
	int		kind;
	uint64_t	lastHash;
	uint64_t	lastChange;	/* ms, for replacing the stalest one */
	uint64_t	lastFullHash;
	char		where[96];
} Candidate;

static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static Candidate sCandidates[MAX_CANDIDATES];
static int sCandidateCount;
static uint64_t sHashes[HASHES];	/* everything written, by content */

static uint32_t sMagic;			/* 0: any 0x??b7 class */
static const char *sOut;
static int sUvm;
static int sMaxFiles = 4000;
static long sPollMicros = 1000;
static uint64_t sRawTimes[MAX_RAW_TIMES];
static int sRawTimeCount;
static uint64_t sRawBudget = 512ull << 20;
static uint64_t sStart;

/* statistics for summary.txt */
static volatile int sFiles, sSetups, sMethodLists, sFaults, sRawFiles, sFinds,
	sCandidatesSeen, sReplaced;
static volatile uint32_t sClassesSeen[8];

static __thread sigjmp_buf *volatile sRecover;
static __thread char *sForwardFrame;
static __thread siginfo_t *sForwardInfo;
/* the scanner's own buffers, which must not be found */
static uint8_t *sBuffers;
static size_t sBuffersSize;
/* and the library's own segments */
static uintptr_t sSelf[8][2];
static int sSelfCount;
static struct sigaction sOldSegv, sOldBus;
static FILE *sIndex;


static uint64_t
now_ms(void)
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}


static void
note(const char *format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	fprintf(stderr, "nvsetupscan: %s\n", line);
}


static void
fault(int signal, siginfo_t *info, void *context)
{
	/* a mapping went away under the scanner: skip it */
	if (sRecover != NULL) {
		sFaults++;
		siglongjmp(*sRecover, 1);
	}
	/* the program's fault: on to its handler. One that passes it back to
	 * us calls us from within itself, deeper on the stack than our call to
	 * it and with the same siginfo; one that leaves by siglongjmp never
	 * returns, and a later fault comes with a siginfo of its own. */
	char here;
	struct sigaction *old = signal == SIGSEGV ? &sOldSegv : &sOldBus;
	if (sForwardFrame == NULL || &here >= sForwardFrame || info != sForwardInfo) {
		char *outer = sForwardFrame;
		siginfo_t *outerInfo = sForwardInfo;
		sForwardFrame = &here;
		sForwardInfo = info;
		if (old->sa_flags & SA_SIGINFO) {
			if (old->sa_sigaction != NULL && old->sa_sigaction != fault) {
				old->sa_sigaction(signal, info, context);
				sForwardFrame = outer;
				sForwardInfo = outerInfo;
				return;
			}
		} else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
			old->sa_handler(signal);
			sForwardFrame = outer;
			sForwardInfo = outerInfo;
			return;
		}
		sForwardFrame = outer;
		sForwardInfo = outerInfo;
	}
	struct sigaction fallback;
	memset(&fallback, 0, sizeof(fallback));
	fallback.sa_handler = SIG_DFL;
	sigaction(signal, &fallback, NULL);
	raise(signal);
}


/* The program or the driver may set handlers of their own after ours: keep
 * ours first, passing on to theirs. */
static void
keep_handlers(void)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&lock);
	int signals[2] = { SIGSEGV, SIGBUS };
	struct sigaction *olds[2] = { &sOldSegv, &sOldBus };
	for (int i = 0; i < 2; i++) {
		struct sigaction current;
		if (sigaction(signals[i], NULL, &current) != 0)
			continue;
		if ((current.sa_flags & SA_SIGINFO) && current.sa_sigaction == fault)
			continue;
		struct sigaction action;
		memset(&action, 0, sizeof(action));
		action.sa_sigaction = fault;
		action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
		sigemptyset(&action.sa_mask);
		struct sigaction previous;
		if (sigaction(signals[i], &action, &previous) == 0)
			*olds[i] = previous;
	}
	pthread_mutex_unlock(&lock);
}


/* Copies memory that may vanish; returns 0 when it faulted. */
static int
safe_copy(void *to, const void *from, size_t size)
{
	/* a handler set since the last look would take the fault instead */
	keep_handlers();
	sigjmp_buf recover;
	if (sigsetjmp(recover, 1) != 0) {
		sRecover = NULL;
		return 0;
	}
	sRecover = &recover;
	memcpy(to, from, size);
	sRecover = NULL;
	return 1;
}


static uint64_t
hash(const uint8_t *data, size_t size)
{
	uint64_t value = 1469598103934665603ull;
	for (size_t i = 0; i < size; i++)
		value = (value ^ data[i]) * 1099511628211ull;
	return value | 1;
}


/* Returns 1 when the content is new. */
static int
remember(uint64_t value)
{
	unsigned slot = (unsigned)(value % HASHES);
	for (int probes = 0; probes < HASHES; probes++) {
		if (sHashes[slot] == value)
			return 0;
		if (sHashes[slot] == 0) {
			sHashes[slot] = value;
			return 1;
		}
		slot = (slot + 1) % HASHES;
	}
	return 1;
}


static int
is_magic(uint32_t word)
{
	if (sMagic != 0)
		return word == sMagic;
	/* any NVENC class: 0xa0b7 (Kepler) to 0xd1b7 and later */
	return (word & 0x00ffffff) == 0x00b70006 && (word >> 24) >= 0x90;
}


static void
note_class(uint32_t word)
{
	for (int i = 0; i < 8; i++) {
		if (sClassesSeen[i] == word)
			return;
		if (sClassesSeen[i] == 0) {
			sClassesSeen[i] = word;
			return;
		}
	}
}


/* A method header (Fermi and later push buffers) for method 0x200,
 * SET_APPLICATION_ID, on any subchannel. Returns the number of words it
 * takes (header and data), or 0. */
static int
application_header(const uint32_t *words, uint32_t *application)
{
	uint32_t header = words[0];
	uint32_t operation = header >> 29;
	if ((header & 0x1fff) != 0x80)
		return 0;
	uint32_t count = (header >> 16) & 0x1fff;
	switch (operation) {
		case 1:	/* incrementing */
		case 3:	/* not incrementing */
		case 5:	/* increment once */
			if (count < 1 || count > 16)
				return 0;
			*application = words[1];
			return 1 + count;
		case 4:	/* immediate data */
			*application = count;
			return 1;
		default:
			return 0;
	}
}


static int
is_header(uint32_t header)
{
	uint32_t operation = header >> 29;
	if (operation == 4)
		return 1;
	if (operation != 1 && operation != 3 && operation != 5)
		return 0;
	uint32_t count = (header >> 16) & 0x1fff;
	return count >= 1 && count <= 0x400;
}


/* A method list starts with SET_APPLICATION_ID (1 H.264, 3 H.265, 7 AV1;
 * older classes 2 for VP8), and is followed by more methods. In memory that
 * is not the driver's, two more headers must follow. */
static int
is_method_list(const uint32_t *words, int strict)
{
	uint32_t application;
	int length = application_header(words, &application);
	if (length == 0 || application < 1 || application > 8)
		return 0;
	if (!is_header(words[length]))
		return 0;
	if (!strict)
		return 1;
	uint32_t next = words[length];
	int skip = (next >> 29) == 4 ? 1 : 1 + (int)((next >> 16) & 0x1fff);
	if (length + skip >= 64)
		return 1;
	return is_header(words[length + skip]);
}


static void
add_candidate(const uint8_t *address, size_t size, int kind, const char *where)
{
	pthread_mutex_lock(&sLock);
	for (int i = 0; i < sCandidateCount; i++) {
		if (sCandidates[i].address == address) {
			sCandidates[i].size = size;
			pthread_mutex_unlock(&sLock);
			return;
		}
	}
	int index = sCandidateCount;
	if (index == MAX_CANDIDATES) {
		/* replace the one that has not changed for longest */
		index = 0;
		for (int i = 1; i < sCandidateCount; i++) {
			if (sCandidates[i].lastChange < sCandidates[index].lastChange)
				index = i;
		}
		sReplaced++;
	} else
		sCandidateCount++;
	Candidate *candidate = &sCandidates[index];
	candidate->address = address;
	candidate->size = size;
	candidate->kind = kind;
	candidate->lastHash = 0;
	candidate->lastFullHash = 0;
	candidate->lastChange = now_ms();
	snprintf(candidate->where, sizeof(candidate->where), "%s", where);
	sCandidatesSeen++;
	pthread_mutex_unlock(&sLock);
	if (sCandidatesSeen <= 200 || sCandidatesSeen % 100 == 0) {
		note("%s at %p (%s)", kind == KIND_SETUP ? "a picture setup buffer"
			: "a method list", address, where);
	}
}


static void
write_file(int kind, const uint8_t *data, size_t size, const void *address,
	const char *where)
{
	if (sFiles >= sMaxFiles)
		return;
	char name[512];
	int number = sFiles++;
	snprintf(name, sizeof(name), "%s/%s-%04d.bin", sOut,
		kind == KIND_SETUP ? "setup" : "methods", number);
	int file = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (file < 0) {
		note("cannot write %s: %s", name, strerror(errno));
		return;
	}
	if (write(file, data, size) != (ssize_t)size)
		note("short write to %s", name);
	close(file);
	if (kind == KIND_SETUP)
		sSetups++;
	else
		sMethodLists++;
	if (sIndex != NULL) {
		const uint32_t *words = (const uint32_t *)data;
		fprintf(sIndex, "%s-%04d.bin %llu ms %p %zu bytes [%s] %08x %08x %08x %08x"
			" %08x %08x %08x %08x\n", kind == KIND_SETUP ? "setup" : "methods",
			number, (unsigned long long)(now_ms() - sStart), address, size, where,
			words[0], words[1], words[2], words[3], words[4], words[5], words[6],
			words[7]);
		fflush(sIndex);
	}
}


static int
is_ours(uintptr_t address)
{
	if (address >= (uintptr_t)sBuffers && address < (uintptr_t)sBuffers + sBuffersSize)
		return 1;
	for (int i = 0; i < sSelfCount; i++) {
		if (address >= sSelf[i][0] && address < sSelf[i][1])
			return 1;
	}
	return 0;
}


static int
find_self(struct dl_phdr_info *info, size_t size, void *data)
{
	(void)size;
	uintptr_t self = (uintptr_t)data;
	int mine = 0;
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *header = &info->dlpi_phdr[i];
		uintptr_t start = info->dlpi_addr + header->p_vaddr;
		if (header->p_type == PT_LOAD && self >= start && self < start + header->p_memsz)
			mine = 1;
	}
	if (!mine)
		return 0;
	for (int i = 0; i < info->dlpi_phnum && sSelfCount < 8; i++) {
		const ElfW(Phdr) *header = &info->dlpi_phdr[i];
		if (header->p_type != PT_LOAD)
			continue;
		uintptr_t start = info->dlpi_addr + header->p_vaddr;
		sSelf[sSelfCount][0] = start & ~(uintptr_t)4095;
		sSelf[sSelfCount][1] = (start + header->p_memsz + 4095) & ~(uintptr_t)4095;
		sSelfCount++;
	}
	return 1;
}


typedef struct {
	uintptr_t	start, end;
	unsigned long	offset;
	char		path[256];
	int		device;
} Mapping;


/* Reads the mappings worth looking at; returns how many. */
static int
read_mappings(Mapping *mappings, int capacity, int all, uint64_t *deviceHash)
{
	FILE *maps = fopen("/proc/self/maps", "r");
	if (maps == NULL)
		return 0;
	char line[512];
	int count = 0;
	uint64_t devices = 1469598103934665603ull;
	while (fgets(line, sizeof(line), maps) != NULL && count < capacity) {
		unsigned long start, end, offset;
		char permissions[8], path[256] = "";
		if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %255[^\n]", &start, &end,
				permissions, &offset, path) < 4 || permissions[0] != 'r')
			continue;
		int device = strncmp(path, "/dev/nvidia", 11) == 0;
		if (device && strncmp(path, "/dev/nvidia-uvm", 15) == 0 && !sUvm)
			continue;
		/* plain memory, in case the driver builds a buffer there first; not
		 * files, stacks or the kernel's pages */
		int plain = path[0] == '\0' || strcmp(path, "[heap]") == 0
			|| strncmp(path, "[anon:", 6) == 0 || strncmp(path, "/memfd:", 7) == 0
			|| strncmp(path, "/dev/zero", 9) == 0 || strncmp(path, "/dev/shm/", 9) == 0
			|| strncmp(path, "/SYSV", 5) == 0;
		if (device)
			devices = hash((const uint8_t *)line, strlen(line)) ^ (devices * 31);
		if (!device && !(plain && all))
			continue;
		if (end - start > (device ? DEVICE_LIMIT : PLAIN_LIMIT))
			continue;
		Mapping *mapping = &mappings[count++];
		mapping->start = start;
		mapping->end = end;
		mapping->offset = offset;
		mapping->device = device;
		snprintf(mapping->path, sizeof(mapping->path), "%s", path[0] ? path : "anon");
	}
	fclose(maps);
	if (deviceHash != NULL)
		*deviceHash = devices;
	return count;
}


static void
save_maps(const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", sOut, name);
	int from = open("/proc/self/maps", O_RDONLY);
	int to = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (from >= 0 && to >= 0) {
		char buffer[8192];
		ssize_t got;
		while ((got = read(from, buffer, sizeof(buffer))) > 0) {
			if (write(to, buffer, got) != got)
				break;
		}
	}
	if (from >= 0)
		close(from);
	if (to >= 0)
		close(to);
}


static void
scan_mapping(const Mapping *mapping, uint8_t *buffer)
{
	/* in chunks: reading the card's memory through a write-combined
	 * mapping a word at a time would take seconds */
	const size_t overlap = 64 * 4;
	for (uintptr_t at = mapping->start; at < mapping->end; at += CHUNK) {
		size_t size = mapping->end - at;
		if (size > CHUNK + overlap)
			size = CHUNK + overlap;
		if (!safe_copy(buffer, (const void *)at, size))
			return;
		size_t limit = size < CHUNK ? size : CHUNK;
		/* the setup buffer is aligned to 256 bytes in the driver's memory;
		 * anywhere a word may start in plain memory */
		size_t step = mapping->device ? 256 : 4;
		for (size_t offset = 0; offset + 4 <= limit; offset += step) {
			uint32_t word = *(const uint32_t *)(buffer + offset);
			if (!is_magic(word))
				continue;
			uintptr_t address = at + offset;
			if (is_ours(address))
				continue;
			note_class(word);
			size_t copy = mapping->end - address;
			if (copy > SETUP_SIZE)
				copy = SETUP_SIZE;
			if (copy < WATCH_SETUP)
				continue;
			char where[96];
			snprintf(where, sizeof(where), "%.60s+%#lx", mapping->path,
				mapping->offset + (unsigned long)(address - mapping->start));
			add_candidate((const uint8_t *)address, copy, KIND_SETUP, where);
		}
		for (size_t offset = 0; offset + 4 <= limit && offset + overlap <= size;
				offset += 4) {
			if (!is_method_list((const uint32_t *)(buffer + offset), !mapping->device))
				continue;
			uintptr_t address = at + offset;
			if (is_ours(address))
				continue;
			size_t copy = mapping->end - address;
			if (copy > METHODS_SIZE)
				copy = METHODS_SIZE;
			char where[96];
			snprintf(where, sizeof(where), "%.60s+%#lx", mapping->path,
				mapping->offset + (unsigned long)(address - mapping->start));
			add_candidate((const uint8_t *)address, copy, KIND_METHODS, where);
		}
	}
}


static void
raw_dump(uint64_t time, uint8_t *buffer)
{
	static Mapping mappings[1024];
	int count = read_mappings(mappings, 1024, 0, NULL);
	char directory[512];
	snprintf(directory, sizeof(directory), "%s/raw", sOut);
	mkdir(directory, 0755);
	char name[1024];
	snprintf(name, sizeof(name), "raw/maps-%llums.txt", (unsigned long long)time);
	save_maps(name);
	for (int i = 0; i < count; i++) {
		const Mapping *mapping = &mappings[i];
		size_t size = mapping->end - mapping->start;
		if (size > RAW_MAPPING_LIMIT || size > sRawBudget)
			continue;
		const char *base = strrchr(mapping->path, '/');
		snprintf(name, sizeof(name), "%s/%llums-%lx-%s-%lx.bin", directory,
			(unsigned long long)time, (unsigned long)mapping->start,
			base != NULL ? base + 1 : mapping->path, mapping->offset);
		int file = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (file < 0)
			continue;
		size_t done = 0;
		for (uintptr_t at = mapping->start; at < mapping->end; at += CHUNK) {
			size_t chunk = mapping->end - at < CHUNK ? mapping->end - at : CHUNK;
			if (!safe_copy(buffer, (const void *)at, chunk))
				break;
			if (write(file, buffer, chunk) != (ssize_t)chunk)
				break;
			done += chunk;
		}
		close(file);
		sRawBudget -= done;
		sRawFiles++;
	}
	note("raw copies of the driver's mappings at %llu ms (%d so far)",
		(unsigned long long)time, sRawFiles);
}


static void
find_candidates(int all, uint8_t *buffer)
{
	static Mapping mappings[4096];
	static uint64_t lastDevices;
	uint64_t devices = 0;
	keep_handlers();
	int count = read_mappings(mappings, 4096, all, &devices);
	if (devices != lastDevices) {
		lastDevices = devices;
		char name[64];
		snprintf(name, sizeof(name), "maps-%06llums.txt",
			(unsigned long long)(now_ms() - sStart));
		save_maps(name);
	}
	for (int i = 0; i < count; i++)
		scan_mapping(&mappings[i], buffer);
	sFinds++;
}


/* Copies a candidate once it holds still (two reads alike), so a buffer
 * the driver is in the middle of writing is not taken half old. */
static int
stable_copy(const Candidate *candidate, uint8_t *copy, uint8_t *again)
{
	for (int attempt = 0; attempt < 8; attempt++) {
		if (!safe_copy(copy, candidate->address, candidate->size))
			return -1;
		struct timespec pause = { 0, 100000 };
		nanosleep(&pause, NULL);
		if (!safe_copy(again, candidate->address, candidate->size))
			return -1;
		if (memcmp(copy, again, candidate->size) == 0)
			return 1;
	}
	return 0;
}


static void
watch_candidates(void)
{
	uint8_t *head = sBuffers, *copy = head + WATCH_SETUP, *again = copy + SETUP_SIZE;
	uint64_t time = now_ms();
	pthread_mutex_lock(&sLock);
	for (int i = 0; i < sCandidateCount; i++) {
		Candidate *candidate = &sCandidates[i];
		int setup = candidate->kind == KIND_SETUP;
		size_t watch = setup ? WATCH_SETUP : WATCH_METHODS;
		if (watch > candidate->size)
			watch = candidate->size;
		/* a method list sits in the push buffer ring until it is written
		 * over: once written out, it need not be watched for long */
		int gone = !setup && time - candidate->lastChange > METHODS_STALE;
		if (!gone)
			gone = !safe_copy(head, candidate->address, watch);
		if (!gone) {
			if (setup ? !is_magic(*(uint32_t *)head)
					: !is_method_list((uint32_t *)head, 0))
				continue;
			uint64_t now = hash(head, watch);
			/* the head is looked at every time; for a while after it
			 * changed the whole buffer is, as a driver may write the rest
			 * later */
			int settling = time - candidate->lastChange < SETTLE_MS;
			if (now == candidate->lastHash && !settling)
				continue;
			int stable = stable_copy(candidate, copy, again);
			if (stable < 0)
				gone = 1;
			else {
				uint64_t full = hash(copy, candidate->size);
				if (now == candidate->lastHash && full == candidate->lastFullHash)
					continue;
				if (hash(copy, watch) != candidate->lastHash)
					candidate->lastChange = now_ms();
				candidate->lastHash = hash(copy, watch);
				candidate->lastFullHash = full;
				if (setup ? !is_magic(*(uint32_t *)copy)
						: !is_method_list((uint32_t *)copy, 0))
					continue;
				if (remember(full)) {
					char where[128];
					snprintf(where, sizeof(where), "%s%s", candidate->where,
						stable ? "" : " (changing)");
					write_file(candidate->kind, copy, candidate->size, candidate->address,
						where);
				}
			}
		}
		if (gone) {
			*candidate = sCandidates[--sCandidateCount];
			i--;
		}
	}
	pthread_mutex_unlock(&sLock);
}


static void *
finder(void *unused)
{
	(void)unused;
	uint8_t *buffer = sBuffers + WATCH_SETUP + 2 * SETUP_SIZE;
	int rawNext = 0;
	for (int tick = 0;; tick++) {
		uint64_t elapsed = now_ms() - sStart;
		while (rawNext < sRawTimeCount && elapsed >= sRawTimes[rawNext])
			raw_dump(sRawTimes[rawNext++], buffer);
		/* the driver's mappings every 200 ms, all memory every second */
		find_candidates(tick % 5 == 0, buffer);
		struct timespec pause = { 0, 200000000 };
		nanosleep(&pause, NULL);
	}
	return NULL;
}


static void *
watcher(void *unused)
{
	(void)unused;
	struct timespec pause = { sPollMicros / 1000000, (sPollMicros % 1000000) * 1000 };
	for (;;) {
		watch_candidates();
		nanosleep(&pause, NULL);
	}
	return NULL;
}


__attribute__((destructor)) static void
stop(void)
{
	if (sOut == NULL)
		return;
	char path[512];
	snprintf(path, sizeof(path), "%s/summary.txt", sOut);
	if (access(path, F_OK) == 0)
		snprintf(path, sizeof(path), "%s/summary-%d.txt", sOut, (int)getpid());
	FILE *summary = fopen(path, "w");
	if (summary == NULL)
		return;
	fprintf(summary, "process %d\ntime %llu ms\nsetup buffers written %d\nmethod lists written %d\n"
		"candidates seen %d (replaced %d)\nfinds %d\nfaults skipped %d\nraw files %d\n"
		"magic looked for %#x%s\n", (int)getpid(), (unsigned long long)(now_ms() - sStart),
		sSetups, sMethodLists, sCandidatesSeen, sReplaced, sFinds, sFaults, sRawFiles,
		sMagic, sMagic == 0 ? " (any NVENC class)" : "");
	for (int i = 0; i < 8 && sClassesSeen[i] != 0; i++)
		fprintf(summary, "magic seen %#x\n", sClassesSeen[i]);
	fclose(summary);
	note("%d setup buffers, %d method lists in %s", sSetups, sMethodLists, sOut);
}


__attribute__((constructor)) static void
start(void)
{
	const char *out = getenv("NVSETUP_OUT");
	if (out == NULL || out[0] == '\0')
		return;
	sStart = now_ms();
	const char *class = getenv("NVSETUP_CLASS");
	if (class != NULL && class[0] != '\0' && strcmp(class, "any") != 0)
		sMagic = ((uint32_t)strtoul(class, NULL, 16) << 16) | 0x0006;
	const char *value = getenv("NVSETUP_UVM");
	sUvm = value != NULL && value[0] == '1';
	if ((value = getenv("NVSETUP_MAX")) != NULL && atoi(value) > 0)
		sMaxFiles = atoi(value);
	if ((value = getenv("NVSETUP_POLL_US")) != NULL && atol(value) >= 50)
		sPollMicros = atol(value);
	if ((value = getenv("NVSETUP_RAW_MB")) != NULL)
		sRawBudget = (uint64_t)atoll(value) << 20;
	if ((value = getenv("NVSETUP_RAW")) != NULL) {
		char *end;
		while (*value != '\0' && sRawTimeCount < MAX_RAW_TIMES) {
			unsigned long long time = strtoull(value, &end, 10);
			if (end == value)
				break;
			sRawTimes[sRawTimeCount++] = time;
			value = *end == ',' ? end + 1 : end;
		}
	}
	mkdir(out, 0755);
	sOut = out;
	/* another process wrote here before: number on after its files */
	DIR *directory = opendir(out);
	if (directory != NULL) {
		struct dirent *entry;
		int number;
		while ((entry = readdir(directory)) != NULL) {
			if ((sscanf(entry->d_name, "setup-%d.bin", &number) == 1
					|| sscanf(entry->d_name, "methods-%d.bin", &number) == 1)
				&& number >= sFiles)
				sFiles = number + 1;
		}
		closedir(directory);
		sMaxFiles += sFiles;
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/index.txt", sOut);
	sIndex = fopen(path, "a");

	/* the scanner's buffers and its threads' stacks, in one place that is
	 * left out of the search (copies of the magic are all over them) */
	size_t buffers = (WATCH_SETUP + 2 * SETUP_SIZE + CHUNK + 4096 + 4095) & ~(size_t)4095;
	sBuffersSize = buffers + 2 * STACK_SIZE;
	sBuffers = mmap(NULL, sBuffersSize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (sBuffers == MAP_FAILED) {
		note("no memory");
		sOut = NULL;
		return;
	}
	dl_iterate_phdr(find_self, (void *)(uintptr_t)&find_self);
	keep_handlers();
	pthread_attr_t attributes;
	pthread_attr_init(&attributes);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	/* the scanner threads must not take signals meant for the program */
	sigset_t all, old;
	sigfillset(&all);
	sigdelset(&all, SIGSEGV);
	sigdelset(&all, SIGBUS);
	pthread_sigmask(SIG_BLOCK, &all, &old);
	pthread_t thread;
	pthread_attr_setstack(&attributes, sBuffers + buffers, STACK_SIZE);
	int failed = pthread_create(&thread, &attributes, finder, NULL);
	pthread_attr_setstack(&attributes, sBuffers + buffers + STACK_SIZE, STACK_SIZE);
	failed |= pthread_create(&thread, &attributes, watcher, NULL);
	if (failed != 0)
		note("cannot start the scanner threads");
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	pthread_attr_destroy(&attributes);
	if (sMagic != 0)
		note("looking for %#x, writing to %s", sMagic, sOut);
	else
		note("looking for any NVENC class's magic (0x??b70006), writing to %s", sOut);
}
