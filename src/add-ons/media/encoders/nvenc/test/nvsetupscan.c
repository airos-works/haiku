/* nvsetupscan: see what NVIDIA's own driver tells NVENC, on Linux.
 *
 * Preloaded into a program that encodes with NVIDIA's driver (ffmpeg with
 * h264_nvenc or hevc_nvenc), it looks through the program's memory for the
 * two things this add-on has to get right and writes each distinct one it
 * sees to a file:
 *
 * - the driver picture setup buffer (SET_IN_DRV_PIC_SETUP), which starts
 *   with the magic number (class << 16) | 0x0006: setup-NNNN.bin, 4096 bytes
 *   from the magic on (picture, slice, motion search, mode decision and
 *   quantisation controls are all in there);
 * - the list of methods of a picture, which starts with SET_APPLICATION_ID
 *   (0x200) in the channel's push buffer: methods-NNNN.bin, 1024 bytes.
 *
 *   gcc -shared -fPIC -O2 -o nvsetupscan.so nvsetupscan.c -lpthread
 *   NVSETUP_OUT=/tmp/nvsetup NVSETUP_CLASS=0xc2b7 LD_PRELOAD=./nvsetupscan.so \
 *       ffmpeg -f lavfi -i testsrc2=s=1280x736:r=30 -frames:v 600 \
 *       -c:v hevc_nvenc -preset p4 -bf 0 -g 300 -f null -
 *
 * NVSETUP_CLASS is the card's encoder class (c2b7 for a GTX 1070; the
 * add-on's NVENC_DEBUG=1 prints it). Run it for H.265 without B pictures
 * (-bf 0), with them, and for H.264 with -bf 2. The add-on writes its own
 * setup buffers with NVENC_DUMP=<directory>, to compare.
 *
 * The buffers live in memory the driver maps from /dev/nvidia*, which
 * another process cannot read (/proc/<pid>/mem fails on such mappings), so
 * the looking is done from inside: a thread finds candidates every 200 ms
 * and then watches them every millisecond. Nothing is written to. */

#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_CANDIDATES	64
#define SETUP_SIZE	4096
#define METHODS_SIZE	1024
#define MAX_FILES	2000

typedef struct {
	const uint8_t	*address;
	size_t		size;		/* SETUP_SIZE or METHODS_SIZE */
	uint64_t	lastHash;
} Candidate;

static Candidate sCandidates[MAX_CANDIDATES];
static int sCandidateCount;
static uint32_t sMagic;
static const char *sOut;
static int sFiles;
/* volatile: the stores around a read that may fault must not be optimised away */
static __thread sigjmp_buf *volatile sRecover;
static struct sigaction sOldSegv, sOldBus;

static void
fault(int signal, siginfo_t *info, void *context)
{
	/* a mapping went away under the scanner: skip it */
	if (sRecover != NULL)
		siglongjmp(*sRecover, 1);
	struct sigaction *old = signal == SIGSEGV ? &sOldSegv : &sOldBus;
	if (old->sa_flags & SA_SIGINFO) {
		if (old->sa_sigaction != NULL)
			old->sa_sigaction(signal, info, context);
	} else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN)
		old->sa_handler(signal);
	else {
		sigaction(signal, old, NULL);
		raise(signal);
	}
}

static uint64_t
hash(const uint8_t *data, size_t size)
{
	uint64_t value = 1469598103934665603ull;
	for (size_t i = 0; i < size; i++)
		value = (value ^ data[i]) * 1099511628211ull;
	return value;
}

static void
add_candidate(const uint8_t *address, size_t size)
{
	for (int i = 0; i < sCandidateCount; i++) {
		if (sCandidates[i].address == address)
			return;
	}
	if (sCandidateCount == MAX_CANDIDATES)
		return;
	sCandidates[sCandidateCount].address = address;
	sCandidates[sCandidateCount].size = size;
	sCandidates[sCandidateCount].lastHash = 0;
	sCandidateCount++;
	fprintf(stderr, "nvsetupscan: %s at %p\n",
		size == SETUP_SIZE ? "a picture setup buffer" : "a method list", address);
}

/* A method list starts with SET_APPLICATION_ID: an incrementing method
 * header (2 << 28) for one word of method 0x200 on any subchannel, then the
 * application (1 H.264, 3 H.265, 7 AV1; older classes 2 for VP8). */
static int
is_method_list(const uint32_t *words)
{
	return (words[0] & 0xffff1fff) == 0x20010080 && words[1] >= 1 && words[1] <= 8
		&& (words[2] & 0xe0000000) == 0x20000000;
}

static void
scan_mapping(const uint8_t *start, const uint8_t *end, int device)
{
	sigjmp_buf recover;
	if (sigsetjmp(recover, 1) != 0) {
		sRecover = NULL;
		return;
	}
	sRecover = &recover;
	/* the setup buffer is aligned to 256 bytes, the method list to words */
	for (const uint8_t *at = start; at + SETUP_SIZE <= end; at += 256) {
		if (*(const uint32_t *)at == sMagic)
			add_candidate(at, SETUP_SIZE);
	}
	if (device) {
		for (const uint8_t *at = start; at + METHODS_SIZE <= end; at += 4) {
			if (is_method_list((const uint32_t *)at))
				add_candidate(at, METHODS_SIZE);
		}
	}
	sRecover = NULL;
}

static void
find_candidates(void)
{
	FILE *maps = fopen("/proc/self/maps", "r");
	if (maps == NULL)
		return;
	char line[512];
	while (fgets(line, sizeof(line), maps) != NULL) {
		unsigned long start, end;
		char permissions[8], path[256] = "";
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &start, &end, permissions,
				path) < 3 || permissions[0] != 'r')
			continue;
		/* the driver's mappings, and plain memory in case it hands the
		 * engine a buffer of its own; not files, stacks or the kernel's */
		int device = strncmp(path, "/dev/nvidia", 11) == 0;
		int plain = path[0] == '\0' || strcmp(path, "[heap]") == 0;
		if ((!device && !plain) || end - start > (1ul << 28))
			continue;
		scan_mapping((const uint8_t *)start, (const uint8_t *)end, device);
	}
	fclose(maps);
}

static void
watch_candidates(void)
{
	for (int i = 0; i < sCandidateCount; i++) {
		Candidate *candidate = &sCandidates[i];
		uint8_t copy[SETUP_SIZE];
		sigjmp_buf recover;
		if (sigsetjmp(recover, 1) != 0) {
			sRecover = NULL;
			*candidate = sCandidates[--sCandidateCount];
			i--;
			continue;
		}
		sRecover = &recover;
		memcpy(copy, candidate->address, candidate->size);
		sRecover = NULL;
		int setup = candidate->size == SETUP_SIZE;
		if (setup ? *(uint32_t *)copy != sMagic : !is_method_list((uint32_t *)copy))
			continue;
		uint64_t now = hash(copy, candidate->size);
		if (now == candidate->lastHash || sFiles >= MAX_FILES)
			continue;
		candidate->lastHash = now;
		char name[512];
		snprintf(name, sizeof(name), "%s/%s-%04d.bin", sOut, setup ? "setup" : "methods",
			sFiles++);
		int file = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (file >= 0) {
			if (write(file, copy, candidate->size) < 0)
				perror("nvsetupscan");
			close(file);
		}
	}
}

static void *
scanner(void *unused)
{
	(void)unused;
	struct timespec pause = { 0, 1000000 };
	for (int tick = 0;; tick++) {
		if (tick % 200 == 0)
			find_candidates();
		watch_candidates();
		nanosleep(&pause, NULL);
	}
	return NULL;
}

__attribute__((constructor)) static void
start(void)
{
	sOut = getenv("NVSETUP_OUT");
	if (sOut == NULL)
		return;
	const char *class = getenv("NVSETUP_CLASS");
	sMagic = ((uint32_t)strtoul(class != NULL ? class : "0xc2b7", NULL, 16) << 16) | 0x0006;
	mkdir(sOut, 0755);
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = fault;
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigaction(SIGSEGV, &action, &sOldSegv);
	sigaction(SIGBUS, &action, &sOldBus);
	pthread_t thread;
	pthread_create(&thread, NULL, scanner, NULL);
	pthread_detach(thread);
	fprintf(stderr, "nvsetupscan: looking for %#x, writing to %s\n", sMagic, sOut);
}
