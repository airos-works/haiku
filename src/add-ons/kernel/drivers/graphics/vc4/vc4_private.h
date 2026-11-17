/*
 * Copyright © 2014 Broadcom
 * Copyright 2026, air/OS. All rights reserved.
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
#ifndef VC4_PRIVATE_H
#define VC4_PRIVATE_H


/*	What the job validation (vc4_validate.cpp, vc4_validate_shaders.cpp,
	vc4_render_cl.cpp, vc4_gem.cpp: ports of Linux' MIT licensed
	drivers/gpu/drm/vc4 sources) and the device (driver.cpp) share. The
	execution and shader structures are those of Linux' vc4_drv.h as Mesa's
	MIT licensed copy (src/gallium/drivers/vc4/kernel/vc4_drv.h) has them,
	with the GEM objects replaced by vc4_bo. */


#include <string.h>

#include <KernelExport.h>
#include <SupportDefs.h>

#include <kernel.h>

#include <graphics/vc4/vc4_drm.h>


struct VMCache;
struct vc4_info;
struct vc4_block;


extern bool gVc4Debug;

#define DRM_DEBUG(x...)	do { if (gVc4Debug) dprintf("vc4: " x); } while (0)
#define DRM_INFO(x...)	dprintf("vc4: " x)
#define DRM_ERROR(x...)	dprintf("vc4: " x)


/*	vc4_texture_sample_info - saves the offsets into the UBO for texture
	setup parameters.

	This will be used at draw time to relocate the reference to the texture
	contents in p0, and validate that the offset combined with
	width/height/stride/etc. from p1 and p2/p3 doesn't sample outside the BO.
	Note that the hardware treats unprovided config parameters as 0, so not
	all of them need to be set up for every texure sample, and we'll store ~0
	as the offset to mark the unused ones.

	See the VC4 3D architecture guide page 41 ("Texture and Memory Lookup Unit
	Setup") for definitions of the texture parameters. */
struct vc4_texture_sample_info {
	bool		is_direct;
	uint32_t	p_offset[4];
};

/*	vc4_validated_shader_info - information about validated shaders that
	needs to be used from command list validation.

	For a given shader, each time a shader state record references it, we
	need to verify that the shader doesn't read more uniforms than the shader
	state record's uniform BO pointer can provide, and we need to apply
	relocations and validate the shader state record's uniforms that define
	the texture samples. */
struct vc4_validated_shader_info {
	uint32_t	uniforms_size;
	uint32_t	uniforms_src_size;
	uint32_t	num_texture_samples;
	vc4_texture_sample_info* texture_samples;

	uint32_t	num_uniform_addr_offsets;
	uint32_t*	uniform_addr_offsets;

	bool		is_threaded;
};


/*	A buffer object: physically contiguous memory the core reaches at
	"paddr", its bus address, mapped write-combining in the kernel. */
struct vc4_bo {
	vc4_bo*		next;			// in the device's list
	int32		references;		// handles, jobs, the driver itself
	uint32		size;			// bytes, whole pages
	phys_addr_t	physical;
	uint32		paddr;			// the core's address of it (bus address)
	uint8*		vaddr;			// the kernel's mapping

	area_id		area;			// a contiguous area of its own, or -1
	vc4_block*	block;			// or its range of the pool

	// mapping into a team (VC4_HAIKU_MMAP_BO)
	area_id		mapSource;		// the kernel area the mapping clones
	VMCache*	mapCache;		// pooled: that area's cache, referenced
	area_id		userArea;		// the team's mapping, or -1
	team_id		userTeam;

	uint64_t	seqno;			// the last job using it
	uint64_t	write_seqno;	// the last job writing it through the RCL
	vc4_validated_shader_info* validated_shader;
	bool		t_format;
	uint32		madv;
	bool		user;			// created by a request (CREATE_*_BO)

	vc4_bo*		unref_next;		// in vc4_exec_info::unref_list
};


struct vc4_exec_info {
	vc4_info*	dev;
	vc4_exec_info* next;		// in the device's job queue

	/* Sequence number for this bin/render job. */
	uint64_t	seqno;

	/* Latest write_seqno of any BO that binning depends on. */
	uint64_t	bin_dep_seqno;

	/* Kernel-space copy of the ioctl arguments */
	drm_vc4_submit_cl* args;

	/* This is the array of BOs that were looked up at the start of exec.
	 * Command validation will use indices into this array.
	 */
	vc4_bo**	bo;
	uint32_t	bo_count;

	/* List of BOs that are being written by the RCL.  Other than
	 * the binner temporary storage, this is all the BOs written
	 * by the job.
	 */
	vc4_bo*		rcl_write_bo[4];
	uint32_t	rcl_write_bo_count;

	/* Other BOs used in the job that need to be released once the job is
	 * complete (the validated lists, the render list).
	 */
	vc4_bo*		unref_list;

	/* Current unvalidated indices into @bo loaded by the non-hardware
	 * VC4_PACKET_GEM_HANDLES.
	 */
	uint32_t	bo_index[2];

	/* This is the BO where we store the validated command lists, shader
	 * records, and uniforms.
	 */
	vc4_bo*		exec_bo;

	/**
	 * This tracks the per-shader-record state (packet 64) that
	 * determines the length of the shader record and the offset
	 * it's expected to be found at.  It gets read in from the
	 * command lists.
	 */
	struct vc4_shader_state {
		uint32_t addr;
		/* Maximum vertex index referenced by any primitive using this
		 * shader state.
		 */
		uint32_t max_index;
	} *shader_state;

	/** How many shader states the user declared they were using. */
	uint32_t	shader_state_size;
	/** How many shader state records the validator has seen. */
	uint32_t	shader_state_count;

	bool		found_tile_binning_mode_config_packet;
	bool		found_start_tile_binning_packet;
	bool		found_increment_semaphore_packet;
	bool		found_flush;
	uint8_t		bin_tiles_x, bin_tiles_y;
	/* Physical address of the start of the tile alloc array
	 * (where each tile's binned CL will start)
	 */
	uint32_t	tile_alloc_offset;
	/* Bitmask of which binner slots are freed when this job completes. */
	uint32_t	bin_slots;

	/**
	 * Computed addresses pointing into exec_bo where we start the
	 * bin thread (ct0) and render thread (ct1).
	 */
	uint32_t	ct0ca, ct0ea;
	uint32_t	ct1ca, ct1ea;

	/* Pointer to the unvalidated bin CL (if present). */
	uint8_t*	bin_u;

	/* Pointers to the shader recs.  These paddr gets incremented as CL
	 * packets are relocated in validate_gl_shader_state, and the vaddrs
	 * (u and v) get incremented and size decremented as the shader recs
	 * themselves are validated.
	 */
	uint8_t*	shader_rec_u;
	uint8_t*	shader_rec_v;
	uint32_t	shader_rec_p;
	uint32_t	shader_rec_size;

	/* Pointers to the uniform data.  These pointers are incremented, and
	 * size decremented, as each batch of uniforms is uploaded.
	 */
	uint8_t*	uniforms_u;
	uint8_t*	uniforms_v;
	uint32_t	uniforms_p;
	uint32_t	uniforms_size;
};


// Unaligned access to the packets: they are byte streams.
static inline uint32_t
get_u32(const void* address)
{
	uint32_t value;
	memcpy(&value, address, sizeof(value));
	return value;
}


static inline uint16_t
get_u16(const void* address)
{
	uint16_t value;
	memcpy(&value, address, sizeof(value));
	return value;
}


static inline void
put_u32(void* address, uint32_t value)
{
	memcpy(address, &value, sizeof(value));
}


static inline void
put_u16(void* address, uint16_t value)
{
	memcpy(address, &value, sizeof(value));
}


// driver.cpp
status_t vc4_bo_create_kernel(vc4_info* info, uint32 size, vc4_bo** _bo);
void vc4_bo_release(vc4_info* info, vc4_bo* bo);
int vc4_get_bin_slot(vc4_info* info);
uint32 vc4_bin_slot_address(vc4_info* info, int slot);
uint32 vc4_bin_alloc_size(vc4_info* info);

// vc4_gem.cpp
status_t vc4_get_bcl(vc4_info* info, vc4_exec_info* exec);
void vc4_exec_cleanup(vc4_info* info, vc4_exec_info* exec);

// vc4_validate.cpp
int vc4_validate_bin_cl(uint8_t* validated, uint8_t* unvalidated,
	vc4_exec_info* exec);
int vc4_validate_shader_recs(vc4_exec_info* exec);
vc4_bo* vc4_use_bo(vc4_exec_info* exec, uint32_t hindex);
bool vc4_check_tex_size(vc4_exec_info* exec, vc4_bo* fbo, uint32_t offset,
	uint8_t tiling_format, uint32_t width, uint32_t height, uint8_t cpp);

// vc4_validate_shaders.cpp
vc4_validated_shader_info* vc4_validate_shader(const uint64_t* shader,
	uint32_t size);
void vc4_free_validated_shader(vc4_validated_shader_info* info);

// vc4_render_cl.cpp
int vc4_get_rcl(vc4_info* info, vc4_exec_info* exec);


#endif	/* VC4_PRIVATE_H */
