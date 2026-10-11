/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef POWERVR_LINUX_COMPAT_SCHED_H
#define POWERVR_LINUX_COMPAT_SCHED_H


/*	The part of Linux's DRM core that drm/imagination's job code (pvr_job.c,
	pvr_queue.c, pvr_sync.c) is built on, reimplemented small: work queues,
	reservation objects, drm_exec, the GPU scheduler with one entity per
	scheduler, fence unwrapping and the sync object calls. Included by
	linux_compat.h; linux_compat_sched.c implements it, the sync objects
	live in glue/pvr_haiku_sync.c. */


/* #pragma mark - work queues */


/*	Each work queue runs its items one at a time on its own kernel thread,
	in the order they were queued. An item queued while it is pending is
	not queued twice; one queued while it runs runs again afterwards. */

struct work_struct;
typedef void (*work_func_t)(struct work_struct* work);

struct work_struct {
	work_func_t					func;
	struct list_head			entry;
	struct workqueue_struct*	queue;		/* where it is pending */
};

/*	A work item queued once its time comes; the queue's thread keeps the
	timers. */
struct delayed_work {
	struct work_struct			work;
	struct list_head			timer;		/* armed while linked */
	bigtime_t					when;
	struct workqueue_struct*	timer_queue;	/* the last one armed on */
};

#define INIT_WORK(work_, func_) \
	do { \
		(work_)->func = (func_); \
		INIT_LIST_HEAD(&(work_)->entry); \
		(work_)->queue = NULL; \
	} while (0)

#define WQ_UNBOUND		(1u << 1)
#define WQ_MEM_RECLAIM	(1u << 3)
#define WQ_HIGHPRI		(1u << 4)

#define INIT_DELAYED_WORK(dwork_, func_) \
	do { \
		INIT_WORK(&(dwork_)->work, (func_)); \
		INIT_LIST_HEAD(&(dwork_)->timer); \
		(dwork_)->timer_queue = NULL; \
	} while (0)
#define to_delayed_work(work_)	container_of(work_, struct delayed_work, work)

struct workqueue_struct* alloc_workqueue(const char* name, unsigned int flags,
	int maxActive, ...);
void destroy_workqueue(struct workqueue_struct* queue);
bool queue_work(struct workqueue_struct* queue, struct work_struct* work);
/* Removes the item if it is pending and waits for it if it runs. */
bool cancel_work_sync(struct work_struct* work);
void flush_workqueue(struct workqueue_struct* queue);
/* false if it is armed or pending already */
bool queue_delayed_work(struct workqueue_struct* queue,
	struct delayed_work* work, unsigned long delay);
/* arms it anew (pending items stay pending) */
bool mod_delayed_work(struct workqueue_struct* queue,
	struct delayed_work* work, unsigned long delay);
bool cancel_delayed_work_sync(struct delayed_work* work);


/* #pragma mark - reservation objects */


/*	A buffer's fences by use (dma_resv). Callers serialize through drm_exec
	(one submission at a time), so dma_resv_lock() only orders memory. */

enum dma_resv_usage {
	DMA_RESV_USAGE_KERNEL,
	DMA_RESV_USAGE_WRITE,
	DMA_RESV_USAGE_READ,
	DMA_RESV_USAGE_BOOKKEEP
};

/* what a job that reads (or writes) has to wait for */
static inline enum dma_resv_usage
dma_resv_usage_rw(bool write)
{
	return write ? DMA_RESV_USAGE_READ : DMA_RESV_USAGE_WRITE;
}

void dma_resv_init(struct dma_resv* resv);
void dma_resv_fini(struct dma_resv* resv);
int dma_resv_lock(struct dma_resv* resv, void* context);
void dma_resv_unlock(struct dma_resv* resv);
int dma_resv_reserve_fences(struct dma_resv* resv, unsigned int count);
/* Replaces the fence of the same context, if there is one. */
void dma_resv_add_fence(struct dma_resv* resv, struct dma_fence* fence,
	enum dma_resv_usage usage);


/* #pragma mark - drm_exec */


/*	drm_exec locks the objects of a submission against other submissions;
	here one lock covers all of them (submissions are short and do not
	block), so there is never contention to retry. */

#define DRM_EXEC_INTERRUPTIBLE_WAIT	(1u << 0)
#define DRM_EXEC_IGNORE_DUPLICATES	(1u << 1)

struct drm_exec {
	u32		flags;
	bool	locked;
	bool	once;
};

void drm_exec_init(struct drm_exec* exec, u32 flags, unsigned int count);
void drm_exec_fini(struct drm_exec* exec);
int drm_exec_lock_obj(struct drm_exec* exec, struct drm_gem_object* object);
int drm_exec_prepare_obj(struct drm_exec* exec, struct drm_gem_object* object,
	unsigned int fenceCount);

/* runs the block once */
#define drm_exec_until_all_locked(exec) \
	for ((exec)->once = true; (exec)->once; (exec)->once = false)
#define drm_exec_retry_on_contention(exec)	do { (void)(exec); } while (0)


/* #pragma mark - the GPU scheduler */


/*	drm_sched with what pvr_queue.c uses: one entity per scheduler, jobs run
	in push order once their dependencies (and the driver's prepare_job()
	fences) have signaled and credits are free; the hardware fence that
	run_job() returns signals the job's finished fence; finished jobs are
	freed in order from the pending list. Work happens on the scheduler's
	work queue.

	Timeouts: when no job of a scheduler finishes for its timeout while
	one is pending, lx_sched_timeout_hook is called if the driver set it
	(the powervr driver resets the GPU there), otherwise the backend's
	timedout_job(). */

struct drm_gpu_scheduler;
struct drm_sched_entity;
struct drm_sched_job;

enum drm_sched_priority {
	DRM_SCHED_PRIORITY_KERNEL,
	DRM_SCHED_PRIORITY_HIGH,
	DRM_SCHED_PRIORITY_NORMAL,
	DRM_SCHED_PRIORITY_LOW,
	DRM_SCHED_PRIORITY_COUNT
};

enum drm_gpu_sched_stat {
	DRM_GPU_SCHED_STAT_NONE,
	DRM_GPU_SCHED_STAT_RESET,
	DRM_GPU_SCHED_STAT_ENODEV,
	DRM_GPU_SCHED_STAT_NO_HANG
};

struct drm_sched_fence {
	struct dma_fence			scheduled;
	struct dma_fence			finished;
	struct dma_fence*			parent;		/* the hardware fence */
	struct drm_gpu_scheduler*	sched;
	void*						owner;
};

struct drm_sched_job {
	struct drm_gpu_scheduler*	sched;
	struct drm_sched_entity*	entity;
	struct drm_sched_fence*		s_fence;
	struct list_head			list;			/* in the pending list */
	struct list_head			queue_link;		/* in the entity's queue */
	u32							credits;
	struct xarray				dependencies;
	unsigned long				last_dependency;
	struct dma_fence_cb			cb;				/* on the parent fence */
	u64							drm_client_id;
	bigtime_t					run_time;		/* handed to run_job() */
};

struct drm_sched_backend_ops {
	struct dma_fence*			(*prepare_job)(struct drm_sched_job* job,
									struct drm_sched_entity* entity);
	struct dma_fence*			(*run_job)(struct drm_sched_job* job);
	enum drm_gpu_sched_stat		(*timedout_job)(struct drm_sched_job* job);
	void						(*free_job)(struct drm_sched_job* job);
};

struct drm_sched_init_args {
	const struct drm_sched_backend_ops*	ops;
	struct workqueue_struct*			submit_wq;
	struct workqueue_struct*			timeout_wq;
	u32									num_rqs;
	u32									credit_limit;
	unsigned int						hang_limit;
	long								timeout;
	atomic_t*							score;
	const char*							name;
	struct device*						dev;
};

struct drm_gpu_scheduler {
	const struct drm_sched_backend_ops*	ops;
	struct device*						dev;
	const char*							name;
	u32									credit_limit;
	atomic_t							credit_count;
	struct workqueue_struct*			submit_wq;
	struct work_struct					work_run_job;
	struct work_struct					work_free_job;
	struct list_head					pending_list;
	spinlock_t							job_list_lock;
	struct drm_sched_entity*			entity;
	bool								pause_submit;
	bool								ready;
	long								timeout;	/* jiffies */
	struct workqueue_struct*			timeout_wq;
	struct delayed_work					work_tdr;
	bool								dead;		/* lx_sched_kill_all() */
};

/* When set, the timeout of schedulers created from then on, in ms. */
extern unsigned int lx_sched_timeout_override_ms;

/* Called when a job that went to run_job() is done, with the time since. */
extern void (*lx_sched_job_time_hook)(struct drm_gpu_scheduler* sched,
	bigtime_t elapsed);

/* Called on the timeout work queue for a job that stopped making progress. */
extern void (*lx_sched_timeout_hook)(struct drm_gpu_scheduler* sched,
	struct drm_sched_job* job);

struct drm_sched_entity {
	struct drm_gpu_scheduler*	sched;
	spinlock_t					lock;
	struct list_head			job_queue;
	u64							fence_context;
	atomic_t					fence_seq;
	atomic_t*					guilty;
	struct dma_fence*			dependency;	/* waited on, with its cb */
	struct dma_fence_cb			cb;
	bool						stopped;
};

int drm_sched_init(struct drm_gpu_scheduler* sched,
	const struct drm_sched_init_args* args);
void drm_sched_fini(struct drm_gpu_scheduler* sched);
void drm_sched_stop(struct drm_gpu_scheduler* sched,
	struct drm_sched_job* bad);
void drm_sched_start(struct drm_gpu_scheduler* sched, int error);
/*	For a lost device: stops the scheduler for good; jobs handed on finish
	with \a error, queued and new ones are killed with it. Call it on the
	scheduler's work queue. */
void lx_sched_kill_all(struct drm_gpu_scheduler* sched, int error);

int drm_sched_entity_init(struct drm_sched_entity* entity,
	enum drm_sched_priority priority, struct drm_gpu_scheduler** schedList,
	unsigned int schedCount, atomic_t* guilty);
void drm_sched_entity_fini(struct drm_sched_entity* entity);
void drm_sched_entity_destroy(struct drm_sched_entity* entity);
void drm_sched_entity_push_job(struct drm_sched_job* job);

int drm_sched_job_init(struct drm_sched_job* job,
	struct drm_sched_entity* entity, u32 credits, void* owner,
	u64 drmClientID);
void drm_sched_job_arm(struct drm_sched_job* job);
/* Takes over the caller's reference to \a fence. */
int drm_sched_job_add_dependency(struct drm_sched_job* job,
	struct dma_fence* fence);
int drm_sched_job_add_resv_dependencies(struct drm_sched_job* job,
	struct dma_resv* resv, enum dma_resv_usage usage);
bool drm_sched_job_has_dependency(struct drm_sched_job* job,
	struct dma_fence* fence);
void drm_sched_job_cleanup(struct drm_sched_job* job);

/* The scheduler fence \a fence belongs to, or NULL. */
struct drm_sched_fence* to_drm_sched_fence(struct dma_fence* fence);


/* #pragma mark - fence unwrapping and chains */


/*	dma_fence_unwrap_for_each() visits the fences a fence stands for: those
	of an lx_dma_fence_all() fence, or the fence itself. The sync objects
	do not build dma_fence_chains, so a chain node is only the allocation
	pvr_sync.c passes to drm_syncobj_add_point(). */

struct dma_fence_unwrap {
	struct dma_fence*	head;
	u32					index;
};

struct dma_fence* lx_dma_fence_unwrap_first(struct dma_fence* head,
	struct dma_fence_unwrap* cursor);
struct dma_fence* lx_dma_fence_unwrap_next(struct dma_fence_unwrap* cursor);

#define dma_fence_unwrap_for_each(fence, cursor, head) \
	for ((fence) = lx_dma_fence_unwrap_first((head), (cursor)); \
		(fence) != NULL; (fence) = lx_dma_fence_unwrap_next(cursor))

struct dma_fence_chain {
	struct dma_fence	base;
};

static inline struct dma_fence_chain*
dma_fence_chain_alloc(void)
{
	return (struct dma_fence_chain*)kzalloc(sizeof(struct dma_fence_chain),
		GFP_KERNEL);
}

static inline void
dma_fence_chain_free(struct dma_fence_chain* chain)
{
	kfree(chain);
}


/* #pragma mark - sync objects (glue/pvr_haiku_sync.c) */


struct drm_syncobj;

struct drm_syncobj* drm_syncobj_find(struct drm_file* file, u32 handle);
void drm_syncobj_put(struct drm_syncobj* syncobj);
int drm_syncobj_find_fence(struct drm_file* file, u32 handle, u64 point,
	u64 flags, struct dma_fence** _fence);
/* \a chain is freed; the point takes its own reference to \a fence. */
void drm_syncobj_add_point(struct drm_syncobj* syncobj,
	struct dma_fence_chain* chain, struct dma_fence* fence, u64 point);
void drm_syncobj_replace_fence(struct drm_syncobj* syncobj,
	struct dma_fence* fence);


#endif	/* POWERVR_LINUX_COMPAT_SCHED_H */
