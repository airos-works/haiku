/*
 * Copyright 2019-2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Augustin Cavalier <waddlesplash>
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <condition_variable.h>
#include <AutoDeleter.h>
#include <kernel.h>
#include <smp.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/ThreadAutoLock.h>

#include <fs/devfs.h>
#include <interrupts.h>
#include <kdevice_manager.h>
#include <ACPI.h>
#include <bus/PCI.h>
#include <driver_settings.h>
#include <vm/vm.h>

#include "IORequest.h"
#include "IOScheduler.h"
#include "trim_range.h"

extern "C" {
#include <libnvme/nvme.h>
#include <libnvme/nvme_internal.h>
}


//#define TRACE_NVME_DISK
#ifdef TRACE_NVME_DISK
#	define TRACE(x...) dprintf("nvme_disk: " x)
#else
#	define TRACE(x...) ;
#endif
#define TRACE_ALWAYS(x...)	dprintf("nvme_disk: " x)
#define TRACE_ERROR(x...)	dprintf("\33[33mnvme_disk:\33[0m " x)
#define CALLED() 			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


static const uint8 kDriveIcon[] = {
	0x6e, 0x63, 0x69, 0x66, 0x08, 0x03, 0x01, 0x00, 0x00, 0x02, 0x00, 0x16,
	0x02, 0x3c, 0xc7, 0xee, 0x38, 0x9b, 0xc0, 0xba, 0x16, 0x57, 0x3e, 0x39,
	0xb0, 0x49, 0x77, 0xc8, 0x42, 0xad, 0xc7, 0x00, 0xff, 0xff, 0xd3, 0x02,
	0x00, 0x06, 0x02, 0x3c, 0x96, 0x32, 0x3a, 0x4d, 0x3f, 0xba, 0xfc, 0x01,
	0x3d, 0x5a, 0x97, 0x4b, 0x57, 0xa5, 0x49, 0x84, 0x4d, 0x00, 0x47, 0x47,
	0x47, 0xff, 0xa5, 0xa0, 0xa0, 0x02, 0x00, 0x16, 0x02, 0xbc, 0x59, 0x2f,
	0xbb, 0x29, 0xa7, 0x3c, 0x0c, 0xe4, 0xbd, 0x0b, 0x7c, 0x48, 0x92, 0xc0,
	0x4b, 0x79, 0x66, 0x00, 0x7d, 0xff, 0xd4, 0x02, 0x00, 0x06, 0x02, 0x38,
	0xdb, 0xb4, 0x39, 0x97, 0x33, 0xbc, 0x4a, 0x33, 0x3b, 0xa5, 0x42, 0x48,
	0x6e, 0x66, 0x49, 0xee, 0x7b, 0x00, 0x59, 0x67, 0x56, 0xff, 0xeb, 0xb2,
	0xb2, 0x03, 0xa7, 0xff, 0x00, 0x03, 0xff, 0x00, 0x00, 0x04, 0x01, 0x80,
	0x07, 0x0a, 0x06, 0x22, 0x3c, 0x22, 0x49, 0x44, 0x5b, 0x5a, 0x3e, 0x5a,
	0x31, 0x39, 0x25, 0x0a, 0x04, 0x22, 0x3c, 0x44, 0x4b, 0x5a, 0x31, 0x39,
	0x25, 0x0a, 0x04, 0x44, 0x4b, 0x44, 0x5b, 0x5a, 0x3e, 0x5a, 0x31, 0x0a,
	0x04, 0x22, 0x3c, 0x22, 0x49, 0x44, 0x5b, 0x44, 0x4b, 0x08, 0x02, 0x27,
	0x43, 0xb8, 0x14, 0xc1, 0xf1, 0x08, 0x02, 0x26, 0x43, 0x29, 0x44, 0x0a,
	0x05, 0x44, 0x5d, 0x49, 0x5d, 0x60, 0x3e, 0x5a, 0x3b, 0x5b, 0x3f, 0x08,
	0x0a, 0x07, 0x01, 0x06, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x10, 0x01, 0x17,
	0x84, 0x00, 0x04, 0x0a, 0x01, 0x01, 0x01, 0x00, 0x0a, 0x02, 0x01, 0x02,
	0x00, 0x0a, 0x03, 0x01, 0x03, 0x00, 0x0a, 0x04, 0x01, 0x04, 0x10, 0x01,
	0x17, 0x85, 0x20, 0x04, 0x0a, 0x06, 0x01, 0x05, 0x30, 0x24, 0xb3, 0x99,
	0x01, 0x17, 0x82, 0x00, 0x04, 0x0a, 0x05, 0x01, 0x05, 0x30, 0x20, 0xb2,
	0xe6, 0x01, 0x17, 0x82, 0x00, 0x04
};


#define NVME_DISK_DRIVER_MODULE_NAME 	"drivers/disk/nvme_disk/driver_v1"
#define NVME_DISK_DEVICE_MODULE_NAME 	"drivers/disk/nvme_disk/device_v1"
#define NVME_DISK_DEVICE_ID_GENERATOR	"nvme_disk/device_id"

#define NVME_MAX_QPAIRS					(16)


static device_manager_info* sDeviceManager;


struct NVMeRequestOwner : IORequestOwner {
	IORequestList requests_queue;
	NVMeRequestOwner* hash_link;

	void Dump() const {}
};

struct RequestOwnerHashDefinition {
	typedef thread_id KeyType;
	typedef NVMeRequestOwner ValueType;

	size_t HashKey(thread_id key) const			{ return key; }
	size_t Hash(const ValueType* value) const	{ return value->thread; }
	bool Compare(thread_id key, const ValueType* value) const
		{ return value->thread == key; }
	ValueType*& GetLink(ValueType* value) const
		{ return value->hash_link; }
};

typedef BOpenHashTable<RequestOwnerHashDefinition, false> RequestOwnerHashTable;


struct nvme_disk_driver_info;
struct nvme_async_io;
struct qpair_info;


/*!	One NVMe command. Its completion is learned either by the thread that
	waits for it (synchronous: waiter, completed) or by the transfer it is
	part of (async), which completes its io_request when its last command
	has.
*/
struct nvme_io_request {
	status_t status;

	bool write;

	off_t lba_start;
	size_t lba_count;

	physical_entry* iovecs;
	int32 iovec_count;

	int32 iovec_i;
	uint32 iovec_offset;

	qpair_info* qpair;
	int32 completed;
	nvme_async_io* async;
		// NULL: a synchronous command, its waiter polls the queue
};


/*!	A transfer of an io_request without bouncing, as one or more commands
	that are on their way at once. Allocated with its commands and a copy of
	the physical vectors behind it.
*/
struct nvme_async_io {
	nvme_disk_driver_info*	info;
	io_request*				request;
	off_t					start;
	generic_size_t			length;
	int32					pending;
		// commands not completed, plus one while they are being submitted
	spinlock				lock;
	status_t				status;
	off_t					failed;
		// where the first failed command starts (INT64_MAX: none failed)
	bool					write_locked;
		// holds rounded_write_lock for reading
	nvme_async_io*			next;
	int32					command_count;
	nvme_io_request			commands[];
};


struct interrupt_vector {
	nvme_disk_driver_info*	info;
	uint32					entry;
		// MSI-X table entry; with one vector it serves every queue pair
	uint32					irq;
};


struct qpair_info {
	struct nvme_qpair*		qpair;
	nvme_disk_driver_info*	info;
	uint32					index;
	uint32					vector;
	// The queue's completion thread waits for its interrupt here.
	ConditionVariable		interrupt;
	int32					interrupted;
	int32					interrupt_count;
	int32					idle;
	int32					outstanding;
	int32					async_outstanding;
		// commands of asynchronous transfers: the completion thread's
	// Threads waiting for a synchronous command wait here; the interrupt
	// wakes them, and they complete the queue themselves.
	ConditionVariable		sync_condition;
	int32					sync_waiters;
	bool					polling;
	thread_id				thread;
	// transfers whose last command completed, to be reported once the
	// queue is unlocked
	spinlock				done_lock;
	nvme_async_io*			done;
};


struct nvme_disk_driver_info {
	device_node*			node;
	pci_info				info;

	struct nvme_ctrlr*		ctrlr;

	struct nvme_ns*			ns;
	uint64					capacity;
	uint32					block_size;
	uint32					max_io_blocks;
	status_t				media_status;

	DMAResource				dma_resource;
	sem_id					dma_buffers_sem;

	RequestOwnerHashTable	request_owners;
	mutex					request_owners_lock;

	rw_lock					rounded_write_lock;

	rw_lock					suspend_lock;
		// read locked for every command, write locked while suspended
	bool					suspended;

	pci_device_module_info*	pci;
	pci_device*				pci_device_cookie;
	bool					idle_d3;
		// "s2idle_d3" in the nvme_disk driver settings (the default with
		// AMD's processors): D3 in a suspend to idle, as the platform's low
		// power constraints ask
	bool					in_idle_d3;
	volatile uint32*		msix_table;
	area_id					msix_area;
	uint32					msix_saved[(NVME_MAX_QPAIRS + 1) * 4];
		// the MSI-X vectors in use: some controllers clear their table on
		// a reset (CC.EN), which Linux does not notice as it sets up its
		// vectors again after every reset

	interrupt_vector		vectors[NVME_MAX_QPAIRS + 1];
	uint32					vector_count;
	uint32					msix_count;
		// vector 0 is the admin queue's; with more than one, each queue pair
		// interrupts on one of the others (nvme_qpair_vector())
	uint32					handled_flags;
	bool					polling;
		// no interrupts at all: the completion threads poll

	qpair_info				qpairs[NVME_MAX_QPAIRS];
	uint32					qpair_count;

	int32					quit;

	// Nobody completes the queues while they are down (suspended, or being
	// set up again on resume); "pollers" counts those who do it now.
	int32					queues_down;
	int32					pollers;

	int64					bounced;
};


typedef struct {
	nvme_disk_driver_info*		info;
} nvme_disk_handle;


static status_t
get_geometry(nvme_disk_handle* handle, device_geometry* geometry)
{
	nvme_disk_driver_info* info = handle->info;

	devfs_compute_geometry_size(geometry, info->capacity, info->block_size);
	geometry->bytes_per_physical_sector = info->block_size;

	geometry->device_type = B_DISK;
	geometry->removable = false;

	geometry->read_only = false;
	geometry->write_once = false;

	TRACE("get_geometry(): %" B_PRId32 ", %" B_PRId32 ", %" B_PRId32 ", %" B_PRId32 ", %d, %d, %d, %d\n",
		geometry->bytes_per_sector, geometry->sectors_per_track,
		geometry->cylinder_count, geometry->head_count, geometry->device_type,
		geometry->removable, geometry->read_only, geometry->write_once);

	return B_OK;
}


static void
nvme_disk_set_capacity(nvme_disk_driver_info* info, uint64 capacity,
	uint32 blockSize)
{
	TRACE("set_capacity(device = %p, capacity = %" B_PRIu64 ", blockSize = %" B_PRIu32 ")\n",
		info, capacity, blockSize);

	info->capacity = capacity;
	info->block_size = blockSize;
}


//	#pragma mark - device module API


static int32 nvme_interrupt_handler(void* _vector);
static status_t setup_interrupts(nvme_disk_driver_info* info,
	pci_device_module_info* pci, pci_device* pcidev, uint32 qpairs);
static status_t start_threads(nvme_disk_driver_info* info);
static void stop_threads(nvme_disk_driver_info* info);


static void
nvme_disk_configure_features(nvme_disk_driver_info* info)
{
	if (info->ctrlr->feature_supported[NVME_FEAT_INTERRUPT_COALESCING]) {
		uint32 microseconds = 16, threshold = 32;
		nvme_ctrlr_set_feature(info->ctrlr, false, NVME_FEAT_INTERRUPT_COALESCING,
			((microseconds / 100) << 8) | threshold, 0, 0, 0, 0, NULL, 0, NULL);
	}

	if (info->ctrlr->feature_supported[NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION]) {
		// dump power states
		struct nvme_ctrlr_data& cdata = info->ctrlr->cdata;
		if (cdata.npss > 0 && cdata.npss < 31) {
			TRACE_ALWAYS("\tpower states: %u\n", cdata.npss);
			for (uint8 i = 0; i <= cdata.npss; i++) {
				struct nvme_power_state	psd;
				memcpy(&psd, &cdata.psd[i], sizeof(struct nvme_power_state));
				TRACE_ALWAYS("\tps %u: mp:%fW %soperational enlat:%u exlat:%u rrt:%u rrl:%u\n",
					i, psd.mp / (psd.mxps == 0 ? 100.0 : 10000.0),
					psd.nops ? "non-" : "", psd.enlat, psd.exlat, psd.rrt, psd.rrl);
				TRACE_ALWAYS("\trwt:%u rwl:%u idlp:%fW actp:%fW apw:%u\n", psd.rwt, psd.rwl,
					psd.idlp / (psd.ips == 2 ? 100.0 : (psd.ips == 1 ? 10000.0 : 1.0)),
					psd.actp / (psd.aps == 2 ? 100.0 : (psd.aps == 1 ? 10000.0 : 1.0)),
					psd.apw);
			}

			uint32_t tableSize = 32 * sizeof(uint64);
			uint64* table = (uint64*)malloc(tableSize);
			memset(table, 0, tableSize);

			// configure apst: the table is filled with two differents states which matches
			// the behavior of the Linux driver, itself inspired by Intel/Windows drivers.
			// a first state with a low latency power state and a second state with a higher
			// latency power state. the feature is activated if at least one state is found.
			uint64 target = 0;
			bool firstStateSet = false;
			bool secondStateSet = false;
			for (uint8 i = cdata.npss; i > 0; i--) {
				struct nvme_power_state	psd;
				memcpy(&psd, &cdata.psd[i], sizeof(struct nvme_power_state));
				if (psd.nops && psd.exlat <= 100000) {
					uint32 totalLatency = psd.enlat + psd.exlat;
					uint32 transitionTime = 0;
					if (totalLatency < 100000 && !secondStateSet) {
						secondStateSet = true;
						transitionTime = 2000;
					}
					if (totalLatency < 15000 && secondStateSet && !firstStateSet) {
						transitionTime = 100;
						firstStateSet = true;
					}
					if (transitionTime > 0)
						target = (i << 3) | (transitionTime << 8);
				}
				table[i - 1] = target;
			}
			TRACE_ALWAYS("\tautonomous power state transition table:\n");
			for (int i = 0; i < 8; i++) {
				// skip the rest if the four values are zero
				if (table[i * 4] == 0 && table[i * 4 + 1] == 0 && table[i * 4 + 2] == 0
					&& table[i * 4 + 3] == 0) {
					break;
				}
				TRACE_ALWAYS("\t%" B_PRIx64 " %" B_PRIx64" %" B_PRIx64" %" B_PRIx64 "\n",
					table[i * 4], table[i * 4 + 1], table[i * 4 + 2], table[i * 4 + 3]);
			}
			int err = nvme_ctrlr_set_feature(info->ctrlr, false,
				NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION,
				(firstStateSet || secondStateSet) ? 1 : 0, 0, 0, 0, 0, table, tableSize, NULL);
			if (err != 0)
				TRACE_ERROR("failed to set apst table!\n");
			else
				TRACE_ALWAYS("\t=> feature apst table set\n");

			free(table);
		}
	}
}


/*!	D3 in a suspend to idle is the default with AMD's processors, as Linux
	has it there (acpi_storage_d3(): their platforms ask for it, and their
	S0i3 takes the disk's power): "s2idle_d3 false" turns it off. */
static bool
idle_d3_default()
{
#if defined(__i386__) || defined(__x86_64__)
	cpuid_info cpuid;
	return get_cpuid(&cpuid, 0, 0) == B_OK
		&& memcmp(cpuid.eax_0.vendor_id, "AuthenticAMD", 12) == 0;
#else
	return false;
#endif
}


static status_t
nvme_disk_init_device(void* _info, void** _cookie)
{
	CALLED();
	nvme_disk_driver_info* info = (nvme_disk_driver_info*)_info;
	ASSERT(info->ctrlr == NULL);

	pci_device_module_info* pci;
	pci_device* pcidev;
	device_node* parent = sDeviceManager->get_parent_node(info->node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&pci,
		(void**)&pcidev);
	pci->get_pci_info(pcidev, &info->info);
	sDeviceManager->put_node(parent);
	info->pci = pci;
	info->pci_device_cookie = pcidev;
	info->in_idle_d3 = false;
	info->msix_table = NULL;
	info->msix_area = -1;
	void* settings = load_driver_settings("nvme_disk");
	info->idle_d3 = get_driver_boolean_parameter(settings, "s2idle_d3",
		idle_d3_default(), true);
	unload_driver_settings(settings);

	// construct the libnvme pci_device struct
	pci_device* device = new pci_device;
	device->vendor_id = info->info.vendor_id;
	device->device_id = info->info.device_id;
	device->subvendor_id = 0;
	device->subdevice_id = 0;

	device->domain = 0;
	device->bus = info->info.bus;
	device->dev = info->info.device;
	device->func = info->info.function;

	device->pci_info = &info->info;

	// enable busmaster and memory mapped access
	uint16 command = pci->read_pci_config(pcidev, PCI_command, 2);
	command |= PCI_command_master | PCI_command_memory;
#if defined(__aarch64__)
	command |= PCI_command_int_disable;
#endif
	pci->write_pci_config(pcidev, PCI_command, 2, command);

	// open the controller
	info->ctrlr = nvme_ctrlr_open(device, NULL);
	if (info->ctrlr == NULL) {
		TRACE_ERROR("failed to open the controller!\n");
		return B_ERROR;
	}

	struct nvme_ctrlr_stat* cstat = (struct nvme_ctrlr_stat*)malloc(sizeof(struct nvme_ctrlr_stat));
	if (cstat == NULL)
		return B_NO_MEMORY;
	MemoryDeleter cstatDeleter(cstat);

	int err = nvme_ctrlr_stat(info->ctrlr, cstat);
	if (err != 0) {
		TRACE_ERROR("failed to get controller information!\n");
		nvme_ctrlr_close(info->ctrlr);
		return err;
	}

	TRACE_ALWAYS("attached to NVMe device \"%s (%s)\"\n", cstat->mn, cstat->sn);
	TRACE_ALWAYS("\tmaximum transfer size: %" B_PRIuSIZE "\n", cstat->max_xfer_size);
	TRACE_ALWAYS("\tqpair count: %d\n", cstat->io_qpairs);

	// TODO: export more than just the first namespace!
	info->ns = nvme_ns_open(info->ctrlr, cstat->ns_ids[0]);
	if (info->ns == NULL) {
		TRACE_ERROR("failed to open namespace!\n");
		nvme_ctrlr_close(info->ctrlr);
		return B_ERROR;
	}
	TRACE_ALWAYS("namespace 0\n");

	struct nvme_ns_stat nsstat;
	err = nvme_ns_stat(info->ns, &nsstat);
	if (err != 0) {
		TRACE_ERROR("failed to get namespace information!\n");
		nvme_ctrlr_close(info->ctrlr);
		return err;
	}

	// store capacity information
	TRACE_ALWAYS("\tblock size: %" B_PRIuSIZE ", stripe size: %u\n",
		nsstat.sector_size, info->ns->stripe_size);
	nvme_disk_set_capacity(info, nsstat.sectors, nsstat.sector_size);

	// One queue pair per CPU, or as many as evenly divide the CPUs.
	uint32 try_qpairs = cstat->io_qpairs;
	try_qpairs = min_c(try_qpairs, NVME_MAX_QPAIRS);
	if (try_qpairs >= (uint32)smp_get_num_cpus()) {
		try_qpairs = smp_get_num_cpus();
	} else {
		// Find the highest number of qpairs that evenly divides the number of CPUs.
		while ((smp_get_num_cpus() % try_qpairs) != 0)
			try_qpairs--;
	}

	// Interrupts: with MSI-X a vector for each queue pair besides the admin
	// queue's, so that a completion wakes only the thread of its own queue
	// and arrives on the processor that queue is for.
	status_t status = setup_interrupts(info, pci, pcidev, try_qpairs);
	if (status != B_OK) {
		nvme_ctrlr_close(info->ctrlr);
		return status;
	}

	nvme_disk_configure_features(info);

	// allocate qpairs
	info->qpair_count = 0;
	for (uint32 i = 0; i < try_qpairs; i++) {
		struct nvme_qpair* qpair = nvme_ioqp_get(info->ctrlr,
			(enum nvme_qprio)0, 0);
		if (qpair == NULL)
			break;

		qpair_info& qpinfo = info->qpairs[i];
		qpinfo.qpair = qpair;
		qpinfo.info = info;
		qpinfo.index = i;
		qpinfo.vector = info->vector_count > 1 ? nvme_qpair_vector(qpair) : 0;
		qpinfo.interrupt.Init(&qpinfo, "nvme_disk queue interrupt");
		qpinfo.interrupted = 0;
		qpinfo.interrupt_count = 0;
		qpinfo.idle = 0;
		qpinfo.outstanding = 0;
		qpinfo.async_outstanding = 0;
		qpinfo.sync_condition.Init(&qpinfo, "nvme_disk command");
		qpinfo.sync_waiters = 0;
		qpinfo.polling = info->polling;
		qpinfo.thread = -1;
		B_INITIALIZE_SPINLOCK(&qpinfo.done_lock);
		qpinfo.done = NULL;
		info->qpair_count++;

		// The queue is used by the processors i, i + count, ...; its
		// interrupt goes to the first of them.
		if (qpinfo.vector != 0 && i < (uint32)smp_get_num_cpus()) {
			InterruptsLocker _;
			assign_io_interrupt_to_cpu(info->vectors[qpinfo.vector].irq, i);
		}
	}
	if (info->qpair_count == 0) {
		TRACE_ERROR("failed to allocate qpairs!\n");
		nvme_ctrlr_close(info->ctrlr);
		return B_NO_MEMORY;
	}
	if (info->qpair_count != try_qpairs) {
		TRACE_ALWAYS("warning: did not get expected number of qpairs\n");
	}
	TRACE_ALWAYS("%" B_PRIu32 " queue pairs, %" B_PRIu32 " interrupt vectors%s\n",
		info->qpair_count, info->vector_count,
		info->polling ? " (polling)" : "");

	// allocate DMA buffers
	int buffers = info->qpair_count * 2;

	dma_restrictions restrictions = {};
	restrictions.alignment = B_PAGE_SIZE;
		// Technically, the first and last segments in a transfer can be aligned
		// only on 32-bits, and the rest only need to have sizes that are a multiple
		// of the block size.
	restrictions.max_segment_count = (NVME_MAX_SGL_DESCRIPTORS / 2);
	restrictions.max_transfer_size = cstat->max_xfer_size;
	info->max_io_blocks = cstat->max_xfer_size / nsstat.sector_size;

	err = info->dma_resource.Init(restrictions, B_PAGE_SIZE, buffers, buffers);
	if (err != 0) {
		TRACE_ERROR("failed to initialize DMA resource!\n");
		nvme_ctrlr_close(info->ctrlr);
		return err;
	}

	info->dma_buffers_sem = create_sem(buffers, "nvme buffers sem");
	if (info->dma_buffers_sem < 0) {
		TRACE_ERROR("failed to create DMA buffers semaphore!\n");
		nvme_ctrlr_close(info->ctrlr);
		return info->dma_buffers_sem;
	}

	if (info->request_owners.Init(buffers) != B_OK)
		return B_ERROR;

	mutex_init(&info->request_owners_lock, "nvme request owners");

	// set up rounded-write lock
	rw_lock_init(&info->rounded_write_lock, "nvme rounded writes");
	rw_lock_init(&info->suspend_lock, "nvme suspend");

	status = start_threads(info);
	if (status != B_OK) {
		nvme_ctrlr_close(info->ctrlr);
		return status;
	}

	*_cookie = info;
	return B_OK;
}


static void
nvme_disk_uninit_device(void* _cookie)
{
	CALLED();
	nvme_disk_driver_info* info = (nvme_disk_driver_info*)_cookie;

	stop_threads(info);
	for (uint32 i = 0; i < info->vector_count; i++) {
		if (info->vectors[i].irq != 0) {
			remove_io_interrupt_handler(info->vectors[i].irq,
				nvme_interrupt_handler, &info->vectors[i]);
		}
	}

	rw_lock_destroy(&info->rounded_write_lock);
	rw_lock_destroy(&info->suspend_lock);
	if (info->msix_area >= 0)
		delete_area(info->msix_area);

	nvme_ns_close(info->ns);
	nvme_ctrlr_close(info->ctrlr);

	// TODO: Deallocate MSI(-X).
	// TODO: Deallocate PCI.
}


static status_t
nvme_disk_open(void* _info, const char* path, int openMode, void** _cookie)
{
	CALLED();

	nvme_disk_driver_info* info = (nvme_disk_driver_info*)_info;
	nvme_disk_handle* handle = (nvme_disk_handle*)malloc(
		sizeof(nvme_disk_handle));
	if (handle == NULL)
		return B_NO_MEMORY;

	handle->info = info;

	*_cookie = handle;
	return B_OK;
}


static status_t
nvme_disk_close(void* cookie)
{
	CALLED();

	//nvme_disk_handle* handle = (nvme_disk_handle*)cookie;
	return B_OK;
}


static status_t
nvme_disk_free(void* cookie)
{
	CALLED();

	nvme_disk_handle* handle = (nvme_disk_handle*)cookie;
	free(handle);
	return B_OK;
}


// #pragma mark - I/O


static void ior_reset_sgl(nvme_io_request* request, uint32_t offset);
static int ior_next_sge(nvme_io_request* request, uint64_t* address,
	uint32_t* length);


// How long a completion thread waits for an interrupt before it looks at
// its queue anyway, while commands are outstanding: a safety net for lost
// interrupts. Three completions in a row found that way, without an
// interrupt, and the queue is polled instead.
static const bigtime_t kInterruptGrace = 20000;
// The same, when nothing was outstanding as it went to sleep.
static const bigtime_t kIdleCheck = 1000000;
// Polling (no interrupts): the first and the longest wait between looks.
static const bigtime_t kMinPollDelay = 20;
static const bigtime_t kMaxPollDelay = 2000;
// A synchronous waiter looks at its queue after this long without an
// interrupt, and gives the controller up after kSyncGiveUp without its
// completion.
static const bigtime_t kSyncWaitTimeout = 20000;
static const bigtime_t kSyncGiveUp = 20000000;


static int32
nvme_interrupt_handler(void* _vector)
{
	interrupt_vector* vector = (interrupt_vector*)_vector;
	nvme_disk_driver_info* info = vector->info;
	bool woken = false;
	for (uint32 i = 0; i < info->qpair_count; i++) {
		qpair_info* qpinfo = &info->qpairs[i];
		if (info->vector_count > 1 && qpinfo->vector != vector->entry)
			continue;
		atomic_set(&qpinfo->interrupted, 1);
		atomic_add(&qpinfo->interrupt_count, 1);
		woken = true;
		// Waiters for synchronous commands complete the queue themselves
		// (the waiter of a command is woken by its interrupt, not by another
		// thread); the completion thread is needed for asynchronous ones.
		if (atomic_get(&qpinfo->sync_waiters) > 0) {
			qpinfo->sync_condition.NotifyAll();
			if (atomic_get(&qpinfo->async_outstanding) == 0)
				continue;
		}
		qpinfo->interrupt.NotifyOne();
	}
	return woken ? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


static qpair_info*
get_qpair(nvme_disk_driver_info* info)
{
	return &info->qpairs[smp_get_current_cpu() % info->qpair_count];
}


//! A polling completion thread that sleeps because nothing is outstanding
//! is woken when something is again.
static void
command_submitted(qpair_info* qpinfo)
{
	// (with interrupts, the completion's wakes the queue's thread)
	if (atomic_add(&qpinfo->outstanding, 1) == 0 && qpinfo->polling
		&& atomic_get(&qpinfo->idle) != 0) {
		qpinfo->interrupt.NotifyOne();
	}
}


/*!	The last command of a transfer completed (or the transfer could not be
	submitted further): its locks are released, and it is reported, on
	whatever thread saw it, without any queue locked. The report may start
	more I/O.
*/
static void
async_io_done(nvme_async_io* io)
{
	nvme_disk_driver_info* info = io->info;
	if (io->write_locked)
		rw_lock_read_unlock(&info->rounded_write_lock);
	rw_lock_read_unlock(&info->suspend_lock);

	const generic_size_t transferred = io->failed == INT64_MAX
		? io->length : io->failed - io->start;
	io->request->SetTransferredBytes(io->status != B_OK, transferred);
	// notification may delete the request
	io->request->SetStatusAndNotify(io->status);
	free(io);
}


//! Called by libnvme with the queue pair's lock held: nothing here may
//! wait or start I/O.
static void
io_finished_callback(nvme_io_request* request, const struct nvme_cpl* cpl)
{
	status_t status = nvme_cpl_is_error(cpl) ? B_IO_ERROR : B_OK;
	qpair_info* qpinfo = request->qpair;
	atomic_add(&qpinfo->outstanding, -1);

	nvme_async_io* io = request->async;
	if (io != NULL) {
		atomic_add(&qpinfo->async_outstanding, -1);
		if (status != B_OK) {
			const off_t offset = request->lba_start * io->info->block_size;
			InterruptsSpinLocker locker(io->lock);
			if (io->status == B_OK)
				io->status = status;
			if (offset < io->failed)
				io->failed = offset;
		}
		if (atomic_add(&io->pending, -1) == 1) {
			InterruptsSpinLocker locker(qpinfo->done_lock);
			io->next = qpinfo->done;
			qpinfo->done = io;
		}
		return;
	}

	request->status = status;
	atomic_set(&request->completed, 1);
		// the last access: the waiter returns as soon as it sees this
	if (atomic_get(&qpinfo->sync_waiters) > 0)
		qpinfo->sync_condition.NotifyAll();
}


/*!	Completes what the controller finished on this queue. Any thread may do
	this; the queue's completion thread does it on every interrupt.
*/
static uint32
poll_qpair(qpair_info* qpinfo)
{
	nvme_disk_driver_info* info = qpinfo->info;
	atomic_add(&info->pollers, 1);
	if (atomic_get(&info->queues_down) != 0) {
		atomic_add(&info->pollers, -1);
		return 0;
	}
	uint32 completions = nvme_qpair_poll(qpinfo->qpair, 0);
	atomic_add(&info->pollers, -1);

	nvme_async_io* done;
	{
		InterruptsSpinLocker locker(qpinfo->done_lock);
		done = qpinfo->done;
		qpinfo->done = NULL;
	}
	while (done != NULL) {
		nvme_async_io* next = done->next;
		async_io_done(done);
		done = next;
	}
	return completions;
}


/*!	Completes what the controller finishes on a queue, and reports finished
	transfers. It runs on the processor that the queue's interrupt goes to
	(and whose threads use the queue): an interrupt wakes it, and it wakes
	the waiter, without involving another processor. (The mask does not
	keep it from running when that processor is disabled.)
*/
static status_t
completion_thread(void* data)
{
	qpair_info* qpinfo = (qpair_info*)data;
	nvme_disk_driver_info* info = qpinfo->info;
	bigtime_t pollDelay = kMinPollDelay;
	int32 withoutInterrupt = 0;
	int32 lastInterruptCount = 0;

	if (qpinfo->index < (uint32)smp_get_num_cpus()) {
		CPUSet mask;
		mask.SetBit(qpinfo->index);
		Thread* thread = thread_get_current_thread();
		{
			ThreadLocker locker(thread);
			thread->cpumask = mask;
		}
		thread_yield();
	}

	while (atomic_get(&info->quit) == 0) {
		bool interrupted = atomic_get_and_set(&qpinfo->interrupted, 0) != 0;
		bool timedOut = false;
		if (!interrupted) {
			ConditionVariableEntry entry;
			qpinfo->interrupt.Add(&entry);
			interrupted = atomic_get_and_set(&qpinfo->interrupted, 0) != 0;
			if (!interrupted) {
				const bool outstanding = atomic_get(&qpinfo->outstanding) > 0;
				const bool asyncOutstanding
					= atomic_get(&qpinfo->async_outstanding) > 0;
				bigtime_t timeout;
				if (qpinfo->polling) {
					timeout = pollDelay;
					if (!outstanding) {
						atomic_set(&qpinfo->idle, 1);
						if (atomic_get(&qpinfo->outstanding) == 0)
							timeout = B_INFINITE_TIMEOUT;
					}
				} else {
					// (Synchronous commands are their waiters' to complete,
					// commands submitted while it sleeps without any are
					// looked for after kIdleCheck, should their interrupt
					// get lost.)
					timeout = asyncOutstanding ? kInterruptGrace : kIdleCheck;
				}
				timedOut = entry.Wait(
					timeout == B_INFINITE_TIMEOUT ? 0 : B_RELATIVE_TIMEOUT,
					timeout == B_INFINITE_TIMEOUT ? 0 : timeout) == B_TIMED_OUT;
				atomic_set(&qpinfo->idle, 0);
				interrupted = atomic_get_and_set(&qpinfo->interrupted, 0) != 0;
			}
		}
		if (atomic_get(&info->quit) != 0)
			break;

		const uint32 completions = poll_qpair(qpinfo);
		if (qpinfo->polling) {
			pollDelay = completions > 0 ? kMinPollDelay
				: min_c(pollDelay * 2, kMaxPollDelay);
		} else if (!timedOut) {
			// an interrupt
			withoutInterrupt = 0;
		} else if (completions > 0) {
			// Found without being woken: slow commands (whose interrupt is
			// still to come) look like that too, but then interrupts do
			// arrive in between.
			const int32 interruptCount
				= atomic_get(&qpinfo->interrupt_count);
			if (interruptCount != lastInterruptCount) {
				lastInterruptCount = interruptCount;
				withoutInterrupt = 0;
			}
			if (++withoutInterrupt >= 3) {
				TRACE_ALWAYS("queue %" B_PRIu32 ": completions without "
					"interrupts, polling it from now on\n", qpinfo->index);
				qpinfo->polling = true;
			}
		}
	}
	return B_OK;
}


static status_t
start_threads(nvme_disk_driver_info* info)
{
	info->quit = 0;
	info->queues_down = 0;
	info->pollers = 0;
	info->bounced = 0;

	for (uint32 i = 0; i < info->qpair_count; i++) {
		char name[B_OS_NAME_LENGTH];
		snprintf(name, sizeof(name), "nvme_disk queue %" B_PRIu32, i);
		qpair_info* qpinfo = &info->qpairs[i];
		qpinfo->thread = spawn_kernel_thread(completion_thread, name,
			B_URGENT_DISPLAY_PRIORITY + 1, qpinfo);
		if (qpinfo->thread < 0)
			return qpinfo->thread;
		resume_thread(qpinfo->thread);
	}
	return B_OK;
}


static void
stop_threads(nvme_disk_driver_info* info)
{
	atomic_set(&info->quit, 1);
	for (uint32 i = 0; i < info->qpair_count; i++) {
		qpair_info* qpinfo = &info->qpairs[i];
		if (qpinfo->thread < 0)
			continue;
		qpinfo->interrupt.NotifyAll();
		wait_for_thread(qpinfo->thread, NULL);
		qpinfo->thread = -1;
	}
}


/*!	Interrupt vectors for \a qpairs queue pairs: with MSI-X one for the admin
	queue and one for each queue pair (or as many as there are), else MSI or
	the PCI interrupt line shared by all of them, else none (polling).
*/
static status_t
setup_interrupts(nvme_disk_driver_info* info, pci_device_module_info* pci,
	pci_device* pcidev, uint32 qpairs)
{
	info->vector_count = 0;
	info->msix_count = 0;
	info->polling = false;
	info->handled_flags = 0;
	for (uint32 i = 0; i <= NVME_MAX_QPAIRS; i++) {
		info->vectors[i].info = info;
		info->vectors[i].entry = i;
		info->vectors[i].irq = 0;
	}

#if defined(__aarch64__)
	// Completion queue processing is deferred to threads. An unacknowledged
	// level-triggered INTx can prevent them from running on ARM64. Keep INTx
	// disabled and poll without a working MSI route.
	uint32 irq = 0;
	(void)pci;
	(void)pcidev;
#else
	uint16 command = pci->read_pci_config(pcidev, PCI_command, 2);
	command &= ~PCI_command_int_disable;
	pci->write_pci_config(pcidev, PCI_command, 2, command);
	uint32 irq = info->info.u.h0.interrupt_line;
	if (irq == 0xFF)
		irq = 0;
	// the PCI interrupt line may be shared with other devices
	uint32 flags = B_NO_HANDLED_INFO;

	uint32 msixCount = pci->get_msix_count(pcidev);
	if (msixCount > 0) {
		uint32 count = min_c(msixCount, qpairs + 1);
		if (count < 2)
			count = 1;
		uint32 msixVector = 0;
		status_t status = pci->configure_msix(pcidev, count, &msixVector);
		if (status != B_OK && count > 1) {
			count = 1;
			status = pci->configure_msix(pcidev, count, &msixVector);
		}
		if (status == B_OK)
			status = pci->enable_msix(pcidev);
		if (status == B_OK) {
			TRACE_ALWAYS("using MSI-X, %" B_PRIu32 " vectors\n", count);
			for (uint32 i = 0; i < count; i++)
				info->vectors[i].irq = msixVector + i;
			info->vector_count = count;
			info->msix_count = count;
			flags = 0;
		} else
			TRACE_ALWAYS("MSI-X unavailable: %s\n", strerror(status));
	} else if (pci->get_msi_count(pcidev) >= 1) {
		uint32 msiVector = 0;
		status_t status = pci->configure_msi(pcidev, 1, &msiVector);
		if (status == B_OK)
			status = pci->enable_msi(pcidev);
		if (status == B_OK) {
			TRACE_ALWAYS("using message signaled interrupts\n");
			info->vectors[0].irq = msiVector;
			info->vector_count = 1;
			flags = 0;
		} else
			TRACE_ALWAYS("MSI unavailable: %s\n", strerror(status));
	}
	info->handled_flags = flags;
#endif

	if (info->vector_count == 0 && irq != 0) {
		info->vectors[0].irq = irq;
		info->vector_count = 1;
	}
	if (info->vector_count == 0) {
		TRACE_ALWAYS("device PCI:%d:%d:%d has no usable interrupt; using polling\n",
			info->info.bus, info->info.device, info->info.function);
		info->polling = true;
		nvme_ctrlr_set_io_vectors(info->ctrlr, 1);
		return B_OK;
	}

	nvme_ctrlr_set_io_vectors(info->ctrlr, info->vector_count);
	for (uint32 i = 0; i < info->vector_count; i++) {
		status_t status = install_io_interrupt_handler(info->vectors[i].irq,
			nvme_interrupt_handler, &info->vectors[i], info->handled_flags);
		if (status != B_OK) {
			while (i-- > 0) {
				remove_io_interrupt_handler(info->vectors[i].irq,
					nvme_interrupt_handler, &info->vectors[i]);
			}
			return status;
		}
	}
	return B_OK;
}


//! Readies a command for submit_command() and await_command().
static inline void
prepare_sync_command(nvme_io_request* request)
{
	request->completed = 0;
	request->async = NULL;
}


/*!	Submits a command, waiting for a free request when the queue has none
	(never on a completion thread: they do not submit).
*/
static int
submit_command(nvme_disk_driver_info* info, qpair_info* qpinfo,
	nvme_io_request* request)
{
	request->qpair = qpinfo;
	for (int32 attempt = 0;; attempt++) {
		if (request->async != NULL)
			atomic_add(&qpinfo->async_outstanding, 1);
		command_submitted(qpinfo);
		int ret;
		if (request->write) {
			ret = nvme_ns_writev(info->ns, qpinfo->qpair, request->lba_start,
				request->lba_count, (nvme_cmd_cb)io_finished_callback, request,
				0, (nvme_req_reset_sgl_cb)ior_reset_sgl,
				(nvme_req_next_sge_cb)ior_next_sge);
		} else {
			ret = nvme_ns_readv(info->ns, qpinfo->qpair, request->lba_start,
				request->lba_count, (nvme_cmd_cb)io_finished_callback, request,
				0, (nvme_req_reset_sgl_cb)ior_reset_sgl,
				(nvme_req_next_sge_cb)ior_next_sge);
		}
		if (ret == 0)
			return 0;
		atomic_add(&qpinfo->outstanding, -1);
		if (request->async != NULL)
			atomic_add(&qpinfo->async_outstanding, -1);
		if (ret != ENOMEM || attempt >= 10000)
			return ret;

		// Every request of the queue is in use: complete some, and try again.
		poll_qpair(qpinfo);
		snooze(attempt < 10 ? 10 : 100);
	}
}


/*!	Waits for a synchronous command: the queue's interrupt wakes this
	thread, which completes the queue itself, as does whoever else completes
	it. (Then a completion thread that is busy reporting is no matter, and
	neither is one that is the waiter.)
*/
static void
await_command(nvme_disk_driver_info* info, qpair_info* qpinfo,
	nvme_io_request* request)
{
	const bigtime_t start = system_time();
	bool failed = false;
	atomic_add(&qpinfo->sync_waiters, 1);
	while (true) {
		ConditionVariableEntry entry;
		qpinfo->sync_condition.Add(&entry);

		// done already? (a fast device, or one emulated)
		poll_qpair(qpinfo);
		if (atomic_get(&request->completed) != 0)
			break;

		if (entry.Wait(B_RELATIVE_TIMEOUT, qpinfo->polling
				? kMaxPollDelay : kSyncWaitTimeout) == B_OK
			|| qpinfo->polling) {
			continue;
		}

		if (!failed && system_time() - start > kSyncGiveUp) {
			// The controller stalled: fail what it has, which completes
			// this command too.
			TRACE_ERROR("timed out waiting for a completion!\n");
			nvme_qpair_fail(qpinfo->qpair);
			failed = true;
		}
	}
	atomic_add(&qpinfo->sync_waiters, -1);
}


static void
ior_reset_sgl(nvme_io_request* request, uint32_t offset)
{
	TRACE("IOR Reset: %" B_PRIu32 "\n", offset);

	int32 i = 0;
	while (i < request->iovec_count && offset > 0
		&& request->iovecs[i].size <= offset) {
		offset -= request->iovecs[i].size;
		i++;
	}
	request->iovec_i = i;
	request->iovec_offset = offset;
}


static int
ior_next_sge(nvme_io_request* request, uint64_t* address, uint32_t* length)
{
	int32 index = request->iovec_i;
	if (index < 0 || index >= request->iovec_count
		|| request->iovec_offset >= request->iovecs[index].size)
		return -1;

	*address = request->iovecs[index].address + request->iovec_offset;
	*length = request->iovecs[index].size - request->iovec_offset;

	TRACE("IOV %d (+ %" B_PRIu32 "): 0x%" B_PRIx64 ", %" B_PRIu32 "\n",
		request->iovec_i, request->iovec_offset, *address, *length);

	request->iovec_i++;
	request->iovec_offset = 0;
	return 0;
}


//! One command, waited for.
static status_t
do_nvme_io_request(nvme_disk_driver_info* info, nvme_io_request* request)
{
	ReadLocker suspendLocker(info->suspend_lock);
	request->status = EINPROGRESS;

	prepare_sync_command(request);

	qpair_info* qpinfo = get_qpair(info);
	int ret = submit_command(info, qpinfo, request);
	if (ret != 0) {
		TRACE_ERROR("attempt to queue %s I/O at LBA %" B_PRIdOFF " of %" B_PRIuSIZE
			" blocks failed!\n", request->write ? "write" : "read",
			request->lba_start, request->lba_count);

		request->lba_count = 0;
		return ret;
	}

	await_command(info, qpinfo, request);

	if (request->status != B_OK) {
		TRACE_ERROR("%s at LBA %" B_PRIdOFF " of %" B_PRIuSIZE
			" blocks failed!\n", request->write ? "write" : "read",
			request->lba_start, request->lba_count);

		request->lba_count = 0;
	}
	return request->status;
}


static status_t
nvme_disk_bounced_io(nvme_disk_handle* handle, io_request* request,
	const char* reason)
{
	CALLED();

	// Bounced transfers are copied and go one command at a time: say how
	// often that happens (the first time, then at every power of two).
	const int64 bounced = atomic_add64(&handle->info->bounced, 1) + 1;
	if ((bounced & (bounced - 1)) == 0) {
		TRACE_ALWAYS("%" B_PRId64 " transfers bounced (this one: %" B_PRIdOFF
			", %" B_PRIuGENADDR " bytes, %s)\n", bounced, request->Offset(),
			request->Length(), reason);
	}

	WriteLocker writeLocker;
	if (request->IsWrite())
		writeLocker.SetTo(handle->info->rounded_write_lock, false);

	status_t status = acquire_sem(handle->info->dma_buffers_sem);
	if (status != B_OK) {
		request->SetStatusAndNotify(status);
		return status;
	}

	const size_t block_size = handle->info->block_size;

	TRACE("%p: IOR Offset: %" B_PRIdOFF "; Length %" B_PRIuGENADDR
		"; Write %s\n", request, request->Offset(), request->Length(),
		request->IsWrite() ? "yes" : "no");

	nvme_io_request nvme_request;
	while (request->RemainingBytes() > 0) {
		IOOperation operation;
		status = handle->info->dma_resource.TranslateNext(request, &operation, 0);
		if (status != B_OK)
			break;

		do {
			TRACE("%p: IOO offset: %" B_PRIdOFF ", length: %" B_PRIuGENADDR
				", write: %s\n", request, operation.Offset(),
				operation.Length(), operation.IsWrite() ? "yes" : "no");

			nvme_request.write = operation.IsWrite();
			nvme_request.lba_start = operation.Offset() / block_size;
			nvme_request.lba_count = operation.Length() / block_size;
			nvme_request.iovecs = (physical_entry*)operation.Vecs();
			nvme_request.iovec_count = operation.VecCount();

			status = do_nvme_io_request(handle->info, &nvme_request);

			operation.SetStatus(status,
				status == B_OK ? operation.Length() : 0);
		} while (status == B_OK && !operation.Finish());

		if (status == B_OK && operation.Status() != B_OK) {
			TRACE_ERROR("I/O succeeded but IOOperation failed!\n");
			status = operation.Status();
		}

		request->OperationFinished(&operation);

		handle->info->dma_resource.RecycleBuffer(operation.Buffer());

		TRACE("%p: status %s, remaining bytes %" B_PRIuGENADDR "\n", request,
			strerror(status), request->RemainingBytes());
		if (status != B_OK)
			break;
	}

	release_sem(handle->info->dma_buffers_sem);

	// Notify() also takes care of UnlockMemory().
	if (status != B_OK && request->Status() == B_OK)
		request->SetStatusAndNotify(status);
	else
		request->NotifyFinished();
	return status;
}


/*!	Copies \a count physical vectors to \a out (if not NULL), cutting those
	larger than \a maxBytes into pieces that end on page boundaries, so that
	all but the first piece of a vector start on one. Returns how many
	vectors that makes.
*/
static int32
split_vectors(const physical_entry* in, int32 count, size_t maxBytes,
	physical_entry* out)
{
	int32 pieces = 0;
	for (int32 i = 0; i < count; i++) {
		phys_addr_t address = in[i].address;
		phys_size_t size = in[i].size;
		while (size > maxBytes) {
			phys_size_t piece = ROUNDDOWN(address + maxBytes, B_PAGE_SIZE)
				- address;
			if (out != NULL) {
				out[pieces].address = address;
				out[pieces].size = piece;
			}
			pieces++;
			address += piece;
			size -= piece;
		}
		if (out != NULL) {
			out[pieces].address = address;
			out[pieces].size = size;
		}
		pieces++;
	}
	return pieces;
}


/*!	Splits a transfer that needs no bouncing into commands and sends them
	all off at once; the request is completed (by whoever sees it) when the last
	of them has. Fails only when there is no memory to track it, and then
	nothing has been sent.
*/
static status_t
do_async_io(nvme_disk_driver_info* info, io_request* request,
	const nvme_io_request& transfer, off_t start)
{
	const size_t block_size = info->block_size;
	const uint32 max_io_blocks = info->max_io_blocks;
	const size_t maxBytes = (size_t)max_io_blocks * block_size;

	// vectors larger than a command, cut at page boundaries
	const int32 vecCount = split_vectors(transfer.iovecs, transfer.iovec_count,
		maxBytes, NULL);

	// count the commands, split as the synchronous path splits them
	int32 commands = 0;
	{
		physical_entry piece;
		int32 pieces = 0;
		int32 inCommand = 0;
		uint32 blocks = 0;
		for (int32 v = 0; v < transfer.iovec_count; v++) {
			phys_addr_t address = transfer.iovecs[v].address;
			phys_size_t size = transfer.iovecs[v].size;
			while (size > 0) {
				piece.size = size > maxBytes
					? ROUNDDOWN(address + maxBytes, B_PAGE_SIZE) - address : size;
				uint32 newBlocks = blocks + piece.size / block_size;
				if (inCommand == NVME_MAX_SGL_DESCRIPTORS / 2
					|| (blocks > 0 && newBlocks > max_io_blocks)) {
					commands++;
					inCommand = 0;
					newBlocks = piece.size / block_size;
				}
				inCommand++;
				blocks = newBlocks;
				pieces++;
				address += piece.size;
				size -= piece.size;
			}
		}
		if (inCommand > 0)
			commands++;
		ASSERT(pieces == vecCount);
	}

	const size_t size = sizeof(nvme_async_io)
		+ commands * sizeof(nvme_io_request)
		+ vecCount * sizeof(physical_entry);
	nvme_async_io* io = (nvme_async_io*)malloc(size);
	if (io == NULL)
		return B_NO_MEMORY;

	physical_entry* vecs = (physical_entry*)&io->commands[commands];
	split_vectors(transfer.iovecs, transfer.iovec_count, maxBytes, vecs);

	io->info = info;
	io->request = request;
	io->start = start;
	io->length = request->Length();
	io->pending = 1;
	B_INITIALIZE_SPINLOCK(&io->lock);
	io->status = B_OK;
	io->failed = INT64_MAX;
	io->write_locked = transfer.write;
	io->next = NULL;
	io->command_count = commands;

	// Held until the last command completes: a bounced write must not
	// read-modify-write blocks under these, and a suspend must wait for them.
	if (transfer.write)
		rw_lock_read_lock(&info->rounded_write_lock);
	rw_lock_read_lock(&info->suspend_lock);

	qpair_info* qpinfo = get_qpair(info);
	off_t lba = start / block_size;
	int32 index = 0;
	for (int32 c = 0; c < commands; c++) {
		nvme_io_request* command = &io->commands[c];
		memset(command, 0, sizeof(nvme_io_request));
		command->write = transfer.write;
		command->iovecs = vecs + index;
		command->iovec_count = min_c(vecCount - index,
			NVME_MAX_SGL_DESCRIPTORS / 2);
		command->lba_start = lba;
		command->lba_count = 0;
		for (int32 i = 0; i < command->iovec_count; i++) {
			uint32 newCount = command->lba_count
				+ command->iovecs[i].size / block_size;
			if (command->lba_count > 0 && newCount > max_io_blocks) {
				command->iovec_count = i;
				break;
			}
			command->lba_count = newCount;
		}
		command->status = EINPROGRESS;
		command->async = io;

		atomic_add(&io->pending, 1);
		int ret = submit_command(info, qpinfo, command);
		if (ret != 0) {
			TRACE_ERROR("attempt to queue %s I/O at LBA %" B_PRIdOFF " of %"
				B_PRIuSIZE " blocks failed!\n", transfer.write ? "write" : "read",
				command->lba_start, command->lba_count);
			atomic_add(&io->pending, -1);
			InterruptsSpinLocker locker(io->lock);
			io->status = ret < 0 ? ret : B_IO_ERROR;
			io->failed = min_c(io->failed, lba * (off_t)block_size);
			break;
		}

		index += command->iovec_count;
		lba += command->lba_count;
	}

	// the submitter's reference
	if (atomic_add(&io->pending, -1) == 1)
		async_io_done(io);
	return B_OK;
}


/*!	\a waited: the caller waits for the request (the read and write hooks):
	what fits in one command is then done synchronously, which the interrupt
	completes without waking another thread.
*/
static status_t
do_io(nvme_disk_handle* handle, io_request* request, bool waited)
{
	CALLED();

	const off_t ns_end = (handle->info->capacity * handle->info->block_size);
	if ((request->Offset() + (off_t)request->Length()) > ns_end) {
		request->SetStatusAndNotify(ERANGE);
		return ERANGE;
	}

	nvme_io_request nvme_request;
	memset(&nvme_request, 0, sizeof(nvme_io_request));

	nvme_request.write = request->IsWrite();

	physical_entry* vtophys = NULL;
	MemoryDeleter vtophysDeleter;

	IOBuffer* buffer = request->Buffer();
	status_t status = B_OK;
	if (!buffer->IsPhysical()) {
		status = buffer->LockMemory(request->TeamID(), request->IsWrite());
		if (status != B_OK) {
			TRACE_ERROR("failed to lock memory: %s\n", strerror(status));
			request->SetStatusAndNotify(status);
			return status;
		}
		// SetStatusAndNotify() takes care of unlocking memory if necessary.

		const int32 vtophysLength = (request->Length() / B_PAGE_SIZE) + 2;
		if (vtophysLength <= 8) {
			vtophys = (physical_entry*)alloca(sizeof(physical_entry) * vtophysLength);
		} else {
			vtophys = (physical_entry*)malloc(sizeof(physical_entry) * vtophysLength);
			vtophysDeleter.SetTo(vtophys);
		}
		if (vtophys == NULL) {
			TRACE_ERROR("failed to allocate memory for iovecs\n");
			request->SetStatusAndNotify(B_NO_MEMORY);
			return B_NO_MEMORY;
		}

		for (size_t i = 0; i < buffer->VecCount(); i++) {
			generic_io_vec virt = buffer->VecAt(i);
			uint32 entries = vtophysLength - nvme_request.iovec_count;

			// Avoid copies by going straight into the vtophys array.
			status = get_memory_map_etc(request->TeamID(), (void*)virt.base,
				virt.length, vtophys + nvme_request.iovec_count, &entries);

			if (status == B_BAD_VALUE && entries == 0)
				status = B_BUFFER_OVERFLOW;
			if (status == B_BUFFER_OVERFLOW) {
				// Too many physical_entries to use unbounced I/O.
				vtophysDeleter.Delete();
				vtophys = NULL;
				break;
			}
			if (status != B_OK) {
				TRACE_ERROR("I/O get_memory_map failed: %s\n", strerror(status));
				request->SetStatusAndNotify(status);
				return status;
			}

			nvme_request.iovec_count += entries;
		}

		nvme_request.iovecs = vtophys;
	} else {
		nvme_request.iovecs = (physical_entry*)buffer->Vecs();
		nvme_request.iovec_count = buffer->VecCount();
	}

	// See if we need to bounce anything other than the first or last vec.
	const size_t block_size = handle->info->block_size;
	bool bounceAll = (nvme_request.iovecs == NULL);
	const char* reason = "too many physical vectors";
	// A single physically contiguous vector can be much larger than the
	// controller's maximum transfer (a run of cache pages often is). The
	// asynchronous path cuts it into pieces; the synchronous one, taken
	// only without memory, splits between vectors and bounces such a
	// vector through the bounded DMA translation path.
	const size_t maxIOBytes = (size_t)handle->info->max_io_blocks * block_size;
	bool tooLarge = false;
	for (int32 i = 0; !bounceAll && i < nvme_request.iovec_count; i++) {
		if (nvme_request.iovecs[i].size > maxIOBytes) {
			tooLarge = true;
			// (cut at page boundaries, the pieces must be whole blocks)
			if ((nvme_request.iovecs[i].address % block_size) != 0) {
				bounceAll = true;
				reason = "unaligned large vector";
			}
		}
	}
	for (int32 i = 1; !bounceAll && i < (nvme_request.iovec_count - 1); i++) {
		if ((nvme_request.iovecs[i].address % B_PAGE_SIZE) != 0
			|| (nvme_request.iovecs[i].size % B_PAGE_SIZE) != 0) {
			bounceAll = true;
			reason = "unaligned vector";
		}
	}

	// See if we need to bounce due to the first or last vecs.
	if (nvme_request.iovec_count > 1) {
		// There are middle vecs, so the first and last vecs have different restrictions: they
		// need only be a multiple of the block size, and must end and start on a page boundary,
		// respectively, though the start address must always be 32-bit-aligned.
		physical_entry* entry = &nvme_request.iovecs[0];
		if (!bounceAll && (((entry->address + entry->size) % B_PAGE_SIZE) != 0
				|| (entry->address & 0x3) != 0 || (entry->size % block_size) != 0)) {
			bounceAll = true;
			reason = "unaligned first vector";
		}

		entry = &nvme_request.iovecs[nvme_request.iovec_count - 1];
		if (!bounceAll && ((entry->address % B_PAGE_SIZE) != 0
				|| (entry->size % block_size) != 0)) {
			bounceAll = true;
			reason = "unaligned last vector";
		}
	} else {
		// There is only one vec. Check that it is a multiple of the block size,
		// and that its address is 32-bit-aligned.
		physical_entry* entry = &nvme_request.iovecs[0];
		if (!bounceAll && ((entry->address & 0x3) != 0 || (entry->size % block_size) != 0)) {
			bounceAll = true;
			reason = "unaligned buffer";
		}
	}

	// See if we need to bounce due to rounding.
	const off_t rounded_pos = ROUNDDOWN(request->Offset(), block_size);
	const phys_size_t rounded_len = ROUNDUP(request->Length() + (request->Offset()
		- rounded_pos), block_size);
	if (!bounceAll && (rounded_pos != request->Offset()
			|| rounded_len != request->Length())) {
		bounceAll = true;
		reason = "not whole blocks";
	}

	if (bounceAll) {
		// Let the bounced I/O routine take care of everything from here.
		return nvme_disk_bounced_io(handle, request, reason);
	}

	// No bouncing was required.
	// Error check before actually doing I/O.
	if (status != B_OK) {
		TRACE_ERROR("I/O failed early: %s\n", strerror(status));
		request->SetStatusAndNotify(status);
		return status;
	}

	// (A report on a completion thread may start more I/O, as an iterative
	// file system request continues: that stays asynchronous, for the thread
	// must not wait for its own queue. Waiting for I/O there is up to the
	// read and write hooks, whose waiters complete the queue themselves.)
	const bool oneCommand = !tooLarge && rounded_len <= maxIOBytes
		&& nvme_request.iovec_count <= NVME_MAX_SGL_DESCRIPTORS / 2;
	if (!(waited && oneCommand)
		&& do_async_io(handle->info, request, nvme_request, rounded_pos)
			== B_OK) {
		return B_OK;
	}

	// One command at a time, waited for (also without memory for the
	// asynchronous transfer).
	if (tooLarge)
		return nvme_disk_bounced_io(handle, request, "vector too large");
	ReadLocker readLocker;
	if (nvme_request.write)
		readLocker.SetTo(handle->info->rounded_write_lock, false);

	const uint32 max_io_blocks = handle->info->max_io_blocks;
	int32 remaining = nvme_request.iovec_count;
	nvme_request.lba_start = rounded_pos / block_size;
	while (remaining > 0) {
		nvme_request.iovec_count = min_c(remaining,
			NVME_MAX_SGL_DESCRIPTORS / 2);

		nvme_request.lba_count = 0;
		for (int i = 0; i < nvme_request.iovec_count; i++) {
			uint32 new_lba_count = nvme_request.lba_count
				+ (nvme_request.iovecs[i].size / block_size);
			if (nvme_request.lba_count > 0 && new_lba_count > max_io_blocks) {
				// We already have a nonzero length, and adding this vec would
				// make us go over (or we already are over.) Stop adding.
				nvme_request.iovec_count = i;
				break;
			}

			nvme_request.lba_count = new_lba_count;
		}

		status = do_nvme_io_request(handle->info, &nvme_request);
		if (status != B_OK)
			break;

		nvme_request.iovecs += nvme_request.iovec_count;
		remaining -= nvme_request.iovec_count;
		nvme_request.lba_start += nvme_request.lba_count;
	}

	if (status != B_OK)
		TRACE_ERROR("I/O failed: %s\n", strerror(status));

	readLocker.Unlock();

	request->SetTransferredBytes(status != B_OK,
		(nvme_request.lba_start * block_size) - rounded_pos);
	request->SetStatusAndNotify(status);
	return status;
}


static status_t
nvme_disk_io_request(nvme_disk_handle* handle, io_request* request,
	bool waited)
{
	MutexLocker requestOwnersLocker(handle->info->request_owners_lock);

	// Use a RequestOwner per-thread to avoid recursion due to
	// requests being queued inside other requests' notify callbacks.
	NVMeRequestOwner* existingOwner = handle->info->request_owners.Lookup(
		thread_get_current_thread_id());
	if (existingOwner != NULL) {
		existingOwner->requests_queue.Add(request);
		return B_OK;
	}

	NVMeRequestOwner owner;
	owner.team = thread_get_current_thread()->team->id;
	owner.thread = thread_get_current_thread_id();
	owner.priority = thread_get_io_priority(owner.thread);
	handle->info->request_owners.InsertUnchecked(&owner);
	requestOwnersLocker.Unlock();

	owner.requests_queue.Add(request);

	while (!owner.requests_queue.IsEmpty()) {
		request = owner.requests_queue.RemoveHead();
		// do_io() completes every request, including errors. Notification may
		// delete it through the caller's callback before do_io() returns.
		do_io(handle, request, waited);
		waited = false;
	}

	requestOwnersLocker.Lock();
	handle->info->request_owners.Remove(&owner);
	return B_OK;
}


static status_t
nvme_disk_io(void* cookie, io_request* request)
{
	CALLED();
	return nvme_disk_io_request((nvme_disk_handle*)cookie, request, false);
}


static status_t
nvme_disk_read(void* cookie, off_t pos, void* buffer, size_t* length)
{
	CALLED();
	nvme_disk_handle* handle = (nvme_disk_handle*)cookie;

	const off_t ns_end = (handle->info->capacity * handle->info->block_size);
	if (pos >= ns_end)
		return B_BAD_VALUE;
	if ((pos + (off_t)*length) > ns_end)
		*length = ns_end - pos;

	IORequest request;
	status_t status = request.Init(pos, (addr_t)buffer, *length, false, 0);
	if (status != B_OK)
		return status;

	status = nvme_disk_io_request(handle, &request, true);
	if (status == B_OK)
		status = request.Wait(0, 0);
	*length = request.TransferredBytes();
	return status;
}


static status_t
nvme_disk_write(void* cookie, off_t pos, const void* buffer, size_t* length)
{
	CALLED();
	nvme_disk_handle* handle = (nvme_disk_handle*)cookie;

	const off_t ns_end = (handle->info->capacity * handle->info->block_size);
	if (pos >= ns_end)
		return B_BAD_VALUE;
	if ((pos + (off_t)*length) > ns_end)
		*length = ns_end - pos;

	IORequest request;
	status_t status = request.Init(pos, (addr_t)buffer, *length, true, 0);
	if (status != B_OK)
		return status;

	status = nvme_disk_io_request(handle, &request, true);
	if (status == B_OK)
		status = request.Wait(0, 0);
	*length = request.TransferredBytes();
	return status;
}


static status_t
nvme_disk_flush(nvme_disk_driver_info* info)
{
	CALLED();
	ReadLocker suspendLocker(info->suspend_lock);

	nvme_io_request request = {};
	prepare_sync_command(&request);
	request.status = EINPROGRESS;

	qpair_info* qpinfo = get_qpair(info);
	request.qpair = qpinfo;
	command_submitted(qpinfo);
	int ret = nvme_ns_flush(info->ns, qpinfo->qpair,
		(nvme_cmd_cb)io_finished_callback, &request);
	if (ret != 0) {
		atomic_add(&qpinfo->outstanding, -1);
		return ret;
	}

	await_command(info, qpinfo, &request);
	return request.status;
}


static status_t
nvme_disk_trim(nvme_disk_driver_info* info, fs_trim_data* trimData)
{
	CALLED();
	trimData->trimmed_size = 0;

	if (info->block_size == 0
		|| info->capacity > uint64(INT64_MAX) / info->block_size
		|| trimData->range_count > NVME_DATASET_MANAGEMENT_MAX_RANGES)
		return B_BAD_VALUE;
	if (trimData->range_count == 0)
		return B_OK;

	const uint64 deviceSize = info->capacity * info->block_size;

	// Validate every input before submitting any deallocation. In particular,
	// a bad later range must not leave the earlier ranges partially trimmed.
	uint64 trimmingSize = 0;
	for (uint32 i = 0; i < trimData->range_count; i++) {
		uint64 lba, blocks;
		if (!nvme_normalize_trim_range(deviceSize, info->block_size,
				trimData->ranges[i].offset, trimData->ranges[i].size, lba, blocks)) {
			return B_BAD_VALUE;
		}
		uint64 length = blocks * info->block_size;
		if (trimmingSize > UINT64_MAX - length)
			return B_BAD_VALUE;
		trimmingSize += length;
	}
	if (trimmingSize == 0)
		return B_OK;

	struct nvme_ns_stat nsstat;
	int ret = nvme_ns_stat(info->ns, &nsstat);
	if (ret != 0)
		return ret;
	if ((nsstat.flags & NVME_NS_DEALLOCATE_SUPPORTED) == 0)
		return B_UNSUPPORTED;

	NVMeTrimBatch batch(nsstat.dsm_max_ranges, nsstat.dsm_max_range_blocks,
		nsstat.dsm_max_command_blocks);
	nvme_dsm_range* dsmRanges = (nvme_dsm_range*)nvme_mem_alloc_node(
		batch.MaxRanges() * sizeof(nvme_dsm_range), 0, 0, NULL);
	if (dsmRanges == NULL)
		return B_NO_MEMORY;
	CObjectDeleter<void, void, nvme_free> dsmRangesDeleter(dsmRanges);

	auto submitBatch = [&]() -> status_t {
		if (batch.Count() == 0)
			return B_OK;
		ReadLocker suspendLocker(info->suspend_lock);
		nvme_io_request request = {};
		prepare_sync_command(&request);
		request.status = EINPROGRESS;
		qpair_info* qpair = get_qpair(info);
		request.qpair = qpair;
		command_submitted(qpair);
		if (nvme_ns_deallocate(info->ns, qpair->qpair, dsmRanges, batch.Count(),
				(nvme_cmd_cb)io_finished_callback, &request) != 0) {
			atomic_add(&qpair->outstanding, -1);
			return B_IO_ERROR;
		}
		// Completion is required before this DMA buffer may be reused or freed.
		await_command(info, qpair, &request);
		status_t status = request.status;
		if (status == B_OK) {
			trimData->trimmed_size += batch.Blocks() * info->block_size;
			batch.Reset();
		}
		return status;
	};

	for (uint32 i = 0; i < trimData->range_count; i++) {
		uint64 lba, blocks;
		nvme_normalize_trim_range(deviceSize, info->block_size,
			trimData->ranges[i].offset, trimData->ranges[i].size, lba, blocks);
		while (blocks != 0) {
			uint32 count = batch.Add(blocks);
			if (count == 0) {
				status_t status = submitBatch();
				if (status != B_OK)
					return status;
				continue;
			}
			nvme_dsm_range& range = dsmRanges[batch.Count() - 1];
			range.attributes = 0;
			range.length = count;
			range.starting_lba = lba;
			lba += count;
			blocks -= count;
		}
	}
	return submitBatch();
}


/*!	Copies a space-padded, not necessarily terminated string from the
	controller's identify data (model or serial number) to a user buffer.
*/
static status_t
copy_identify_string(void* buffer, size_t length, const int8_t* field,
	size_t fieldLength)
{
	while (fieldLength > 0 && (field[fieldLength - 1] == ' '
			|| field[fieldLength - 1] == '\0'))
		fieldLength--;

	char string[NVME_MODEL_NUMBER_CHARACTERS + 1];
	fieldLength = min_c(fieldLength, sizeof(string) - 1);
	memcpy(string, field, fieldLength);
	string[fieldLength] = '\0';

	status_t status = user_strlcpy((char*)buffer, string, length);
	return status < B_OK ? status : B_OK;
}


static status_t
nvme_disk_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	CALLED();
	nvme_disk_handle* handle = (nvme_disk_handle*)cookie;
	nvme_disk_driver_info* info = handle->info;

	TRACE("ioctl(op = %" B_PRId32 ")\n", op);

	switch (op) {
		case B_GET_MEDIA_STATUS:
		{
			return user_memcpy(buffer, &info->media_status, sizeof(status_t));
		}

		case B_GET_DEVICE_SIZE:
		{
			size_t size = info->capacity * info->block_size;
			return user_memcpy(buffer, &size, sizeof(size_t));
		}

		case B_GET_GEOMETRY:
		{
			if (buffer == NULL || length > sizeof(device_geometry))
				return B_BAD_VALUE;

		 	device_geometry geometry;
			status_t status = get_geometry(handle, &geometry);
			if (status != B_OK)
				return status;

			return user_memcpy(buffer, &geometry, length);
		}

		case B_GET_DEVICE_NAME:
		{
			return copy_identify_string(buffer, length, info->ctrlr->cdata.mn,
				NVME_MODEL_NUMBER_CHARACTERS);
		}

		case B_GET_DEVICE_SERIAL_NUMBER:
		{
			return copy_identify_string(buffer, length, info->ctrlr->cdata.sn,
				NVME_SERIAL_NUMBER_CHARACTERS);
		}

		case B_GET_ICON_NAME:
			return user_strlcpy((char*)buffer, "devices/drive-harddisk",
				B_FILE_NAME_LENGTH);

		case B_GET_VECTOR_ICON:
		{
			device_icon iconData;
			if (length != sizeof(device_icon))
				return B_BAD_VALUE;
			if (user_memcpy(&iconData, buffer, sizeof(device_icon)) != B_OK)
				return B_BAD_ADDRESS;

			if (iconData.icon_size >= (int32)sizeof(kDriveIcon)) {
				if (user_memcpy(iconData.icon_data, kDriveIcon,
						sizeof(kDriveIcon)) != B_OK)
					return B_BAD_ADDRESS;
			}

			iconData.icon_size = sizeof(kDriveIcon);
			return user_memcpy(buffer, &iconData, sizeof(device_icon));
		}

		case B_FLUSH_DRIVE_CACHE:
			return nvme_disk_flush(info);

		case B_TRIM_DEVICE:
			ASSERT(IS_KERNEL_ADDRESS(buffer));
			return nvme_disk_trim(info, (fs_trim_data*)buffer);
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module API


static float
nvme_disk_supports_device(device_node *parent)
{
	CALLED();

	const char* bus;
	uint16 baseClass, subClass;

	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_TYPE, &baseClass, false) != B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_SUB_TYPE, &subClass, false) != B_OK)
		return -1.0f;

	if (strcmp(bus, "pci") != 0 || baseClass != PCI_mass_storage)
		return 0.0f;

	if (subClass != PCI_nvm)
		return 0.0f;

	TRACE("NVMe device found!\n");
	return 1.0f;
}


static status_t
nvme_disk_register_device(device_node* parent)
{
	CALLED();

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "NVMe Disk" } },
		{ NULL }
	};

	return sDeviceManager->register_node(parent, NVME_DISK_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
nvme_disk_init_driver(device_node* node, void** cookie)
{
	CALLED();

	int ret = nvme_lib_init((enum nvme_log_level)0, (enum nvme_log_facility)0, NULL);
	if (ret != 0) {
		TRACE_ERROR("libnvme initialization failed!\n");
		return ret;
	}

	nvme_disk_driver_info* info = new nvme_disk_driver_info;
	if (info == NULL)
		return B_NO_MEMORY;

	info->media_status = B_OK;
	info->node = node;

	info->ctrlr = NULL;
	info->suspended = false;

	*cookie = info;
	return B_OK;
}


static void
nvme_disk_uninit_driver(void* _cookie)
{
	CALLED();

	nvme_disk_driver_info* info = (nvme_disk_driver_info*)_cookie;
	free(info);
}


static volatile uint32*
msix_table(nvme_disk_driver_info* info)
{
	if (info->msix_table != NULL)
		return info->msix_table;

	uint8 capability;
	if (info->pci->find_pci_capability(info->pci_device_cookie,
			PCI_cap_id_msix, &capability) != B_OK) {
		return NULL;
	}
	uint32 table = info->pci->read_pci_config(info->pci_device_cookie,
		capability + PCI_msix_table, 4);
	uint8 bar = table & PCI_msix_bir_mask;
	pci_info& pciInfo = info->info;
	uint64 address = pciInfo.u.h0.base_registers[bar];
	if ((pciInfo.u.h0.base_register_flags[bar] & PCI_address_type)
			== PCI_address_type_64) {
		address |= (uint64)pciInfo.u.h0.base_registers[bar + 1] << 32;
	}
	address += table & PCI_msix_offset_mask;

	// all the entries we can use, 16 bytes each
	size_t size = ROUNDUP((address & (B_PAGE_SIZE - 1))
		+ (NVME_MAX_QPAIRS + 1) * 16, B_PAGE_SIZE);
	void* mapped;
	info->msix_area = map_physical_memory("nvme msi-x table",
		address & ~(B_PAGE_SIZE - 1), size, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &mapped);
	if (info->msix_area < 0)
		return NULL;
	info->msix_table = (volatile uint32*)((addr_t)mapped
		+ (address & (B_PAGE_SIZE - 1)));
	return info->msix_table;
}


static void
save_msix_vector(nvme_disk_driver_info* info)
{
	volatile uint32* table = msix_table(info);
	if (table == NULL)
		return;
	uint32 entries = max_c(info->msix_count, 1) * 4;
	for (uint32 i = 0; i < entries; i++)
		info->msix_saved[i] = table[i];
}


static void
restore_msix_vector(nvme_disk_driver_info* info)
{
	volatile uint32* table = msix_table(info);
	if (table == NULL)
		return;
	uint32 count = max_c(info->msix_count, 1);
	for (uint32 entry = 0; entry < count; entry++) {
		volatile uint32* vector = table + entry * 4;
		const uint32* saved = info->msix_saved + entry * 4;
		if (saved[0] == 0)
			continue;
		if (vector[0] == saved[0] && vector[2] == saved[2])
			continue;
		// masked while it is written, as the PCI specification asks
		vector[3] = saved[3] | 1;
		for (int i = 0; i < 3; i++)
			vector[i] = saved[i];
		vector[3] = saved[3];
		device_manager_suspend_trace("nvme: MSI-X vector %" B_PRIu32
			" written back (%#" B_PRIx32 "/%#" B_PRIx32 ")", entry, vector[0],
			vector[2]);
	}
}


/*!	Takes the queues down before the controller is shut down: with all I/O
	done (the suspend lock is write locked), the completion threads (which
	look at their queues on their own) must stay away from them until the
	controller and its queues are set up again.
*/
static void
take_queues_down(nvme_disk_driver_info* info)
{
	atomic_set(&info->queues_down, 1);
	while (atomic_get(&info->pollers) > 0)
		snooze(100);
}


static status_t
nvme_disk_suspend(void* _cookie, int32 state)
{
	nvme_disk_driver_info* info = (nvme_disk_driver_info*)_cookie;
	if (info->ctrlr == NULL)
		return B_OK;

	// Suspend to idle keeps the controller powered; autonomous power state
	// transitions already take it to its lowest power state when it idles.
	// With "s2idle_d3", the function goes to D3hot as well, shut down first
	// and reset when it is back, as Linux' "simple suspend": in the SoC's
	// deepest state (S0i3) it loses its power, and its queues with it.
	if (state == ACPI_POWER_STATE_ON) {
		// (read again, so that a change applies to the next sleep)
		void* settings = load_driver_settings("nvme_disk");
		info->idle_d3 = get_driver_boolean_parameter(settings, "s2idle_d3",
			idle_d3_default(), true);
		unload_driver_settings(settings);
		if (!info->idle_d3 || info->pci->suspend_device == NULL)
			return B_OK;

		nvme_disk_flush(info);
		rw_lock_write_lock(&info->suspend_lock);
		take_queues_down(info);
		save_msix_vector(info);
		nvme_ctrlr_suspend(info->ctrlr);
		status_t status = info->pci->suspend_device(info->pci_device_cookie,
			PCI_pm_state_d3);
		if (status != B_OK) {
			TRACE_ALWAYS("no D3 while suspended to idle: %s\n",
				strerror(status));
		} else
			info->in_idle_d3 = true;
		info->suspended = true;
		return B_OK;
	}

	// Wait for pending commands; new ones are blocked until resumed.
	rw_lock_write_lock(&info->suspend_lock);
	take_queues_down(info);
	save_msix_vector(info);
		// (a device that loses its power comes back with an empty table)

	nvme_ctrlr_suspend(info->ctrlr);
	info->suspended = true;
	return B_OK;
}


static status_t
nvme_disk_resume(void* _cookie)
{
	nvme_disk_driver_info* info = (nvme_disk_driver_info*)_cookie;
	if (info->ctrlr != NULL && info->in_idle_d3) {
		info->in_idle_d3 = false;
		device_manager_suspend_trace("nvme: to D0");
		status_t status = info->pci->resume_device(info->pci_device_cookie);
		device_manager_suspend_trace("nvme: in D0: %s", strerror(status));
		if (status != B_OK)
			TRACE_ERROR("back from D3: %s\n", strerror(status));
	}
	if (info->ctrlr == NULL || !info->suspended)
		return B_OK;
	info->suspended = false;

	status_t status = B_OK;
	int result = nvme_ctrlr_resume(info->ctrlr);
	device_manager_suspend_trace("nvme: controller reset returned %d", result);
	if (result != 0) {
		TRACE_ERROR("resuming the controller failed!\n");
		status = B_IO_ERROR;
	} else {
		restore_msix_vector(info);
		nvme_disk_configure_features(info);
	}

	// (also after a failed resume: commands fail then, they do not hang)
	atomic_set(&info->queues_down, 0);
	rw_lock_write_unlock(&info->suspend_lock);
	return status;
}


static status_t
nvme_disk_register_child_devices(void* _cookie)
{
	CALLED();

	nvme_disk_driver_info* info = (nvme_disk_driver_info*)_cookie;
	status_t status;

	int32 id = sDeviceManager->create_id(NVME_DISK_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "disk/nvme/%" B_PRId32 "/raw",
		id);

	status = sDeviceManager->publish_device(info->node, name,
		NVME_DISK_DEVICE_MODULE_NAME);
	if (status != B_OK)
		sDeviceManager->free_id(NVME_DISK_DEVICE_ID_GENERATOR, id);
	return status;
}


//	#pragma mark -


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ NULL }
};

struct device_module_info sNvmeDiskDevice = {
	{
		NVME_DISK_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	nvme_disk_init_device,
	nvme_disk_uninit_device,
	NULL, // remove,

	nvme_disk_open,
	nvme_disk_close,
	nvme_disk_free,
	nvme_disk_read,
	nvme_disk_write,
	nvme_disk_io,
	nvme_disk_ioctl,

	NULL,	// select
	NULL,	// deselect
};

struct driver_module_info sNvmeDiskDriver = {
	{
		NVME_DISK_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	nvme_disk_supports_device,
	nvme_disk_register_device,
	nvme_disk_init_driver,
	nvme_disk_uninit_driver,
	nvme_disk_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
	nvme_disk_suspend,
	nvme_disk_resume,
};

module_info* modules[] = {
	(module_info*)&sNvmeDiskDriver,
	(module_info*)&sNvmeDiskDevice,
	NULL
};
