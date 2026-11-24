/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	/dev/misc/rpi_hevc: the HEVC decoder block of the BCM2711 (Pi 4) and
	the BCM2712 (Pi 5) for the program that parses the stream (the rpi_hevc
	media add-on); see <rpi_hevc.h>. On another board no device is found
	and every open fails.

	What the block's registers mean was taken from the Linux drivers of
	Raspberry Pi Ltd: drivers/staging/media/rpivid (Pi 4) and
	drivers/media/platform/raspberrypi/hevc_dec (Pi 5). Both program the
	two phases with the same registers, the same interrupt control and the
	same firmware clock; the Pi 5's block differs in where it is:
	- its registers and interrupt control are above 64 GB
	  (/axi/codec@800000), and it sees the ARM's physical addresses 1:1,
	  as the Pi 4's does;
	- it sits behind one of the BCM2712's IOMMUs ("iommus"), which it
	  shares with the camera's ISP back end. Linux turns that IOMMU on;
	  nothing here does, and the block is only used with it off (all
	  addresses pass unchanged).

	Phase one runs a list of register writes the program makes (#202). The
	driver copies it into a buffer of its own (sCommands) and checks the
	copy (rpi_hevc_commands.h) before the block reads it: the program keeps
	its own buffer mapped and could change the list under the block, and
	the list names physical addresses and the rows the block writes.

	Neither Linux driver stops or resets the block: they wait for its
	interrupts without a time limit. Its device tree node names no reset
	line or power domain, and the SoCs' reset controller (bcm2835-power)
	resets V3D, H264 and ISP only. So after a phase did not end in time,
	nothing here can stop the block, and its buffers are kept. With
	"timeout_recovery true" in the driver settings, the block's activity
	lines (ACTIVE1/2_STATUS of its interrupt control; the interrupts are
	their falling edges) tell when such a phase has ended after all: its
	buffers are released then, and until then no phase of that kind is
	started again. That is off by default until a Pi has shown that the
	lines drop when a phase ends. */


#include <ByteOrder.h>
#include <Drivers.h>
#include <KernelExport.h>
#include <driver_settings.h>

#include <new>
#include <stdlib.h>
#include <string.h>

#include <bus/FDT.h>
#include <device_manager.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <rpi_firmware.h>
#include <rpi_hevc.h>

#include "rpi_hevc_commands.h"
#include "rpi_hevc_geometry.h"


//#define TRACE_HEVC
#ifdef TRACE_HEVC
#	define TRACE(x...)	dprintf("rpi_hevc: " x)
#else
#	define TRACE(x...)	do {} while (false)
#endif
#define ERROR(x...)		dprintf("rpi_hevc: " x)

#define CACHE_LINE_SIZE			64
#define PHASE_TIMEOUT			2000000LL

// phase one
#define REG_STATUS				56
#define REG_VERSION				60
#define REG_PU_WRITE_BASE		80
#define REG_PU_WRITE_STRIDE		84
#define REG_COEFF_WRITE_BASE	88
#define REG_COEFF_WRITE_STRIDE	92
#define REG_COMMAND_BASE		108
#define REG_COMMAND_COUNT		112
#define REG_COMMAND_STATUS		116

// phase two
#define REG_PU_READ_BASE		0x8000
#define REG_PU_READ_STRIDE		0x8004
#define REG_COEFF_READ_BASE		0x8008
#define REG_COEFF_READ_STRIDE	0x800c
#define REG_ROWS				0x8010
#define REG_CONFIG2				0x8014
#define REG_OUT_Y_BASE			0x8018
#define REG_OUT_Y_STRIDE		0x801c
#define REG_OUT_C_BASE			0x8020
#define REG_OUT_C_STRIDE		0x8024
#define REG_FRAME_SIZE			0x802c
#define REG_MV_BASE				0x8030
#define REG_MV_STRIDE			0x8034
#define REG_COL_BASE			0x8038
#define REG_COL_STRIDE			0x803c
#define REG_CURRENT_POC			0x8040
#define REG_REFERENCE(i)		(0x9000 + 16 * (i))

// the block's interrupt control
#define INT_ACTIVE1				(1 << 0)
#define INT_ACTIVE1_ENABLE		(1 << 2)
#define INT_ACTIVE1_STATUS		(1 << 3)	// the phase is running
#define INT_ACTIVE2				(1 << 4)
#define INT_ACTIVE2_ENABLE		(1 << 6)
#define INT_ACTIVE2_STATUS		(1 << 7)
#define INT_H264				(1 << 25)
#define INT_VP9					(1u << 29)
#define INT_ALL			(INT_ACTIVE1 | INT_ACTIVE2 | INT_H264 | INT_VP9)
#define INT_WRITE_ZERO			((0xff << 12) | (1 << 11))

// the BCM2712's IOMMU in front of the block (Linux: bcm2712-iommu.c)
#define IOMMU_CTRL				0x00
#define IOMMU_CTRL_ENABLE		(1 << 0)
#define IOMMU_DEBUG_INFO		0x38


struct hevc_buffer {
	area_id		area;
	uint8*		address;
	phys_addr_t	physical;
	size_t		size;
};

struct hevc_client {
	mutex		lock;
	hevc_buffer	buffers[RPI_HEVC_MAX_BUFFERS];
	int32		busy;
		// phases under way: buffers stay until they are through
	int32		lost;
		// of those, phases that did not end in time: the block may still
		// write to the buffers, which therefore stay for good (or, with
		// timeout recovery, until the block is seen idle after lostEpoch)
	uint32		lostEpoch;
};

// what the block may still use after its phase did not end
struct hevc_kept_area {
	area_id			area;
	size_t			size;
	hevc_kept_area*	next;
};

enum {
	PHASE1_HUNG = 1 << 0,
	PHASE2_HUNG = 1 << 1
};


int32 api_version = B_CUR_DRIVER_API_VERSION;

static const char* sDeviceNames[] = { "misc/rpi_hevc", NULL };

static device_manager_info* sDeviceManager;
static rpi_firmware_module_info* sFirmware;

static area_id sRegistersArea = -1;
static volatile uint8* sRegisters;
static area_id sInterruptArea = -1;
static volatile uint32* sInterruptControl;
static uint32 sInterrupt;
static bool sInterruptInstalled;
static area_id sIommuArea = -1;
static volatile uint32* sIommu;
static bool sPoweredBefore;

static mutex sPhase1Lock = MUTEX_INITIALIZER("rpi_hevc phase 1");
static mutex sPhase2Lock = MUTEX_INITIALIZER("rpi_hevc phase 2");
static mutex sOpenLock = MUTEX_INITIALIZER("rpi_hevc open");
static sem_id sPhase1Sem = -1;
static sem_id sPhase2Sem = -1;
static int32 sOpenCount;

// phase one's command list as the block reads it (sPhase1Lock)
static area_id sCommandArea = -1;
static rpi_hevc_command* sCommands;
static phys_addr_t sCommandsPhysical;
static size_t sCommandsSize;

// after a phase that did not end in time (sStateLock)
static bool sTimeoutRecovery;
static mutex sStateLock = MUTEX_INITIALIZER("rpi_hevc state");
static uint32 sHung;
	// PHASE1_HUNG, PHASE2_HUNG: running past its time, as far as known
static uint32 sEpoch;
	// counts the times the block was seen idle again after a hang
static hevc_kept_area* sKeptAreas;


static inline uint32
read_reg(uint32 offset)
{
	return *(volatile uint32*)(sRegisters + offset);
}


static inline void
write_reg(uint32 offset, uint32 value)
{
	*(volatile uint32*)(sRegisters + offset) = value;
}


static inline void
barrier()
{
#ifdef __aarch64__
	asm volatile("dsb sy" : : : "memory");
#else
	memory_full_barrier();
#endif
}


/*!	Writes the CPU cache's content of the range to memory and drops it: the
	decoder reads and writes the memory behind the cache's back.
*/
static void
flush_cache(void* address, size_t size)
{
#ifdef __aarch64__
	addr_t end = (addr_t)address + size;
	for (addr_t line = (addr_t)address & ~(addr_t)(CACHE_LINE_SIZE - 1);
			line < end; line += CACHE_LINE_SIZE) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	asm volatile("dsb sy" : : : "memory");
#endif
}


static int32
hevc_interrupt(void* data)
{
	uint32 control = *sInterruptControl;
	if ((control & INT_ALL) == 0)
		return B_UNHANDLED_INTERRUPT;

	// writing a pending bit back clears it
	*sInterruptControl = control & ~INT_WRITE_ZERO;
	barrier();

	if ((control & INT_ACTIVE2) != 0)
		release_sem_etc(sPhase2Sem, 1, B_DO_NOT_RESCHEDULE);
	if ((control & INT_ACTIVE1) != 0)
		release_sem_etc(sPhase1Sem, 1, B_DO_NOT_RESCHEDULE);
	return B_INVOKE_SCHEDULER;
}


//	#pragma mark - buffers


static hevc_buffer*
buffer_for(hevc_client* client, uint32 index)
{
	if (index >= RPI_HEVC_MAX_BUFFERS || client->buffers[index].area < 0)
		return NULL;
	return &client->buffers[index];
}


static inline uint32
block_address(const hevc_buffer* buffer, uint32 offset = 0)
{
	// the block takes addresses in units of 64 bytes
	return (uint32)((buffer->physical + offset) >> 6);
}


static inline uint32
block_length(uint32 bytes)
{
	return (bytes + 63) >> 6;
}


static void
free_buffer(hevc_buffer& buffer)
{
	if (buffer.area >= 0)
		delete_area(buffer.area);
	buffer.area = -1;
	buffer.address = NULL;
	buffer.physical = 0;
	buffer.size = 0;
}


//	#pragma mark - after a phase that did not end


/*!	With sStateLock held, and the block's clock on: whether the phases that
	ran past their time have ended since, by the block's activity lines.
	Once neither is running any more, what was kept for them is freed.
*/
static void
check_hung_locked()
{
	if (sHung == 0)
		return;

	uint32 control = *sInterruptControl;
	uint32 hung = sHung;
	if ((control & INT_ACTIVE1_STATUS) == 0)
		hung &= ~PHASE1_HUNG;
	if ((control & INT_ACTIVE2_STATUS) == 0)
		hung &= ~PHASE2_HUNG;
	if (hung == sHung)
		return;

	sHung = hung;
	if (hung != 0)
		return;

	// Every phase that ran past its time has ended now.
	sEpoch++;
	size_t size = 0;
	while (sKeptAreas != NULL) {
		hevc_kept_area* kept = sKeptAreas;
		sKeptAreas = kept->next;
		size += kept->size;
		delete_area(kept->area);
		free(kept);
	}
	dprintf("rpi_hevc: the block is idle again, %" B_PRIuSIZE " KiB of "
		"buffers freed\n", size / 1024);
}


/*!	The phase of \a which did not end in time, and the block still says it
	is running it (or nothing tells): from now on, nothing it may use is
	freed until it is seen idle. Returns the epoch of the loss.
*/
static uint32
phase_lost(uint32 which)
{
	MutexLocker locker(sStateLock);
	if (sTimeoutRecovery)
		sHung |= which;
	return sEpoch;
}


/*!	After the phase of \a which did not end in time: whether it is over
	all the same. Only the block's activity line can tell; without timeout
	recovery it is not asked.
*/
static bool
phase_ended_late(uint32 which)
{
	if (!sTimeoutRecovery)
		return false;
	uint32 control = *sInterruptControl;
	return (control & (which == PHASE1_HUNG
		? INT_ACTIVE1_STATUS : INT_ACTIVE2_STATUS)) == 0;
}


/*!	Before a phase of \a which starts, with its lock held: whether the
	block is still at one that did not end.
*/
static bool
phase_hung(uint32 which)
{
	MutexLocker locker(sStateLock);
	check_hung_locked();
	return (sHung & which) != 0;
}


/*!	With the client's lock held: its phases that did not end are over if
	the block was seen idle after the last of them.
*/
static void
settle_lost(hevc_client* client)
{
	if (client->lost == 0 || !sTimeoutRecovery)
		return;

	MutexLocker locker(sStateLock);
	check_hung_locked();
	if (sEpoch != client->lostEpoch) {
		client->busy -= client->lost;
		client->lost = 0;
	}
}


/*!	Keeps \a area until the block is idle (with timeout recovery), or for
	good.
*/
static void
keep_area(area_id area, size_t size)
{
	if (!sTimeoutRecovery)
		return;

	hevc_kept_area* kept = (hevc_kept_area*)malloc(sizeof(hevc_kept_area));
	if (kept == NULL)
		return;
	kept->area = area;
	kept->size = size;

	MutexLocker locker(sStateLock);
	kept->next = sKeptAreas;
	sKeptAreas = kept;
}


static status_t
allocate_buffer(hevc_client* client, rpi_hevc_allocate& request)
{
	if (request.size == 0 || request.size > 256 * 1024 * 1024)
		return B_BAD_VALUE;
	size_t size = ROUNDUP(request.size, B_PAGE_SIZE);

	MutexLocker locker(client->lock);

	uint32 index = 0;
	while (index < RPI_HEVC_MAX_BUFFERS && client->buffers[index].area >= 0)
		index++;
	if (index == RPI_HEVC_MAX_BUFFERS)
		return B_NO_MEMORY;

	hevc_buffer& buffer = client->buffers[index];
	void* address;
	buffer.area = create_area("rpi_hevc buffer", &address,
		B_ANY_KERNEL_ADDRESS, size, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (buffer.area < 0) {
		status_t status = buffer.area;
		buffer.area = -1;
		return status;
	}

	physical_entry entry;
	status_t status = get_memory_map(address, size, &entry, 1);
	if (status != B_OK) {
		free_buffer(buffer);
		return status;
	}
	buffer.address = (uint8*)address;
	buffer.physical = entry.address;
	buffer.size = size;

	// nothing of it in the cache when the decoder first writes there
	flush_cache(address, size);

	request.size = size;
	request.buffer = index;
	request.area = buffer.area;
	request.physical = buffer.physical;
	return B_OK;
}


//	#pragma mark - the phases


/*!	With sPhase1Lock held: copies the command list out of \a commands into
	sCommands, where nobody else can change it, and checks the copy.
*/
static status_t
copy_commands(const hevc_buffer* commands, const hevc_buffer* bitstream,
	const hevc_buffer* pu, const hevc_buffer* coeff,
	const rpi_hevc_phase1& request)
{
	size_t size = request.command_count * sizeof(rpi_hevc_command);
	if (sCommandArea < 0 || sCommandsSize < size) {
		if (sCommandArea >= 0)
			delete_area(sCommandArea);
		sCommandArea = -1;

		size_t areaSize = 64 * 1024;
		while (areaSize < size)
			areaSize *= 2;
		void* address;
		area_id area = create_area("rpi_hevc commands", &address,
			B_ANY_KERNEL_ADDRESS, areaSize, B_CONTIGUOUS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (area < 0)
			return area;
		physical_entry entry;
		status_t status = get_memory_map(address, areaSize, &entry, 1);
		if (status != B_OK) {
			delete_area(area);
			return status;
		}
		sCommandArea = area;
		sCommands = (rpi_hevc_command*)address;
		sCommandsPhysical = entry.address;
		sCommandsSize = areaSize;
	}

	memcpy(sCommands, commands->address, size);

	hevc_phase1_check check;
	hevc_phase1_needs needs;
	if (check_phase1_commands(sCommands, request.command_count,
			bitstream->physical, request.bitstream_size, check) != B_OK) {
		ERROR("phase 1: command %" B_PRIu32 " of %" B_PRIu32 " (%#" B_PRIx32
			" = %#" B_PRIx32 ") refused: %s\n", check.entry,
			request.command_count, sCommands[check.entry].address,
			sCommands[check.entry].data, check.reason);
		return B_BAD_VALUE;
	}
	if (get_phase1_needs(request, check.rows, needs) != B_OK
		|| pu->size < needs.pu || coeff->size < needs.coeff) {
		ERROR("phase 1: %" B_PRIu32 " rows do not fit the buffers\n",
			check.rows);
		return B_BAD_VALUE;
	}

	flush_cache(sCommands, size);
	return B_OK;
}


/*!	With sPhase1Lock held, after a phase one that did not end: the block
	may still read the list, which therefore stays where it is.
*/
static void
retire_commands()
{
	if (sCommandArea >= 0) {
		ERROR("keeping the command list (%" B_PRIuSIZE " KiB)\n",
			sCommandsSize / 1024);
		keep_area(sCommandArea, sCommandsSize);
	}
	sCommandArea = -1;
	sCommands = NULL;
	sCommandsSize = 0;
}


static status_t
phase1(hevc_client* client, rpi_hevc_phase1& request)
{
	MutexLocker clientLocker(client->lock);
	hevc_buffer* commands = buffer_for(client, request.commands);
	hevc_buffer* bitstream = buffer_for(client, request.bitstream);
	hevc_buffer* pu = buffer_for(client, request.pu);
	hevc_buffer* coeff = buffer_for(client, request.coeff);
	if (commands == NULL || bitstream == NULL || pu == NULL || coeff == NULL
		|| request.command_count == 0
		|| request.command_count > RPI_HEVC_MAX_COMMANDS
		|| (uint64)request.command_count * sizeof(rpi_hevc_command)
			> commands->size
		|| request.bitstream_size > bitstream->size) {
		return B_BAD_VALUE;
	}
	client->busy++;
	clientLocker.Unlock();

	MutexLocker locker(sPhase1Lock);

	status_t status = B_OK;
	if (phase_hung(PHASE1_HUNG))
		status = B_BUSY;
	else
		status = copy_commands(commands, bitstream, pu, coeff, request);
	if (status != B_OK) {
		locker.Unlock();
		clientLocker.Lock();
		client->busy--;
		return status;
	}

	flush_cache(bitstream->address, request.bitstream_size);

	// an interrupt nobody waited for
	while (acquire_sem_etc(sPhase1Sem, 1, B_RELATIVE_TIMEOUT, 0) == B_OK) {
	}

	write_reg(REG_PU_WRITE_BASE, block_address(pu));
	write_reg(REG_PU_WRITE_STRIDE, block_length(request.pu_stride));
	write_reg(REG_COEFF_WRITE_BASE, block_address(coeff));
	write_reg(REG_COEFF_WRITE_STRIDE, block_length(request.coeff_stride));
	write_reg(REG_COMMAND_COUNT, request.command_count);
	barrier();
	// this one starts the phase
	write_reg(REG_COMMAND_BASE, (uint32)(sCommandsPhysical >> 6));
	barrier();

	status = acquire_sem_etc(sPhase1Sem, 1, B_RELATIVE_TIMEOUT,
		PHASE_TIMEOUT);
	bool lost = false;
	uint32 epoch = 0;
	if (status != B_OK) {
		if (phase_ended_late(PHASE1_HUNG))
			ERROR("phase 1 ended without its interrupt\n");
		else {
			lost = true;
			epoch = phase_lost(PHASE1_HUNG);
			retire_commands();
		}
	}

	uint32 done = read_reg(REG_COMMAND_STATUS);
	uint32 count = read_reg(REG_COMMAND_COUNT);
	request.status = read_reg(REG_STATUS);
	if (status != B_OK) {
		ERROR("phase 1 did not end: %" B_PRIu32 " of %" B_PRIu32
			" commands, status %#" B_PRIx32 "\n", done, count,
			request.status);
		request.result = RPI_HEVC_PHASE1_ERROR;
	} else if (done == count)
		request.result = RPI_HEVC_PHASE1_OK;
	else {
		request.result = request.status
			& (RPI_HEVC_PHASE1_COEFF_FULL | RPI_HEVC_PHASE1_PU_FULL);
		if (request.result == 0)
			request.result = RPI_HEVC_PHASE1_ERROR;
		TRACE("phase 1: %" B_PRIu32 " of %" B_PRIu32 " commands, status %#"
			B_PRIx32 "\n", done, count, request.status);
	}
	locker.Unlock();

	clientLocker.Lock();
	if (lost) {
		client->lost++;
		client->lostEpoch = epoch;
	} else
		client->busy--;
	return status == B_TIMED_OUT ? B_OK : status;
}


static status_t
phase2(hevc_client* client, const rpi_hevc_phase2& request)
{
	MutexLocker clientLocker(client->lock);
	hevc_buffer* pu = buffer_for(client, request.pu);
	hevc_buffer* coeff = buffer_for(client, request.coeff);
	hevc_buffer* frame = buffer_for(client, request.frame);
	if (pu == NULL || coeff == NULL || frame == NULL)
		return B_BAD_VALUE;

	// The block writes the picture, and the motion vectors, wherever the
	// geometry takes it: a picture larger than its buffer went on into the
	// physical pages after it (#202).
	hevc_phase2_needs needs;
	if (get_phase2_needs(request, needs) != B_OK
		|| pu->size < needs.pu || coeff->size < needs.coeff
		|| frame->size < needs.picture) {
		return B_BAD_VALUE;
	}

	hevc_buffer* references[16];
	for (int32 i = 0; i < 16; i++) {
		// an address that is valid in any case
		references[i] = frame;
		if (request.references[i] == RPI_HEVC_NO_BUFFER)
			continue;
		references[i] = buffer_for(client, request.references[i]);
		if (references[i] == NULL || references[i]->size < needs.picture)
			return B_BAD_VALUE;
	}

	hevc_buffer* mv = NULL;
	hevc_buffer* collocated = NULL;
	if (request.mv != RPI_HEVC_NO_BUFFER) {
		mv = buffer_for(client, request.mv);
		if (mv == NULL || mv->size < needs.mv)
			return B_BAD_VALUE;
	}
	if (request.collocated != RPI_HEVC_NO_BUFFER) {
		collocated = buffer_for(client, request.collocated);
		if (collocated == NULL || collocated->size < needs.mv)
			return B_BAD_VALUE;
	}
	client->busy++;
	clientLocker.Unlock();

	MutexLocker locker(sPhase2Lock);

	if (phase_hung(PHASE2_HUNG)) {
		locker.Unlock();
		clientLocker.Lock();
		client->busy--;
		return B_BUSY;
	}

	while (acquire_sem_etc(sPhase2Sem, 1, B_RELATIVE_TIMEOUT, 0) == B_OK) {
	}

	write_reg(REG_PU_READ_BASE, block_address(pu));
	write_reg(REG_PU_READ_STRIDE, block_length(request.pu_stride));
	write_reg(REG_COEFF_READ_BASE, block_address(coeff));
	write_reg(REG_COEFF_READ_STRIDE, block_length(request.coeff_stride));

	write_reg(REG_OUT_Y_BASE, block_address(frame));
	write_reg(REG_OUT_C_BASE, block_address(frame, request.chroma_offset));
	write_reg(REG_OUT_Y_STRIDE, block_length(request.frame_stride));
	write_reg(REG_OUT_C_STRIDE, block_length(request.frame_stride));

	for (int32 i = 0; i < 16; i++) {
		write_reg(REG_REFERENCE(i), block_address(references[i]));
		write_reg(REG_REFERENCE(i) + 4, block_length(request.frame_stride));
		write_reg(REG_REFERENCE(i) + 8,
			block_address(references[i], request.chroma_offset));
		write_reg(REG_REFERENCE(i) + 12, block_length(request.frame_stride));
	}

	write_reg(REG_CONFIG2, request.config);
	write_reg(REG_FRAME_SIZE, request.frame_size);
	write_reg(REG_CURRENT_POC, request.current_poc);

	write_reg(REG_COL_STRIDE, block_length(request.mv_stride));
	write_reg(REG_MV_STRIDE, block_length(request.mv_stride));
	write_reg(REG_MV_BASE, mv != NULL ? block_address(mv) : 0);
	write_reg(REG_COL_BASE,
		collocated != NULL ? block_address(collocated) : 0);
	barrier();
	// this one starts the phase
	write_reg(REG_ROWS, request.rows);
	barrier();

	status_t status = acquire_sem_etc(sPhase2Sem, 1, B_RELATIVE_TIMEOUT,
		PHASE_TIMEOUT);
	bool lost = false;
	uint32 epoch = 0;
	if (status != B_OK) {
		ERROR("phase 2 did not end: %s\n", strerror(status));
		if (phase_ended_late(PHASE2_HUNG))
			ERROR("phase 2 ended without its interrupt\n");
		else {
			lost = true;
			epoch = phase_lost(PHASE2_HUNG);
		}
	}
	locker.Unlock();

	// what the cache has of the picture is the picture before
	flush_cache(frame->address, frame->size);

	clientLocker.Lock();
	if (lost) {
		client->lost++;
		client->lostEpoch = epoch;
	} else
		client->busy--;
	return status;
}


//	#pragma mark - device


/*!	The index of the "reg" entry that "reg-names" calls \a name, or
	\a fallback without "reg-names".
*/
static uint32
reg_index(fdt_device_module_info* fdt, fdt_device* device, const char* name,
	uint32 fallback)
{
	int length;
	const char* names = (const char*)fdt->get_prop(device, "reg-names",
		&length);
	if (names == NULL || length <= 0)
		return fallback;

	const char* end = names + length;
	for (uint32 index = 0; names < end; index++) {
		if (strncmp(names, name, end - names) == 0)
			return index;
		names += strnlen(names, end - names) + 1;
	}
	return fallback;
}


/*!	The registers of the IOMMU that the decoder's "iommus" names, or 0. */
static phys_addr_t
find_iommu(fdt_device_module_info* fdt, fdt_device* device)
{
	int length;
	const uint32* iommus = (const uint32*)fdt->get_prop(device, "iommus",
		&length);
	if (iommus == NULL || length < 4)
		return 0;

	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(fdt->get_bus(device),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK
		|| bus == NULL) {
		return 0;
	}
	device_node* node = busModule->node_by_phandle(bus,
		B_BENDIAN_TO_HOST_INT32(iommus[0]));
	fdt_device_module_info* iommuModule;
	fdt_device* iommuDevice;
	uint64 base, size;
	if (node == NULL || sDeviceManager->get_driver(node,
			(driver_module_info**)&iommuModule, (void**)&iommuDevice) != B_OK
		|| iommuDevice == NULL
		|| !iommuModule->get_reg(iommuDevice, 0, &base, &size)) {
		return 0;
	}
	return base;
}


static status_t
find_decoder(phys_addr_t* _registers, size_t* _size, phys_addr_t* _control,
	uint32* _interrupt, phys_addr_t* _iommu)
{
	device_node* root = sDeviceManager->get_root_node();
	if (root == NULL)
		return B_DEVICE_NOT_FOUND;

	static const char* const kCompatible[] = {
		"brcm,bcm2711-hevc-dec", "brcm,bcm2712-hevc-dec",
		"raspberrypi,hevc-dec"
	};

	device_node* node = NULL;
	for (size_t i = 0; node == NULL && i < B_COUNT_OF(kCompatible); i++) {
		device_attr attributes[] = {
			{ "fdt/compatible", B_STRING_TYPE,
				{ .string = kCompatible[i] } },
			{}
		};
		if (sDeviceManager->find_child_node(root, attributes, &node) != B_OK)
			node = NULL;
	}
	sDeviceManager->put_node(root);
	if (node == NULL)
		return B_DEVICE_NOT_FOUND;

	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(node,
		(driver_module_info**)&fdt, (void**)&device);
	if (status == B_OK) {
		uint64 base, size, control, controlSize, interrupt;
		if (fdt->get_reg(device, reg_index(fdt, device, "hevc", 0), &base,
				&size)
			&& fdt->get_reg(device, reg_index(fdt, device, "intc", 1),
				&control, &controlSize)
			&& fdt->get_interrupt(device, 0, NULL, &interrupt)) {
			*_registers = base;
			*_size = size;
			*_control = control;
			*_interrupt = interrupt;
			*_iommu = find_iommu(fdt, device);
		} else
			status = B_BAD_DATA;
	}

	sDeviceManager->put_node(node);
	return status;
}


/*!	Turns the IOMMU in front of the block off, if the firmware left it on:
	the block is given physical addresses. Nothing else in this system uses
	that IOMMU (the ISP back end has no driver).
*/
static void
bypass_iommu()
{
	if (sIommu == NULL)
		return;

	uint32 control = sIommu[IOMMU_CTRL / 4];
	if ((control & IOMMU_CTRL_ENABLE) == 0)
		return;

	ERROR("the IOMMU in front of the decoder is on (control %#" B_PRIx32
		"), turning it off\n", control);
	sIommu[IOMMU_CTRL / 4] = 0;
	barrier();
}


static void
power(bool on)
{
	uint32 maximum = 0;
	if (on) {
		bypass_iommu();
		if (sFirmware->get_clock_rate(RPI_FIRMWARE_CLOCK_HEVC, true, &maximum)
				== B_OK && maximum != 0) {
			sFirmware->set_clock_rate(RPI_FIRMWARE_CLOCK_HEVC, maximum);
		}
	}
	status_t status = sFirmware->set_clock_state(RPI_FIRMWARE_CLOCK_HEVC, on);
	if (status != B_OK)
		ERROR("turning the clock %s: %s\n", on ? "on" : "off",
			strerror(status));

	if (on && !sPoweredBefore) {
		// once, for the record: what the firmware made of it, and the
		// block's own version register (it answers with its clock on)
		sPoweredBefore = true;
		uint32 rate = 0;
		sFirmware->get_clock_rate(RPI_FIRMWARE_CLOCK_HEVC, false, &rate);
		dprintf("rpi_hevc: clock %" B_PRIu32 " Hz (maximum %" B_PRIu32
			"), version %#" B_PRIx32 "\n", rate, maximum,
			read_reg(REG_VERSION));
	}
}


static status_t
hevc_open(const char* name, uint32 flags, void** _cookie)
{
	if (sRegisters == NULL)
		return B_DEVICE_NOT_FOUND;

	hevc_client* client = new(std::nothrow) hevc_client;
	if (client == NULL)
		return B_NO_MEMORY;
	mutex_init(&client->lock, "rpi_hevc client");
	client->busy = 0;
	client->lost = 0;
	client->lostEpoch = 0;
	for (uint32 i = 0; i < RPI_HEVC_MAX_BUFFERS; i++) {
		client->buffers[i].area = -1;
		client->buffers[i].address = NULL;
		client->buffers[i].physical = 0;
		client->buffers[i].size = 0;
	}

	MutexLocker locker(sOpenLock);
	if (sOpenCount++ == 0) {
		power(true);
		// both phases' interrupts, and nothing pending
		*sInterruptControl = INT_ACTIVE1_ENABLE | INT_ACTIVE2_ENABLE;
		barrier();
		*sInterruptControl = *sInterruptControl & ~INT_WRITE_ZERO;
		barrier();
	}

	*_cookie = client;
	return B_OK;
}


static status_t
hevc_close(void* cookie)
{
	return B_OK;
}


static status_t
hevc_free(void* cookie)
{
	hevc_client* client = (hevc_client*)cookie;

	// A phase that is under way writes to the buffers until it ends.
	while (true) {
		MutexLocker locker(client->lock);
		if (client->busy == client->lost)
			break;
		locker.Unlock();
		snooze(10000);
	}
	// ... and one the caller did not wait for (it was killed)
	mutex_lock(&sPhase1Lock);
	mutex_unlock(&sPhase1Lock);
	mutex_lock(&sPhase2Lock);
	mutex_unlock(&sPhase2Lock);

	mutex_lock(&client->lock);
	settle_lost(client);
	mutex_unlock(&client->lock);

	if (client->lost == 0) {
		for (uint32 i = 0; i < RPI_HEVC_MAX_BUFFERS; i++)
			free_buffer(client->buffers[i]);
	} else {
		// Nothing stops a phase that did not end: its buffers are not given
		// back, lest the block go on writing into memory that has another
		// use by then (#202). With timeout recovery they go once the block
		// is seen idle.
		size_t size = 0;
		for (uint32 i = 0; i < RPI_HEVC_MAX_BUFFERS; i++) {
			if (client->buffers[i].area >= 0) {
				size += client->buffers[i].size;
				keep_area(client->buffers[i].area, client->buffers[i].size);
			}
		}
		ERROR("%" B_PRId32 " phases did not end: keeping %" B_PRIuSIZE
			" KiB of buffers\n", client->lost, size / 1024);
	}
	mutex_destroy(&client->lock);
	delete client;

	MutexLocker locker(sOpenLock);
	if (--sOpenCount == 0)
		power(false);
	return B_OK;
}


static status_t
hevc_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	hevc_client* client = (hevc_client*)cookie;

	switch (op) {
		case RPI_HEVC_ALLOCATE:
		{
			rpi_hevc_allocate request;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			status_t status = allocate_buffer(client, request);
			if (status != B_OK)
				return status;
			if (user_memcpy(buffer, &request, sizeof(request)) != B_OK) {
				MutexLocker locker(client->lock);
				free_buffer(client->buffers[request.buffer]);
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case RPI_HEVC_FREE:
		{
			uint32 index;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&index, buffer, sizeof(index)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			MutexLocker locker(client->lock);
			if (buffer_for(client, index) == NULL)
				return B_BAD_VALUE;
			settle_lost(client);
			if (client->busy != 0)
				return B_BUSY;
			free_buffer(client->buffers[index]);
			return B_OK;
		}

		case RPI_HEVC_PHASE1:
		{
			rpi_hevc_phase1 request;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			status_t status = phase1(client, request);
			if (status != B_OK)
				return status;
			return user_memcpy(buffer, &request, sizeof(request));
		}

		case RPI_HEVC_PHASE2:
		{
			rpi_hevc_phase2 request;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return phase2(client, request);
		}
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
hevc_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
hevc_write(void* cookie, off_t position, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sHooks = {
	hevc_open,
	hevc_close,
	hevc_free,
	hevc_control,
	hevc_read,
	hevc_write
};


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	status_t status = get_module(B_DEVICE_MANAGER_MODULE_NAME,
		(module_info**)&sDeviceManager);
	if (status != B_OK)
		return status;

	phys_addr_t registers, control, iommu;
	size_t size;
	status = find_decoder(&registers, &size, &control, &sInterrupt, &iommu);
	if (status != B_OK) {
		// another board: the device is there and cannot be opened
		return B_OK;
	}

	status = get_module(RPI_FIRMWARE_MODULE_NAME, (module_info**)&sFirmware);
	if (status != B_OK)
		return B_OK;

	void* settings = load_driver_settings("rpi_hevc");
	if (settings != NULL) {
		sTimeoutRecovery = get_driver_boolean_parameter(settings,
			"timeout_recovery", false, true);
		unload_driver_settings(settings);
	}

	sPhase1Sem = create_sem(0, "rpi_hevc phase 1");
	sPhase2Sem = create_sem(0, "rpi_hevc phase 2");

	void* address;
	sInterruptArea = map_physical_memory("rpi_hevc interrupt control",
		control & ~(phys_addr_t)(B_PAGE_SIZE - 1), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&address);
	if (sInterruptArea >= 0) {
		sInterruptControl = (volatile uint32*)((uint8*)address
			+ (control & (B_PAGE_SIZE - 1)));

		sRegistersArea = map_physical_memory("rpi_hevc registers", registers,
			ROUNDUP(size, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	}
	if (sPhase1Sem < 0 || sPhase2Sem < 0 || sInterruptArea < 0
		|| sRegistersArea < 0) {
		ERROR("no memory for the device\n");
		return B_OK;
	}

	if (iommu != 0) {
		void* iommuAddress;
		sIommuArea = map_physical_memory("rpi_hevc iommu",
			iommu & ~(phys_addr_t)(B_PAGE_SIZE - 1), B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			&iommuAddress);
		if (sIommuArea < 0) {
			ERROR("cannot map the IOMMU at %#" B_PRIxPHYSADDR "\n", iommu);
			return B_OK;
		}
		sIommu = (volatile uint32*)((uint8*)iommuAddress
			+ (iommu & (B_PAGE_SIZE - 1)));
		dprintf("rpi_hevc: IOMMU at %#" B_PRIxPHYSADDR ", control %#"
			B_PRIx32 ", debug info %#" B_PRIx32 "\n", iommu,
			sIommu[IOMMU_CTRL / 4], sIommu[IOMMU_DEBUG_INFO / 4]);
	}

	status = install_io_interrupt_handler(sInterrupt, hevc_interrupt, NULL,
		0);
	if (status != B_OK) {
		ERROR("interrupt %" B_PRIu32 ": %s\n", sInterrupt, strerror(status));
		return B_OK;
	}
	sInterruptInstalled = true;

	sRegisters = (volatile uint8*)address;
	dprintf("rpi_hevc: decoder at %#" B_PRIxPHYSADDR ", interrupt %" B_PRIu32
		"\n", registers, sInterrupt);
	return B_OK;
}


void
uninit_driver()
{
	if (sInterruptInstalled)
		remove_io_interrupt_handler(sInterrupt, hevc_interrupt, NULL);
	sInterruptInstalled = false;
	sRegisters = NULL;
	if (sRegistersArea >= 0)
		delete_area(sRegistersArea);
	if (sInterruptArea >= 0)
		delete_area(sInterruptArea);
	sRegistersArea = sInterruptArea = -1;
	sIommu = NULL;
	if (sIommuArea >= 0)
		delete_area(sIommuArea);
	sIommuArea = -1;
	// Nobody has the device open: the block does not read the list now,
	// unless a phase did not end (then it was retired).
	if (sCommandArea >= 0)
		delete_area(sCommandArea);
	sCommandArea = -1;
	sCommands = NULL;
	sCommandsSize = 0;
	// what a phase that did not end may still use stays
	while (sKeptAreas != NULL) {
		hevc_kept_area* kept = sKeptAreas;
		sKeptAreas = kept->next;
		free(kept);
	}

	if (sPhase1Sem >= 0)
		delete_sem(sPhase1Sem);
	if (sPhase2Sem >= 0)
		delete_sem(sPhase2Sem);
	sPhase1Sem = sPhase2Sem = -1;

	if (sFirmware != NULL)
		put_module(RPI_FIRMWARE_MODULE_NAME);
	sFirmware = NULL;
	put_module(B_DEVICE_MANAGER_MODULE_NAME);
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sHooks;
}
