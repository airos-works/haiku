/* A resman and a GPU in memory, for tests of NvEngine2D without a card
 * (Engine2DRecoveryTest.cpp). It stands in for sdk/NvRmApi.cpp and
 * sdk/NvRmDevice.cpp: objects are handles in a table (freeing one twice, or
 * memory that is still mapped, is an error), memory is an area (freed memory
 * is gone: reading it afterwards crashes), GPU addresses come from virtual
 * memory objects, and a thread plays the GPU: it fetches the GPFIFO entries
 * of every scheduled channel and carries out their semaphore releases, and
 * nothing else. FakeGpu says what it does: stall, raise a channel error.
 */
#include "FakeRm.h"

#include <map>
#include <mutex>
#include <vector>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

extern "C" {
#include "nvos.h"
#include "nvgputypes.h"
#include "class/cl0002.h"
#include "class/cl003e.h"
#include "class/cl0040.h"
#include "class/cl50a0.h"
#include "class/cl902d.h"
#include "class/clb06f.h"
#include "class/clc06f.h"
#include "alloc/alloc_channel.h"
#include "ctrl/ctrla06f/ctrla06fgpfifo.h"
}


namespace {

struct Object {
	NvHandle parent = 0;
	NvV32 hClass = 0;
	area_id area = -1;			// memory, or a channel's USERD
	uint8 *bits = nullptr;
	NvU64 size = 0;
	NvU64 gpuAddress = 0;		// virtual memory: where its range begins
	NvU64 gpFifo = 0;			// channel: GPU address of its GPFIFO
	bool scheduled = false;
	bool frozen = false;
	NvHandle errorContext = 0;
	NvHandle notifierMemory = 0;	// context DMA: the memory behind it
	uint32 gpGet = 0;
};

struct DmaMapping {
	NvU64 address;
	NvU64 length;
	NvHandle memory;
	NvHandle virt;
};

std::recursive_mutex sLock;
std::map<NvHandle, Object> sObjects;
std::vector<DmaMapping> sMappings;
NvHandle sNextHandle = 0xcafe0001;
NvU64 sNextGpuAddress = 0x100000000ull;
thread_id sGpuThread = -1;

}

FakeGpu gFakeGpu;


static void Fail(const char *what)
{
	fprintf(stderr, "FAIL fake resman: %s\n", what);
	gFakeGpu.errors++;
	RaiseNvStatus(NV_ERR_INVALID_ARGUMENT);
}


static area_id NewArea(NvU64 size, uint8 **bits)
{
	size = (size + B_PAGE_SIZE - 1) & ~(NvU64)(B_PAGE_SIZE - 1);
	void *address;
	area_id area = create_area("fake GPU memory", &address, B_ANY_ADDRESS,
		size, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < 0)
		Fail("out of memory");
	memset(address, 0, size);
	*bits = (uint8*)address;
	return area;
}


// What the GPU sees at a GPU address (sLock held).
static uint8 *Resolve(NvU64 address, NvU64 length)
{
	for (const DmaMapping &mapping: sMappings) {
		if (address >= mapping.address
			&& address + length <= mapping.address + mapping.length) {
			auto it = sObjects.find(mapping.memory);
			if (it == sObjects.end() || it->second.bits == nullptr)
				return nullptr;
			return it->second.bits + (address - mapping.address);
		}
	}
	return nullptr;
}


static void RunChannel(Object &channel)
{
	volatile uint32 *userd = (volatile uint32*)channel.bits;
	uint32 put = userd[0x8c / 4];
	while (channel.gpGet != put) {
		uint32 *entry = (uint32*)Resolve(channel.gpFifo + channel.gpGet * 8, 8);
		if (entry == nullptr) {
			fprintf(stderr, "FAIL fake GPU: GPFIFO entry %u not mapped\n",
				(unsigned)channel.gpGet);
			gFakeGpu.errors++;
			return;
		}
		NvU64 address = entry[0] | ((NvU64)(entry[1] & 0xff) << 32);
		uint32 words = entry[1] >> 10;
		uint32 *push = (uint32*)Resolve(address, words * 4);
		if (push == nullptr) {
			fprintf(stderr, "FAIL fake GPU: push buffer not mapped\n");
			gFakeGpu.errors++;
			return;
		}
		for (uint32 i = 0; i < words; ) {
			uint32 header = push[i++];
			uint32 count = (header >> 16) & 0x1fff;
			uint32 method = (header & 0x1fff) << 2;
			if ((header >> 29) == 1 && method == 0x10 && count >= 4) {
				NvU64 semaphore = ((NvU64)(push[i] & 0xff) << 32) | push[i + 1];
				uint32 *target = (uint32*)Resolve(semaphore, 4);
				if (target == nullptr) {
					fprintf(stderr, "FAIL fake GPU: semaphore not mapped\n");
					gFakeGpu.errors++;
				} else
					__atomic_store_n(target, push[i + 2], __ATOMIC_SEQ_CST);
			}
			i += count;
		}
		channel.gpGet = (channel.gpGet + 1) % 1024;
		userd[0x88 / 4] = channel.gpGet;
		gFakeGpu.entries++;
	}
}


static status_t GpuThread(void *)
{
	while (!gFakeGpu.quit) {
		{
			std::lock_guard<std::recursive_mutex> lock(sLock);
			for (auto &[handle, object]: sObjects) {
				if (!object.scheduled)
					continue;
				if (gFakeGpu.raiseError) {
					// the error notifier, as resman fills it
					auto context = sObjects.find(object.errorContext);
					if (context != sObjects.end()) {
						auto memory = sObjects.find(context->second.notifierMemory);
						if (memory != sObjects.end() && memory->second.bits != nullptr) {
							NvNotification *notifier
								= (NvNotification*)memory->second.bits;
							notifier[NV_CHANNELGPFIFO_NOTIFICATION_TYPE_ERROR].info32 = 31;
							notifier[NV_CHANNELGPFIFO_NOTIFICATION_TYPE_ERROR].status = 0xffff;
						}
					}
					continue;
				}
				if (!gFakeGpu.stalled && !object.frozen)
					RunChannel(object);
			}
			gFakeGpu.raiseError = false;
		}
		snooze(50);
	}
	return B_OK;
}


void FakeGpu::Start()
{
	quit = false;
	sGpuThread = spawn_thread(GpuThread, "fake GPU", B_NORMAL_PRIORITY, nullptr);
	resume_thread(sGpuThread);
}


void FakeGpu::Stop()
{
	quit = true;
	status_t result;
	wait_for_thread(sGpuThread, &result);
}


void FakeGpu::FreezeChannels()
{
	std::lock_guard<std::recursive_mutex> lock(sLock);
	for (auto &[handle, object]: sObjects) {
		if (object.gpFifo != 0)
			object.frozen = true;
	}
}


int32 FakeGpu::CountObjects(NvV32 hClass)
{
	std::lock_guard<std::recursive_mutex> lock(sLock);
	int32 count = 0;
	for (auto &[handle, object]: sObjects) {
		if (hClass == 0 || object.hClass == hClass)
			count++;
	}
	return count;
}


bool FakeGpu::IsLive(NvHandle handle)
{
	std::lock_guard<std::recursive_mutex> lock(sLock);
	return sObjects.find(handle) != sObjects.end();
}


int32 FakeGpu::CountDmaMappings()
{
	std::lock_guard<std::recursive_mutex> lock(sLock);
	return sMappings.size();
}


// #pragma mark - NvRmApi


NvRmApi::NvRmApi()
{
	std::lock_guard<std::recursive_mutex> lock(sLock);
	NvHandle handle = sNextHandle++;
	sObjects[handle].hClass = 0;
	fClient = NvRmObject(*this, handle);
}


NvHandle NvRmApi::Alloc(NvV32 hClass, NvHandle hParent, void *params,
	NvU32 paramsSize)
{
	std::lock_guard<std::recursive_mutex> lock(sLock);
	if (sObjects.find(hParent) == sObjects.end())
		Fail("allocating under a parent that is gone");
	if (gFakeGpu.refuseChannels && (hClass == PASCAL_CHANNEL_GPFIFO_A
			|| hClass == MAXWELL_CHANNEL_GPFIFO_A))
		RaiseNvStatus(NV_ERR_NO_MEMORY);

	Object object;
	object.parent = hParent;
	object.hClass = hClass;
	switch (hClass) {
		case NV01_MEMORY_SYSTEM:
		case NV01_MEMORY_LOCAL_USER:
		{
			NV_MEMORY_ALLOCATION_PARAMS *memory
				= (NV_MEMORY_ALLOCATION_PARAMS*)params;
			object.size = memory->size;
			object.area = NewArea(memory->size, &object.bits);
			break;
		}
		case NV50_MEMORY_VIRTUAL:
		{
			NV_MEMORY_ALLOCATION_PARAMS *memory
				= (NV_MEMORY_ALLOCATION_PARAMS*)params;
			if (sObjects.find(memory->hVASpace) == sObjects.end())
				Fail("virtual memory in an address space that is gone");
			object.size = memory->size;
			object.gpuAddress = sNextGpuAddress;
			sNextGpuAddress += (memory->size + 0x1fffff) & ~0x1fffffull;
			break;
		}
		case NV01_CONTEXT_DMA:
		{
			NV_CONTEXT_DMA_ALLOCATION_PARAMS *context
				= (NV_CONTEXT_DMA_ALLOCATION_PARAMS*)params;
			if (sObjects.find(context->hMemory) == sObjects.end())
				Fail("a context DMA on memory that is gone");
			object.notifierMemory = context->hMemory;
			break;
		}
		case PASCAL_CHANNEL_GPFIFO_A:
		case MAXWELL_CHANNEL_GPFIFO_A:
		{
			NV_CHANNEL_ALLOC_PARAMS *channel = (NV_CHANNEL_ALLOC_PARAMS*)params;
			if (sObjects.find(channel->hObjectError) == sObjects.end())
				Fail("a channel without its error notifier");
			if (sObjects.find(channel->hVASpace) == sObjects.end())
				Fail("a channel in an address space that is gone");
			object.gpFifo = channel->gpFifoOffset;
			object.errorContext = channel->hObjectError;
			object.area = NewArea(B_PAGE_SIZE, &object.bits);
			break;
		}
		default:
			break;
	}
	NvHandle handle = sNextHandle++;
	sObjects[handle] = object;
	return handle;
}


void NvRmApi::Free(NvHandle hObject)
{
	std::lock_guard<std::recursive_mutex> lock(sLock);
	auto it = sObjects.find(hObject);
	if (it == sObjects.end())
		Fail("freeing an object that is gone");
	for (auto &[handle, object]: sObjects) {
		if (object.parent == hObject)
			Fail("freeing an object before its children");
		if (object.errorContext == hObject)
			Fail("freeing a channel's error notifier before the channel");
		if (object.notifierMemory == hObject)
			Fail("freeing notifier memory before its context DMA");
	}
	for (const DmaMapping &mapping: sMappings) {
		if (mapping.memory == hObject || mapping.virt == hObject)
			Fail("freeing memory that is still in the GPU's address space");
	}
	if (it->second.area >= 0)
		delete_area(it->second.area);
	sObjects.erase(it);
}


void NvRmApi::Control(NvHandle hObject, NvV32 cmd, void *params,
	NvU32 paramsSize, NvU32 flags)
{
	std::lock_guard<std::recursive_mutex> lock(sLock);
	auto it = sObjects.find(hObject);
	if (it == sObjects.end())
		Fail("control of an object that is gone");
	if (cmd == NVA06F_CTRL_CMD_GPFIFO_SCHEDULE)
		it->second.scheduled = ((NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS*)params)->bEnable;
}


void NvRmApi::MapMemory(NvU32 hDevice, NvU32 hMemory, NvU64 offset,
	NvU64 length, void *&pLinearAddress, NvU32 flags, int memFd)
{
}


void NvRmApi::UnmapMemory(NvU32 hDevice, NvU32 hMemory, void *pLinearAddress,
	NvU32 flags)
{
}


NvU64 NvRmApi::MapMemoryDma(NvU32 hDevice, NvU32 hDma, NvU32 hMemory,
	NvU64 offset, NvU64 length, NvU32 flags)
{
	std::lock_guard<std::recursive_mutex> lock(sLock);
	auto virt = sObjects.find(hDma);
	auto memory = sObjects.find(hMemory);
	if (virt == sObjects.end() || memory == sObjects.end())
		Fail("mapping an object that is gone");
	if (length > virt->second.size)
		Fail("mapping more than the range holds");
	NvU64 address = virt->second.gpuAddress + offset;
	sMappings.push_back({address, length, hMemory, hDma});
	return address;
}


void NvRmApi::UnmapMemoryDma(NvU32 hDevice, NvU32 hDma, NvU32 hMemory,
	NvU32 flags, NvU64 dmaOffset)
{
	std::lock_guard<std::recursive_mutex> lock(sLock);
	for (auto it = sMappings.begin(); it != sMappings.end(); it++) {
		if (it->virt == hDma && it->memory == hMemory
			&& it->address == dmaOffset) {
			sMappings.erase(it);
			return;
		}
	}
	Fail("unmapping what is not mapped");
}


// #pragma mark - NvRmDevice


void NvRmMemoryMapping::Delete()
{
	delete_area(fArea);
	fRm->UnmapMemory(fHDevice, fHMemory, fPLinearAddress, fFlags);
}


NvRmDevice::NvRmDevice(NvRmApi &rm, NvU32 deviceId)
{
	fDevice = rm.Client().Alloc(0x80, nullptr);
	fSubdevice = fDevice.Alloc(0x2080, nullptr);
}


NvRmMemoryMapping NvRmDevice::MapMemory(NvHandle hMemory, bool isSystemMemory,
	NvU64 offset, NvU64 length, NvU32 flags, NvHandle hParent)
{
	std::lock_guard<std::recursive_mutex> lock(sLock);
	auto it = sObjects.find(hMemory);
	if (it == sObjects.end() || it->second.area < 0)
		Fail("mapping for the CPU what is not memory");
	void *address;
	area_id clone = clone_area("fake GPU memory mapping", &address,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, it->second.area);
	if (clone < 0)
		Fail("cloning an area");
	return NvRmMemoryMapping(*fDevice.Rm(), fDevice.Get(), hMemory, address,
		flags, (uint8*)address + offset, length, clone);
}
