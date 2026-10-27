/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	The Linux API of linux_compat.h that needs no Haiku C++: logging,
	errors, drm_mm, the minimal dma_fence, drmm allocations, debugfs,
	seq_file and module parameters. linux_compat_haiku.cpp has the rest. */


#include "linux_compat.h"


const volatile void* volatile lx_mmio_trace_base;
u64 lx_dma_mask = DMA_BIT_MASK(32);


/* #pragma mark - printing */


void
lx_log(int level, const char* format, ...)
{
	char buffer[256];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (length < 0)
		return;

	// Linux messages end in a newline, mostly; dprintf() lines must.
	size_t end = strnlen(buffer, sizeof(buffer));
	while (end > 0 && buffer[end - 1] == '\n')
		buffer[--end] = '\0';

	const char* prefix = "";
	if (level == LX_LOG_ERROR)
		prefix = "error: ";
	else if (level == LX_LOG_WARNING)
		prefix = "warning: ";
	dprintf("powervr: %s%s\n", prefix, buffer);
}


void
lx_warn_on(const char* file, int line, const char* condition)
{
	const char* name = strrchr(file, '/');
	dprintf("powervr: warning: %s at %s:%d\n", condition,
		name != NULL ? name + 1 : file, line);
}


/* #pragma mark - DRM device */


struct lx_managed {
	struct list_head	link;
	u64					data[];
};


void
lx_drm_dev_init(struct drm_device* drm, struct device* device)
{
	drm->dev = device;
	INIT_LIST_HEAD(&drm->managed);
}


void
lx_drm_dev_release(struct drm_device* drm)
{
	while (!list_empty(&drm->managed)) {
		struct lx_managed* allocation = list_first_entry(&drm->managed,
			struct lx_managed, link);
		list_del(&allocation->link);
		free(allocation);
	}
}


void*
drmm_kzalloc(struct drm_device* drm, size_t size, gfp_t flags)
{
	(void)flags;
	struct lx_managed* allocation = (struct lx_managed*)calloc(1,
		sizeof(struct lx_managed) + size);
	if (allocation == NULL)
		return NULL;
	list_add_tail(&allocation->link, &drm->managed);
	return allocation->data;
}


/* #pragma mark - drm_mm */


void
drm_mm_init(struct drm_mm* mm, u64 start, u64 size)
{
	mm->start = start;
	mm->size = size;
	INIT_LIST_HEAD(&mm->nodes);
}


void
drm_mm_takedown(struct drm_mm* mm)
{
	if (!list_empty(&mm->nodes))
		lx_log(LX_LOG_WARNING, "drm_mm_takedown: nodes left in the heap");
	INIT_LIST_HEAD(&mm->nodes);
}


/*!	Links \a node into the sorted node list, in front of \a next (or at
	the end, if \a next is the list head).
*/
static void
drm_mm_link(struct drm_mm* mm, struct drm_mm_node* node,
	struct list_head* next)
{
	(void)mm;
	list_add_tail(&node->link, next);
	node->allocated = true;
}


int
drm_mm_insert_node_in_range(struct drm_mm* mm, struct drm_mm_node* node,
	u64 size, u64 alignment, unsigned long color, u64 rangeStart,
	u64 rangeEnd, enum drm_mm_insert_mode mode)
{
	(void)color;
	(void)mode;
	if (size == 0 || node->allocated)
		return -EINVAL;
	if (alignment == 0)
		alignment = 1;

	u64 start = max(rangeStart, mm->start);
	u64 end = min(rangeEnd, mm->start + mm->size);

	// first fit: walk the holes in front of each node, then the last one
	u64 holeStart = mm->start;
	struct list_head* position;
	for (position = mm->nodes.next;; position = position->next) {
		u64 holeEnd = mm->start + mm->size;
		if (position != &mm->nodes) {
			holeEnd = list_entry(position, struct drm_mm_node, link)->start;
		}

		u64 candidate = max(holeStart, start);
		candidate = (candidate + alignment - 1) / alignment * alignment;
		if (candidate + size > candidate
			&& candidate + size <= min(holeEnd, end)) {
			node->start = candidate;
			node->size = size;
			drm_mm_link(mm, node, position);
			return 0;
		}

		if (position == &mm->nodes)
			break;
		struct drm_mm_node* current = list_entry(position, struct drm_mm_node,
			link);
		holeStart = current->start + current->size;
	}
	return -ENOSPC;
}


int
drm_mm_reserve_node(struct drm_mm* mm, struct drm_mm_node* node)
{
	u64 end = node->start + node->size;
	if (node->size == 0 || node->allocated || end < node->start
		|| node->start < mm->start || end > mm->start + mm->size) {
		return -ENOSPC;
	}

	struct list_head* position;
	list_for_each(position, &mm->nodes) {
		struct drm_mm_node* current = list_entry(position, struct drm_mm_node,
			link);
		if (current->start >= end)
			break;
		if (current->start + current->size > node->start)
			return -ENOSPC;
	}
	drm_mm_link(mm, node, position);
	return 0;
}


void
drm_mm_remove_node(struct drm_mm_node* node)
{
	if (!node->allocated)
		return;
	list_del(&node->link);
	node->allocated = false;
}


/* #pragma mark - dma_fence */


wait_queue_head_t lx_fence_queue;

static spinlock sFenceLock = B_SPINLOCK_INITIALIZER;
static struct dma_fence sStubFence;


static const char*
lx_fence_driver_name(struct dma_fence* fence)
{
	(void)fence;
	return "powervr";
}


static const struct dma_fence_ops sStubFenceOps = {
	.get_driver_name = lx_fence_driver_name,
	.get_timeline_name = lx_fence_driver_name,
};


void
lx_dma_fence_init_globals(void)
{
	static bool sInitialized = false;
	if (sInitialized)
		return;
	sInitialized = true;

	init_waitqueue_head(&lx_fence_queue);
	dma_fence_init(&sStubFence, &sStubFenceOps, NULL, 0, 0);
	dma_fence_signal(&sStubFence);
}


u64
dma_fence_context_alloc(unsigned int count)
{
	static int64 sNextContext = 1;
	return (u64)atomic_add64(&sNextContext, count);
}


void
dma_fence_init(struct dma_fence* fence, const struct dma_fence_ops* ops,
	spinlock_t* lock, u64 context, u64 seqno)
{
	fence->ops = ops;
	fence->lock = lock;
	fence->context = context;
	fence->seqno = seqno;
	fence->flags = 0;
	fence->error = 0;
	fence->timestamp = 0;
	INIT_LIST_HEAD(&fence->cb_list);
	kref_init(&fence->refcount);
}


void
dma_fence_free(struct dma_fence* fence)
{
	free(fence);
}


static void
dma_fence_release(struct kref* kref)
{
	struct dma_fence* fence = container_of(kref, struct dma_fence, refcount);
	if (WARN_ON(fence == &sStubFence))
		return;
	if (fence->ops != NULL && fence->ops->release != NULL)
		fence->ops->release(fence);
	else
		dma_fence_free(fence);
}


void
dma_fence_put(struct dma_fence* fence)
{
	if (fence != NULL)
		kref_put(&fence->refcount, dma_fence_release);
}


static cpu_status
fence_lock(void)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sFenceLock);
	return state;
}


static void
fence_unlock(cpu_status state)
{
	release_spinlock(&sFenceLock);
	restore_interrupts(state);
}


bool
dma_fence_is_signaled(struct dma_fence* fence)
{
	return (__atomic_load_n(&fence->flags, __ATOMIC_ACQUIRE)
		& (1UL << DMA_FENCE_FLAG_SIGNALED_BIT)) != 0;
}


/*!	Marks the fence signaled, then runs its callbacks and wakes every fence
	waiter; -EINVAL if it was signaled before.
*/
int
dma_fence_signal(struct dma_fence* fence)
{
	if (fence == NULL)
		return -EINVAL;

	struct list_head callbacks;
	INIT_LIST_HEAD(&callbacks);

	cpu_status state = fence_lock();
	if (dma_fence_is_signaled(fence)) {
		fence_unlock(state);
		return -EINVAL;
	}
	fence->timestamp = system_time();
	__atomic_fetch_or(&fence->flags, 1UL << DMA_FENCE_FLAG_SIGNALED_BIT,
		__ATOMIC_RELEASE);
	list_splice_init(&fence->cb_list, &callbacks);
	fence_unlock(state);

	struct dma_fence_cb* cb;
	struct dma_fence_cb* next;
	list_for_each_entry_safe(cb, next, &callbacks, node) {
		INIT_LIST_HEAD(&cb->node);
		cb->func(fence, cb);
	}

	wake_up_all(&lx_fence_queue);
	return 0;
}


int
dma_fence_add_callback(struct dma_fence* fence, struct dma_fence_cb* cb,
	dma_fence_func_t func)
{
	cpu_status state = fence_lock();
	if (dma_fence_is_signaled(fence)) {
		fence_unlock(state);
		INIT_LIST_HEAD(&cb->node);
		return -ENOENT;
	}
	cb->func = func;
	list_add_tail(&cb->node, &fence->cb_list);
	fence_unlock(state);
	return 0;
}


bool
dma_fence_remove_callback(struct dma_fence* fence, struct dma_fence_cb* cb)
{
	cpu_status state = fence_lock();
	bool removed = !dma_fence_is_signaled(fence) && !list_empty(&cb->node);
	if (removed)
		list_del_init(&cb->node);
	fence_unlock(state);
	return removed;
}


int
lx_dma_fence_wait(struct dma_fence* fence, bool interruptible,
	bigtime_t deadline)
{
	for (;;) {
		int32 generation = lx_wait_queue_generation(&lx_fence_queue);
		if (dma_fence_is_signaled(fence))
			return 0;
		if (system_time() >= deadline)
			return -ETIME;
		if (!interruptible) {
			lx_wait_queue_sleep(&lx_fence_queue, generation, deadline);
			continue;
		}
		if (lx_wait_queue_sleep_interruptible(&lx_fence_queue, generation,
				deadline) == B_INTERRUPTED) {
			return -EINTR;
		}
	}
}


/*!	Linux's dma_fence_wait_timeout(): the jiffies left (at least 1) once
	signaled, 0 on timeout, -EINTR when interrupted.
*/
long
dma_fence_wait_timeout(struct dma_fence* fence, bool interruptible,
	long timeout)
{
	bigtime_t deadline = timeout == MAX_SCHEDULE_TIMEOUT ? B_INFINITE_TIMEOUT
		: system_time() + (bigtime_t)jiffies_to_usecs(timeout);
	int error = lx_dma_fence_wait(fence, interruptible, deadline);
	if (error == -ETIME)
		return 0;
	if (error != 0)
		return error;
	if (timeout == MAX_SCHEDULE_TIMEOUT)
		return timeout;
	long left = (long)((deadline - system_time()) / (1000000 / HZ));
	return left < 1 ? 1 : left;
}


struct dma_fence*
dma_fence_get_stub(void)
{
	return dma_fence_get(&sStubFence);
}


/* #pragma mark - lx_dma_fence_all */


struct lx_fence_all;

struct lx_fence_all_part {
	struct dma_fence_cb		cb;
	struct dma_fence*		fence;
	struct lx_fence_all*	all;
};

struct lx_fence_all {
	struct dma_fence			base;
	atomic_t					pending;
	u32							count;
	struct lx_fence_all_part	parts[];
};


static void
lx_fence_all_release(struct dma_fence* fence)
{
	struct lx_fence_all* all = container_of(fence, struct lx_fence_all, base);
	for (u32 i = 0; i < all->count; i++)
		dma_fence_put(all->parts[i].fence);
	free(all);
}


static const struct dma_fence_ops sFenceAllOps = {
	.get_driver_name = lx_fence_driver_name,
	.get_timeline_name = lx_fence_driver_name,
	.release = lx_fence_all_release,
};


static void
lx_fence_all_part_done(struct lx_fence_all* all, struct dma_fence* fence)
{
	if (fence->error != 0 && all->base.error == 0)
		all->base.error = fence->error;
	if (atomic_dec_return(&all->pending) == 0) {
		dma_fence_signal(&all->base);
		dma_fence_put(&all->base);
			// the reference the pending parts held
	}
}


static void
lx_fence_all_callback(struct dma_fence* fence, struct dma_fence_cb* cb)
{
	struct lx_fence_all_part* part
		= container_of(cb, struct lx_fence_all_part, cb);
	lx_fence_all_part_done(part->all, fence);
}


struct dma_fence*
lx_dma_fence_all(struct dma_fence** fences, u32 count)
{
	struct lx_fence_all* all = (struct lx_fence_all*)calloc(1,
		sizeof(*all) + count * sizeof(all->parts[0]));
	if (all == NULL) {
		for (u32 i = 0; i < count; i++)
			dma_fence_put(fences[i]);
		return NULL;
	}

	dma_fence_init(&all->base, &sFenceAllOps, NULL,
		dma_fence_context_alloc(1), 1);
	all->count = count;
	atomic_set(&all->pending, (int)count + 1);
	kref_get(&all->base.refcount);
		// for the pending parts, dropped when the last one is done
	for (u32 i = 0; i < count; i++) {
		all->parts[i].fence = fences[i];
		all->parts[i].all = all;
	}
	for (u32 i = 0; i < count; i++) {
		if (dma_fence_add_callback(fences[i], &all->parts[i].cb,
				lx_fence_all_callback) != 0) {
			lx_fence_all_part_done(all, fences[i]);
		}
	}
	// the "+ 1": no signal before every callback is in place
	lx_fence_all_part_done(all, &sStubFence);
	return &all->base;
}


/* #pragma mark - dma_fence_unwrap */


struct dma_fence*
lx_dma_fence_unwrap_first(struct dma_fence* head,
	struct dma_fence_unwrap* cursor)
{
	cursor->head = head;
	cursor->index = 0;
	if (head == NULL || head->ops != &sFenceAllOps)
		return head;
	struct lx_fence_all* all = container_of(head, struct lx_fence_all, base);
	return all->count > 0 ? all->parts[0].fence : NULL;
}


struct dma_fence*
lx_dma_fence_unwrap_next(struct dma_fence_unwrap* cursor)
{
	struct dma_fence* head = cursor->head;
	if (head == NULL || head->ops != &sFenceAllOps)
		return NULL;
	struct lx_fence_all* all = container_of(head, struct lx_fence_all, base);
	if (++cursor->index >= all->count)
		return NULL;
	return all->parts[cursor->index].fence;
}


/* #pragma mark - seq_file */


#define LX_SEQ_BUFFER_SIZE	1024


int
seq_open(struct file* file, const struct seq_operations* operations)
{
	struct seq_file* seq = (struct seq_file*)calloc(1, sizeof(*seq));
	if (seq == NULL)
		return -ENOMEM;
	seq->buffer = (char*)malloc(LX_SEQ_BUFFER_SIZE);
	if (seq->buffer == NULL) {
		free(seq);
		return -ENOMEM;
	}
	seq->size = LX_SEQ_BUFFER_SIZE;
	seq->op = operations;
	file->private_data = seq;
	return 0;
}


int
seq_release(struct inode* inode, struct file* file)
{
	(void)inode;
	struct seq_file* seq = (struct seq_file*)file->private_data;
	if (seq != NULL) {
		free(seq->buffer);
		free(seq);
	}
	file->private_data = NULL;
	return 0;
}


ssize_t
seq_read(struct file* file, char __user* buffer, size_t size,
	loff_t* position)
{
	// lx_debugfs_dump() drives the seq_file operations directly
	(void)file;
	(void)buffer;
	(void)size;
	(void)position;
	return -EINVAL;
}


loff_t
seq_lseek(struct file* file, loff_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return -EINVAL;
}


void
seq_printf(struct seq_file* file, const char* format, ...)
{
	if (file->count >= file->size)
		return;
	va_list args;
	va_start(args, format);
	int length = vsnprintf(file->buffer + file->count,
		file->size - file->count, format, args);
	va_end(args);
	if (length > 0)
		file->count = min(file->count + (size_t)length, file->size - 1);
}


void
seq_puts(struct seq_file* file, const char* text)
{
	seq_printf(file, "%s", text);
}


/* #pragma mark - debugfs */


#define LX_DEBUGFS_FILES	8

static struct {
	char							name[32];
	void*							data;
	const struct file_operations*	fops;
} sDebugFiles[LX_DEBUGFS_FILES];


struct dentry*
debugfs_create_file(const char* name, umode_t mode, struct dentry* parent,
	void* data, const struct file_operations* fops)
{
	(void)mode;
	(void)parent;
	for (int i = 0; i < LX_DEBUGFS_FILES; i++) {
		if (sDebugFiles[i].fops == NULL
			|| strcmp(sDebugFiles[i].name, name) == 0) {
			strlcpy(sDebugFiles[i].name, name, sizeof(sDebugFiles[i].name));
			sDebugFiles[i].data = data;
			sDebugFiles[i].fops = fops;
			return NULL;
		}
	}
	lx_log(LX_LOG_WARNING, "no room for debugfs file %s", name);
	return NULL;
}


void
lx_debugfs_remove_all(void)
{
	memset(sDebugFiles, 0, sizeof(sDebugFiles));
}


static int
lx_debugfs_find(const char* name)
{
	for (int i = 0; i < LX_DEBUGFS_FILES; i++) {
		if (sDebugFiles[i].fops != NULL
			&& strcmp(sDebugFiles[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}


/*!	Reads a seq_file based debugfs file the way seq_read() would, and
	hands each line of its output to \a line.
*/
int
lx_debugfs_dump(const char* name, void (*line)(void* cookie, const char* text),
	void* cookie, unsigned int maxLines)
{
	int index = lx_debugfs_find(name);
	if (index < 0)
		return -ENOENT;
	const struct file_operations* fops = sDebugFiles[index].fops;
	if (fops->open == NULL)
		return -EINVAL;

	struct inode inode = { sDebugFiles[index].data };
	struct file file = { NULL };
	int error = fops->open(&inode, &file);
	if (error != 0)
		return error;

	struct seq_file* seq = (struct seq_file*)file.private_data;
	unsigned int lines = 0;
	loff_t position = 0;
	void* entry = seq->op->start(seq, &position);
	while (entry != NULL && lines < maxLines) {
		seq->count = 0;
		if (seq->op->show(seq, entry) == 0) {
			seq->buffer[min(seq->count, seq->size - 1)] = '\0';
			// one show() may hold several lines
			char* text = seq->buffer;
			while (*text != '\0' && lines < maxLines) {
				char* end = strchr(text, '\n');
				if (end != NULL)
					*end = '\0';
				line(cookie, text);
				lines++;
				if (end == NULL)
					break;
				text = end + 1;
			}
		}
		entry = seq->op->next(seq, entry, &position);
	}
	seq->op->stop(seq, entry);

	if (fops->release != NULL)
		fops->release(&inode, &file);
	return (int)lines;
}


int
lx_debugfs_attr_set(const char* name, u64 value)
{
	int index = lx_debugfs_find(name);
	if (index < 0)
		return -ENOENT;
	if (sDebugFiles[index].fops->attr_set == NULL)
		return -EINVAL;
	return sDebugFiles[index].fops->attr_set(sDebugFiles[index].data, value);
}


/* #pragma mark - module parameters */


int
kstrtouint(const char* text, unsigned int base, unsigned int* _value)
{
	if (text == NULL || *text == '\0')
		return -EINVAL;
	char* end;
	unsigned long value = strtoul(text, &end, base);
	if (*end == '\n')
		end++;
	if (end == text || *end != '\0')
		return -EINVAL;
	if (value > UINT32_MAX)
		return -ERANGE;
	*_value = (unsigned int)value;
	return 0;
}


int
param_get_hexint(char* buffer, const struct kernel_param* parameter)
{
	return sprintf(buffer, "%#08x\n", *(unsigned int*)parameter->arg);
}


/* #pragma mark - user memory */


unsigned long
clear_user(void __user* to, unsigned long size)
{
	static const u8 kZeroes[64];
	unsigned long done = 0;
	if (!lx_access_ok(to, size))
		return size;
	while (done < size) {
		unsigned long chunk = min_t(unsigned long, size - done,
			sizeof(kZeroes));
		if (user_memcpy((u8 __user*)to + done, kZeroes, chunk) != B_OK)
			return size - done;
		done += chunk;
	}
	return 0;
}


/*!	Linux's rule for extensible structures: a shorter user structure is
	zero-extended, a longer one is accepted only if its extra bytes are 0.
*/
int
copy_struct_from_user(void* to, size_t size, const void __user* from,
	size_t userSize)
{
	if (!lx_access_ok(from, userSize))
		return -EFAULT;
	if (userSize < size) {
		memset((u8*)to + userSize, 0, size - userSize);
	} else if (userSize > size) {
		u8 extra[64];
		for (size_t offset = size; offset < userSize;) {
			size_t chunk = min_t(size_t, userSize - offset, sizeof(extra));
			if (user_memcpy(extra, (const u8 __user*)from + offset, chunk)
					!= B_OK) {
				return -EFAULT;
			}
			if (!mem_is_zero(extra, chunk))
				return -E2BIG;
			offset += chunk;
		}
	}
	if (user_memcpy(to, from, min(size, userSize)) != B_OK)
		return -EFAULT;
	return 0;
}


void*
memdup_user(const void __user* from, size_t size)
{
	if (!lx_access_ok(from, size))
		return ERR_PTR(-EFAULT);
	void* copy = kmalloc(size, GFP_KERNEL);
	if (copy == NULL)
		return ERR_PTR(-ENOMEM);
	if (user_memcpy(copy, from, size) != B_OK) {
		kfree(copy);
		return ERR_PTR(-EFAULT);
	}
	return copy;
}


struct drm_gem_object*
drm_gem_shmem_prime_import_sg_table(struct drm_device* dev,
	struct dma_buf_attachment* attachment, struct sg_table* table)
{
	(void)dev;
	(void)attachment;
	(void)table;
	return ERR_PTR(-ENODEV);
}


/* #pragma mark - xarray */


void
xa_init_flags(struct xarray* xa, unsigned int flags)
{
	memset(xa, 0, sizeof(*xa));
	recursive_lock_init(&xa->lock, "powervr xarray");
	xa->base = (flags & XA_FLAGS_ALLOC1) != 0 ? 1 : 0;
	xa->initialized = true;
}


void
xa_destroy(struct xarray* xa)
{
	if (!xa->initialized)
		return;
	free(xa->entries);
	recursive_lock_destroy(&xa->lock);
	memset(xa, 0, sizeof(*xa));
}


void
xa_lock(struct xarray* xa)
{
	recursive_lock_lock(&xa->lock);
}


void
xa_unlock(struct xarray* xa)
{
	recursive_lock_unlock(&xa->lock);
}


static int
xa_reserve_index(struct xarray* xa, unsigned long index)
{
	if (index < xa->capacity)
		return 0;
	u32 capacity = max(xa->capacity * 2, 16u);
	while (capacity <= index)
		capacity *= 2;
	void** entries = (void**)realloc(xa->entries, capacity * sizeof(void*));
	if (entries == NULL)
		return -ENOMEM;
	memset(entries + xa->capacity, 0,
		(capacity - xa->capacity) * sizeof(void*));
	xa->entries = entries;
	xa->capacity = capacity;
	return 0;
}


int
xa_alloc(struct xarray* xa, u32* _id, void* entry, struct xa_limit limit,
	gfp_t flags)
{
	(void)flags;
	if (entry == NULL)
		return -EINVAL;

	xa_lock(xa);
	u32 index = max(limit.min, xa->base);
	while (index < xa->capacity && xa->entries[index] != NULL)
		index++;
	// IDs are kept dense: anything near 2^32 entries is out of reach anyway
	int error = index > limit.max || index >= (1u << 24)
		? -EBUSY : xa_reserve_index(xa, index);
	if (error == 0) {
		xa->entries[index] = entry;
		*_id = index;
	}
	xa_unlock(xa);
	return error;
}


void*
xa_store(struct xarray* xa, unsigned long index, void* entry, gfp_t flags)
{
	(void)flags;
	xa_lock(xa);
	void* old = NULL;
	if (index >= (1u << 24) || xa_reserve_index(xa, index) != 0) {
		xa_unlock(xa);
		return ERR_PTR(-ENOMEM);
	}
	old = xa->entries[index];
	xa->entries[index] = entry;
	xa_unlock(xa);
	return old;
}


void*
xa_load(struct xarray* xa, unsigned long index)
{
	xa_lock(xa);
	void* entry = index < xa->capacity ? xa->entries[index] : NULL;
	xa_unlock(xa);
	return entry;
}


void*
xa_erase(struct xarray* xa, unsigned long index)
{
	xa_lock(xa);
	void* entry = NULL;
	if (index < xa->capacity) {
		entry = xa->entries[index];
		xa->entries[index] = NULL;
	}
	xa_unlock(xa);
	return entry;
}


bool
xa_empty(struct xarray* xa)
{
	unsigned long index = 0;
	return lx_xa_find(xa, &index) == NULL;
}


/*!	The first entry at or after \a index; \a index is set to it. */
void*
lx_xa_find(struct xarray* xa, unsigned long* index)
{
	xa_lock(xa);
	void* entry = NULL;
	for (; *index < xa->capacity; (*index)++) {
		entry = xa->entries[*index];
		if (entry != NULL)
			break;
	}
	xa_unlock(xa);
	return entry;
}
