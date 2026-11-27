/*
 * Copyright 2002-2018, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef _FD_H
#define _FD_H


#include <vfs.h>
#include <team.h>
#include <thread.h>


#ifdef __cplusplus
extern "C" {
#endif

struct file_descriptor;
struct io_context;
struct selectsync;
struct select_info;
struct module_info;

struct fd_ops {
	status_t	(*fd_close)(struct file_descriptor *);
	void		(*fd_free)(struct file_descriptor *);

	status_t	(*fd_read)(struct file_descriptor *, off_t pos, void *buffer,
						size_t *length);
	status_t	(*fd_write)(struct file_descriptor *, off_t pos,
						const void *buffer, size_t *length);
	ssize_t		(*fd_readv)(struct file_descriptor *, off_t pos,
						const struct iovec *vecs, int count);
	ssize_t		(*fd_writev)(struct file_descriptor *, off_t pos,
						const struct iovec *vecs, int count);
	off_t		(*fd_seek)(struct file_descriptor *, off_t pos, int seekType);

	status_t	(*fd_ioctl)(struct file_descriptor *, ulong op, void *buffer,
						size_t length);
	status_t	(*fd_set_flags)(struct file_descriptor *, int flags);

	status_t	(*fd_select)(struct file_descriptor *, uint8 event,
						struct selectsync *sync);
	status_t	(*fd_deselect)(struct file_descriptor *, uint8 event,
						struct selectsync *sync);

	status_t	(*fd_read_dir)(struct io_context* ioContext,
						struct file_descriptor *, struct dirent *buffer,
						size_t bufferSize, uint32 *_count);
	status_t	(*fd_rewind_dir)(struct file_descriptor *);

	status_t	(*fd_read_stat)(struct file_descriptor *, struct stat *);
	status_t	(*fd_write_stat)(struct file_descriptor *, const struct stat *,
		int statMask);

	// Optional: select or deselect several events (SELECT_FLAG()s) in one
	// call, where fd_select and fd_deselect take one event at a time. poll()
	// and select() always add POLLERR and POLLHUP, so a descriptor costs at
	// least three fd_select and three fd_deselect calls otherwise (#227).
	// fd_select_events returns in *_selected the events that fd_select would
	// have returned B_OK for, and notifies those that are ready, like
	// fd_select. Either hook may return B_UNSUPPORTED to decline, for a
	// descriptor whose events it cannot take at once; the caller then calls
	// fd_select or fd_deselect per event. A hook must decline (or not) the
	// same way for the same descriptor every time.
	status_t	(*fd_select_events)(struct file_descriptor *, uint16 events,
						struct selectsync *sync, uint16 *_selected);
	status_t	(*fd_deselect_events)(struct file_descriptor *, uint16 events,
						struct selectsync *sync);
};

struct file_descriptor {
	int32	ref_count;
	int32	open_count;
	struct fd_ops *ops;
	union {
		struct vnode *vnode;
		struct fs_mount *mount;
	} u;
	void	*cookie;
	int32	open_mode;
	off_t	pos;
	// Optional owner of anonymous fd_ops. The kernel releases this reference
	// after the final callback returns, so a driver cannot unload on its stack.
	struct module_info *module;
};


// additional open mode - kernel special
#define O_DISCONNECTED 0x80000000

/* Prototypes */

extern struct file_descriptor *alloc_fd(void);
extern int new_fd_etc(struct io_context *, struct file_descriptor *,
	int firstIndex);
extern int new_fd(struct io_context *, struct file_descriptor *);
extern int new_fd_flags(struct io_context *, struct file_descriptor *, int flags);
// Copy out the number while holding the table lock, before publication. A
// failed copy publishes nothing and leaves descriptor ownership with caller.
extern int new_fd_user(struct io_context *, struct file_descriptor *, int flags,
	int *userFD);
// Call before publication. On success the descriptor owns the module reference.
extern status_t fd_hold_module(struct file_descriptor *, const char *name);
extern struct file_descriptor *get_fd(const struct io_context *, int);
extern struct file_descriptor *get_open_fd(const struct io_context *, int);
extern void close_fd(struct io_context *context,
	struct file_descriptor *descriptor);
extern status_t close_fd_index(struct io_context *context, int fd);
extern void put_fd(struct file_descriptor *descriptor);
extern void disconnect_fd(struct file_descriptor *descriptor);
extern void inc_fd_ref_count(struct file_descriptor *descriptor);
extern void inc_fd_open_count(struct file_descriptor *descriptor);
extern int dup_foreign_fd(team_id fromTeam, int fd, bool kernel);
extern status_t select_fd(int32 fd, struct select_info *info, bool kernel);
extern status_t deselect_fd(int32 fd, struct select_info *info, bool kernel);
extern bool fd_is_valid(int fd, bool kernel);
extern struct vnode *fd_vnode(struct file_descriptor *descriptor);
extern bool fd_is_file(struct file_descriptor* descriptor);

extern bool fd_close_on_exec(const struct io_context *context, int fd);
extern void fd_set_close_on_exec(struct io_context *context, int fd,
	bool closeFD);
extern bool fd_close_on_fork(const struct io_context *context, int fd);
extern void fd_set_close_on_fork(struct io_context *context, int fd,
	bool closeFD);

static struct io_context *get_current_io_context(bool kernel);

extern status_t user_fd_kernel_ioctl(int fd, ulong op, void *buffer,
	size_t length);

/* The prototypes of the (sys|user)_ functions are currently defined in vfs.h */


/* Inlines */

static inline struct io_context *
get_current_io_context(bool kernel)
{
	if (kernel)
		return (struct io_context *)team_get_kernel_team()->io_context;

	return (struct io_context *)thread_get_current_thread()->team->io_context;
}

#ifdef __cplusplus
}
#endif

#endif /* _FD_H */
