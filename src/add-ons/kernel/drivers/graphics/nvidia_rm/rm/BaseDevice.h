#pragma once

#include <atomic>
#include <OS.h>
#include <AutoDeleterOS.h>
#include <lock.h>

#include "Drivers_cpp.h"

#include "nv-include.h"
#include "nv-haiku.h"

struct select_sync_pool;


struct nv_alloc_t {
	AreaDeleter area;
	NvU32 cacheType {};
	phys_addr_t physBase {};
};


// A range of an application's memory that has been pinned so the GPU can read
// and write it: what an "OS descriptor" allocation is made of.
struct nv_user_pages_t {
	team_id team {};
	addr_t address {};
	size_t size {};
	uint64 pageCount {};
	bool writable {};
};


const char *nv_cache_type_str(NvU32 cache_type);
uint32 to_haiku_cache_type(NvU32 cache_type);


class NvHaikuBaseDevice {
protected:
	friend class NvHaikuBaseDeviceHandle;
	nv_state_t fNvState {};

public:
	NvHaikuBaseDevice();
	~NvHaikuBaseDevice();

	static inline NvHaikuBaseDevice *FromNv(nv_state_t *nv) {return static_cast<NvHaikuBaseDevice*>(nv->os_state);}
	inline nv_state_t *Nv() {return &fNvState;}
};


class NvHaikuBaseDeviceHandle {
private:
	NvHaikuBaseDevice *fDevice {};

protected:
	nv_file_private_t fNvfp {};
	std::atomic<bool> fClosed = false;

	mutex fSelectPoolMutex = MUTEX_INITIALIZER("NvHaikuSelectPool");
	select_sync_pool* fSelectPool {};
	std::atomic<bool> fDatalessEventPending {};

	nv_alloc_mapping_context_t fMmapContext {};

	status_t ControlUngated(uint32 cmd, void *data, size_t len);

public:
	NvHaikuBaseDeviceHandle(NvHaikuBaseDevice *device);
	~NvHaikuBaseDeviceHandle();

	static NvHaikuBaseDeviceHandle *FromFd(int fd);
	static inline NvHaikuBaseDeviceHandle *FromNvfp(nv_file_private_t *nvfp) {return static_cast<NvHaikuBaseDeviceHandle*>(nvfp->ctl_nvfp_priv);}
	inline nv_file_private_t &Nvfp() {return fNvfp;}
	inline nv_alloc_mapping_context_t &MmapContext() {return fMmapContext;}
	void ConsumeMmapContext();
	NvHaikuBaseDevice *GetBaseDevice() const {return fDevice;}

	inline void SetDatalessEventPending();
	void NotifySelectPool();

	status_t Close();
	status_t Control(uint32 op, void *data, size_t len);
	status_t Select(uint8 event, uint32 ref, selectsync *sync);
	status_t Deselect(uint8 event, selectsync *sync);
};


void NvHaikuBaseDeviceHandle::SetDatalessEventPending()
{
	fDatalessEventPending = true;
}
