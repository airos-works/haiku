/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2026, air/OS.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

// Polaris IH3 programming and vector format: Linux 6.18.52 tonga_ih.c,
// gfx_v8_0.c, gmc_v8_0.c and AMD's oss_3_0/bif_5_1 register headers (MIT).
#include "Interrupts.h"
#include <string.h>
#include <vm/vm.h>

status_t
GpuInterrupts::Initialize(volatile uint32* r, const amdgpu_info& info, pci_module_info* module)
{
	if (enabled) return Error();
	if (attempted) return RearmAfterReset();
	// Never take over an enabled firmware/foreign interrupt ring.
	if (module == NULL || (r[0xe30] & 0x20001) != 0) return B_NOT_ALLOWED;
	attempted = true; area = signal = -1;
	regs = r; pci = module;
	bus = info.bus; slot = info.slot; function = info.function;
	B_INITIALIZE_SPINLOCK(&lock);
	if (pci->get_msi_count(bus, slot, function) == 0) return B_NOT_SUPPORTED;
	virtual_address_restrictions va = {};
	physical_address_restrictions pa = {};
	pa.high_address = 1ULL << 40; pa.alignment = 4096;
	area = create_area_etc(B_SYSTEM_TEAM, "amdgpu IH ring", kAllocationBytes,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &va, &pa,
		(void**)&memory);
	status_t status = area < 0 ? area : B_OK;
	physical_entry physical = {};
	uint32 count = 1;
	if (status == B_OK)
		status = get_memory_map_etc(B_SYSTEM_TEAM, (void*)memory, kAllocationBytes, &physical, &count);
	if (status == B_OK && (physical.size != kAllocationBytes || (physical.address & 4095) != 0
		|| physical.address > (1ULL << 40) - kAllocationBytes)) status = B_BAD_ADDRESS;
	if (status == B_OK) {
		memset((void*)memory, 0, kAllocationBytes);
		signal = create_sem(0, "amdgpu graphics interrupt");
		if (signal < 0) status = signal;
	}
	if (status == B_OK) {
		status = pci->configure_msi(bus, slot, function, 1, &vector);
		configured = status == B_OK;
	}
	if (status == B_OK) {
		status = install_io_interrupt_handler(vector, Handle, this, 0);
		handler = status == B_OK;
	}
	if (status == B_OK) status = pci->enable_msi(bus, slot, function);
	if (status != B_OK) {
		Uninitialize();
		return status;
	}
	// Snooped physical system RAM, not a client GPU mapping. A private final
	// page supplies the dummy-read address; MSI normally disables that read.
	programmed = true;
	r[0x151b] = (physical.address + kRingBytes + 4096) >> 8;
	r[0x151a] &= ~9u;
	r[0xe31] = physical.address >> 8;
	const uint32 control = (14 << 1) | (1 << 8) | (1 << 21);
	r[0xe30] = control | 0x80000000;
	uint64 writeback = physical.address + kRingBytes;
	r[0xe35] = (uint32)writeback;
	r[0xe34] = writeback >> 32;
	r[0xe32] = r[0xe33] = 0;
	r[0xe42] &= ~0x10000000u; // MMIO RPTR; no doorbell allocation
	__sync_synchronize();
	cpu_status previous = disable_interrupts();
	acquire_spinlock(&lock);
	enabled = true;
	release_spinlock(&lock);
	restore_interrupts(previous);
	r[0xe30] = control | 0x20001;
	(void)r[0xe30];
	r[0x306a] |= kCPInterrupts;
	r[0x504] |= kVMInterrupts;
	r[0x505] |= kVMInterrupts;
	(void)r[0x505];
	dprintf("amdgpu: IH3 MSI vector %u ring %#" B_PRIx64 " bytes %u control %#x\n",
		(unsigned)vector, (uint64)physical.address, (unsigned)kRingBytes, (unsigned)r[0xe30]);
	return B_OK;
}

status_t
GpuInterrupts::SuspendForReset()
{
	if (!programmed || !configured || !handler || area < 0 || signal < 0)
		return B_DEV_NOT_READY;
	resetSuspended = false;
	cpu_status previous = disable_interrupts();
	acquire_spinlock(&lock);
	regs[0x306a] &= ~kCPInterrupts;
	regs[0x504] &= ~kVMInterrupts;
	regs[0x505] &= ~kVMInterrupts;
	regs[0xe30] &= ~0x20001u;
	(void)regs[0xe30];
	enabled = false;
	pendingFence = false;
	fence = NULL;
	release_spinlock(&lock);
	restore_interrupts(previous);
	bigtime_t deadline = system_time() + 100000;
	while ((regs[0x394] & 0x20000) != 0 && system_time() < deadline) snooze(10);
	resetSuspended = (regs[0x394] & 0x20000) == 0;
	return resetSuspended ? B_OK : B_TIMED_OUT;
}

status_t
GpuInterrupts::RearmAfterReset()
{
	// The exclusive recovery owner already halted/reset GFX and waited for
	// IH idle. Keep MSI, its handler and wired memory; discard old vectors
	// only after the reset proved they cannot describe new-epoch work.
	if (!resetSuspended || !programmed || enabled || !configured || !handler || area < 0 || signal < 0
		|| (regs[0xe30] & 0x20001) != 0 || (regs[0x394] & 0x20000) != 0)
		return B_DEV_NOT_READY;
	resetSuspended = false;
	while (acquire_sem_etc(signal, 1, B_RELATIVE_TIMEOUT, 0) == B_OK) {}
	cpu_status previous = disable_interrupts();
	acquire_spinlock(&lock);
	memset((void*)memory, 0, kAllocationBytes);
	memset(&stats, 0, sizeof(stats));
	rptr = expectedFence = 0;
	pendingFence = false; fence = NULL;
	atomic_set(&error, B_OK);
	const uint32 control = (14 << 1) | (1 << 8) | (1 << 21);
	regs[0xe30] = control | 0x80000000;
	regs[0xe32] = regs[0xe33] = 0;
	__sync_synchronize();
	enabled = true;
	regs[0xe30] = control | 0x20001;
	regs[0x306a] |= kCPInterrupts;
	regs[0x504] |= kVMInterrupts;
	regs[0x505] |= kVMInterrupts;
	(void)regs[0x505];
	release_spinlock(&lock);
	restore_interrupts(previous);
	return B_OK;
}

int32
GpuInterrupts::Handle(void* cookie)
{
	GpuInterrupts& irq = *(GpuInterrupts*)cookie;
	acquire_spinlock(&irq.lock);
	if (!irq.enabled) {
		release_spinlock(&irq.lock);
		return B_UNHANDLED_INTERRUPT;
	}
	uint32 raw = irq.memory[kRingBytes / 4];
	__sync_synchronize();
	uint32 wptr = raw & kMask;
	// WPTR overflow is bit 0; CNTL's write-one overflow-clear is bit 31.
	bool invalid = (raw & 1) != 0 || (wptr & 15) != 0 || (raw & 0x3fffc) > kMask;
	if (!invalid && wptr == irq.rptr) {
		release_spinlock(&irq.lock);
		return B_UNHANDLED_INTERRUPT;
	}
	irq.stats.interrupts++;
	bool wake = false;
	if (invalid) {
		irq.stats.overflows++;
		irq.RecordError(B_BAD_DATA);
		irq.regs[0xe30] &= ~0x20001u;
		wake = true;
	} else {
		// Consume only the captured producer extent. At most 4095 vectors fit;
		// a later arrival causes another MSI after RPTR_REARM is written.
		while (irq.rptr != wptr) {
			uint32 index = irq.rptr / 4;
			for (uint32 i = 0; i < 4; i++) irq.stats.last[i] = irq.memory[index + i];
			uint32 source = irq.stats.last[0] & 255;
			uint32 ring = irq.stats.last[2] & 255;
			uint32 vmid = (irq.stats.last[2] >> 8) & 255;
			irq.stats.vectors++;
			if (source == 181 && ring == 0 && vmid == 0) {
				if (irq.stats.eop_events < 2)
					memcpy(irq.stats.first_eop[irq.stats.eop_events], irq.stats.last, sizeof(irq.stats.last));
				irq.stats.eop_events++;
				// Hardware can report more than one EOP vector. Only the IRQ
				// that observes this job's private fence may complete its wait.
				__sync_synchronize();
				if (irq.pendingFence && *irq.fence == irq.expectedFence) {
					irq.pendingFence = false;
					irq.stats.completed_fences++;
					wake = true;
				}
			} else if (source == 146 || source == 147) {
				irq.stats.vm_faults++;
				irq.RecordError(B_BAD_DATA); wake = true;
			} else if (source == 184 || source == 185) {
				irq.stats.privileged_faults++;
				irq.RecordError(B_NOT_ALLOWED); wake = true;
			} else {
				irq.stats.unknown++;
				// Other CP error sources must not look like successful work.
				if (source == 180 || source == 183 || source == 186
					|| (source >= 192 && source <= 197)) {
					irq.RecordError(B_BAD_DATA); wake = true;
				}
			}
			irq.rptr = (irq.rptr + 16) & kMask;
		}
		__sync_synchronize();
		irq.regs[0xe32] = irq.rptr;
		(void)irq.regs[0xe32];
	}
	if (irq.Error() != B_OK) {
		// Preserve the first fault registers for the submitting thread. Stop
		// fault notification storms; that thread halts GFX and quarantines BOs.
		irq.regs[0x306a] &= ~kCPInterrupts;
		irq.regs[0x504] &= ~kVMInterrupts;
		irq.regs[0x505] &= ~kVMInterrupts;
		(void)irq.regs[0x505];
	}
	release_spinlock(&irq.lock);
	if (wake) release_sem_etc(irq.signal, 1, B_DO_NOT_RESCHEDULE);
	return wake ? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}

uint64
GpuInterrupts::Ticket(volatile uint32* completion, uint32 sequence)
{
	while (acquire_sem_etc(signal, 1, B_RELATIVE_TIMEOUT, 0) == B_OK) {}
	cpu_status previous = disable_interrupts();
	acquire_spinlock(&lock);
	uint64 ticket = stats.completed_fences;
	fence = completion; expectedFence = sequence; pendingFence = true;
	release_spinlock(&lock);
	restore_interrupts(previous);
	return ticket;
}

status_t
GpuInterrupts::Wait(uint64 ticket, bigtime_t deadline)
{
	cpu_status previous = disable_interrupts();
	acquire_spinlock(&lock);
	stats.waits++;
	release_spinlock(&lock);
	restore_interrupts(previous);
	for (;;) {
		previous = disable_interrupts();
		acquire_spinlock(&lock);
		status_t status = Error();
		bool completed = stats.completed_fences > ticket;
		release_spinlock(&lock);
		restore_interrupts(previous);
		if (status != B_OK || completed) return status;
		status = acquire_sem_etc(signal, 1, B_ABSOLUTE_TIMEOUT, deadline);
		if (status != B_OK && status != B_INTERRUPTED) return status;
	}
}

void
GpuInterrupts::Snapshot(amdgpu_irq_info& result)
{
	cpu_status previous = disable_interrupts();
	acquire_spinlock(&lock);
	result = stats;
	result.version = AMDGPU_HAIKU_ABI_VERSION; result.size = sizeof(result);
	result.enabled = enabled; result.msi = configured;
	result.vector = vector; result.ring_bytes = kRingBytes;
	result.rptr = rptr; result.wptr = enabled ? memory[kRingBytes / 4] : 0;
	result.status = Error();
	release_spinlock(&lock);
	restore_interrupts(previous);
}

void
GpuInterrupts::Uninitialize()
{
	if (!attempted) return;
	bool idle = true;
	if (programmed) {
		cpu_status previous = disable_interrupts();
		acquire_spinlock(&lock);
		regs[0x306a] &= ~kCPInterrupts;
		regs[0x504] &= ~kVMInterrupts;
		regs[0x505] &= ~kVMInterrupts;
		regs[0xe30] &= ~0x20001u;
		(void)regs[0xe30];
		enabled = false;
		release_spinlock(&lock);
		restore_interrupts(previous);
		bigtime_t deadline = system_time() + 100000;
		while ((regs[0x394] & 0x20000) != 0 && system_time() < deadline) snooze(10);
		idle = (regs[0x394] & 0x20000) == 0;
	}
	if (configured) pci->disable_msi(bus, slot, function);
	if (handler) remove_io_interrupt_handler(vector, Handle, this);
	if (configured) pci->unconfigure_msi(bus, slot, function);
	handler = configured = false;
	if (signal >= 0) delete_sem(signal);
	if (area >= 0 && idle) delete_area(area);
	if (!idle) dprintf("amdgpu: IH did not become idle; retaining wired ring\n");
	area = signal = -1;
}
