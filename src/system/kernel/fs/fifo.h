/*
 * Copyright 2008, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef _VFS_FIFO_H
#define _VFS_FIFO_H

#include <fs_interface.h>


status_t	create_fifo_vnode(fs_volume* superVolume, fs_vnode* vnode);
void		fifo_init();

status_t	fifo_select_events(fs_vnode* vnode, void* cookie, uint16 events,
				selectsync* sync, uint16* _selected);
status_t	fifo_deselect_events(fs_vnode* vnode, void* cookie,
				uint16 events, selectsync* sync);


#endif	// _VFS_FIFO_H
