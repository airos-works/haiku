/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*	air/OS: vc4_get_bcl() of Linux' drivers/gpu/drm/vc4/vc4_gem.c
	(rpi-6.6.y): copies the binner list, the shader records and the uniforms
	in from the caller and validates them into a buffer of the kernel's,
	where the core reads them. The rest of vc4_gem.c (the job queue, the
	waits, the hang check) is in driver.cpp, in the form the driver's job
	thread needs it.

	Linux then waits for the jobs that last wrote the buffers the binner
	reads (exec->bin_dep_seqno); here the jobs run one at a time in the
	order they were submitted, so every earlier job is done before this one
	starts. */


#include <limits.h>
#include <stdlib.h>

#include <kernel.h>

#include "vc4_private.h"


// Linux has no limit but what kvmalloc() can do; one frame's lists, records
// and uniforms are far below this.
#define VC4_MAX_EXEC_SIZE	(32 * 1024 * 1024)


static status_t
copy_from_user(void* to, uint64 from, uint32 size)
{
	if (size == 0)
		return B_OK;
	if (!IS_USER_ADDRESS((addr_t)from)
		|| !IS_USER_ADDRESS((addr_t)from + size - 1)) {
		return B_BAD_ADDRESS;
	}
	return user_memcpy(to, (const void*)(addr_t)from, size) == B_OK
		? B_OK : B_BAD_ADDRESS;
}


status_t
vc4_get_bcl(vc4_info* info, vc4_exec_info* exec)
{
	struct drm_vc4_submit_cl *args = exec->args;
	uint8_t *temp = NULL;
	uint8_t *bin;
	status_t ret = B_OK;
	uint32_t bin_offset = 0;
	uint32_t shader_rec_offset = ROUNDUP(bin_offset + args->bin_cl_size,
					     (uint32_t)16);
	uint32_t uniforms_offset = shader_rec_offset + args->shader_rec_size;
	uint32_t exec_size = uniforms_offset + args->uniforms_size;
	// the shader states start 8 byte aligned in the temporary buffer
	uint32_t state_offset = ROUNDUP(exec_size, (uint32_t)8);
	uint32_t temp_size = state_offset
		+ (sizeof(vc4_exec_info::vc4_shader_state) * args->shader_rec_count);
	vc4_bo *bo;

	if (shader_rec_offset < args->bin_cl_size ||
	    uniforms_offset < shader_rec_offset ||
	    exec_size < uniforms_offset ||
	    state_offset < exec_size ||
	    args->shader_rec_count >= (UINT_MAX /
					  sizeof(vc4_exec_info::vc4_shader_state)) ||
	    temp_size < state_offset || temp_size > VC4_MAX_EXEC_SIZE) {
		DRM_DEBUG("overflow in exec arguments\n");
		return B_BAD_VALUE;
	}

	/* Allocate space where we'll store the copied in user command lists
	 * and shader records.
	 *
	 * We don't just copy directly into the BOs because we need to
	 * read the contents back for validation, and I think the
	 * bo->vaddr is uncached access.
	 */
	temp = (uint8_t*)malloc(temp_size);
	if (!temp) {
		DRM_ERROR("Failed to allocate storage for copying "
			  "in bin/render CLs.\n");
		return B_NO_MEMORY;
	}
	bin = temp + bin_offset;
	exec->shader_rec_u = temp + shader_rec_offset;
	exec->uniforms_u = temp + uniforms_offset;
	exec->shader_state = (vc4_exec_info::vc4_shader_state*)
		(temp + state_offset);
	exec->shader_state_size = args->shader_rec_count;

	ret = copy_from_user(bin, args->bin_cl, args->bin_cl_size);
	if (ret == B_OK) {
		ret = copy_from_user(exec->shader_rec_u, args->shader_rec,
			args->shader_rec_size);
	}
	if (ret == B_OK) {
		ret = copy_from_user(exec->uniforms_u, args->uniforms,
			args->uniforms_size);
	}
	if (ret != B_OK)
		goto fail;

	ret = vc4_bo_create_kernel(info, exec_size, &bo);
	if (ret != B_OK) {
		DRM_ERROR("Couldn't allocate BO for binning\n");
		goto fail;
	}
	exec->exec_bo = bo;
	bo->unref_next = exec->unref_list;
	exec->unref_list = bo;

	exec->ct0ca = exec->exec_bo->paddr + bin_offset;

	exec->bin_u = bin;

	exec->shader_rec_v = exec->exec_bo->vaddr + shader_rec_offset;
	exec->shader_rec_p = exec->exec_bo->paddr + shader_rec_offset;
	exec->shader_rec_size = args->shader_rec_size;

	exec->uniforms_v = exec->exec_bo->vaddr + uniforms_offset;
	exec->uniforms_p = exec->exec_bo->paddr + uniforms_offset;
	exec->uniforms_size = args->uniforms_size;

	ret = vc4_validate_bin_cl(exec->exec_bo->vaddr + bin_offset,
				  bin,
				  exec);
	if (ret)
		goto fail;

	ret = vc4_validate_shader_recs(exec);

fail:
	// no pointers into the temporary copy outlive it
	exec->bin_u = NULL;
	exec->shader_rec_u = NULL;
	exec->uniforms_u = NULL;
	exec->shader_state = NULL;
	free(temp);
	return ret;
}
