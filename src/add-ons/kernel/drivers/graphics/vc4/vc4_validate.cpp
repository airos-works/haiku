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

/**
 * DOC: Command list validator for VC4.
 *
 * Since the VC4 has no IOMMU between it and system memory, a user
 * with access to execute command lists could escalate privilege by
 * overwriting system memory (drawing to it as a framebuffer) or
 * reading system memory it shouldn't (reading it as a vertex buffer
 * or index buffer)
 *
 * We validate binner command lists to ensure that all accesses are
 * within the bounds of the GEM objects referenced by the submitted
 * job.  It explicitly whitelists packets, and looks at the offsets in
 * any address fields to make sure they're contained within the BOs
 * they reference.
 *
 * Note that because CL validation is already reading the
 * user-submitted CL and writing the validated copy out to the memory
 * that the GPU will actually read, this is also where GEM relocation
 * processing (turning BO references into actual addresses for the GPU
 * to use) happens.
 *
 * air/OS: Linux' drivers/gpu/drm/vc4/vc4_validate.c (rpi-6.6.y) in C++ on
 * the driver's vc4_bo. Differences: a shader record's handles must be
 * below the BO count (Linux accepts the count itself), a shader BO's
 * validation state is checked before it is used, the uniforms the
 * shader records consume are counted against the submitted uniform data,
 * the tile state and lists of the binning config must fit its bin slot, a
 * cube map's face offsets must not wrap, and a packet handler's error is
 * passed on as it is.
 */

#include <limits.h>

#include <algorithm>

#include "vc4_packet.h"
#include "vc4_private.h"


#define VALIDATE_ARGS \
	vc4_exec_info* exec,			\
	uint8_t* validated,				\
	uint8_t* untrusted


/** Return the width in pixels of a 64-byte microtile. */
static uint32_t
utile_width(int cpp)
{
	switch (cpp) {
	case 1:
	case 2:
		return 8;
	case 4:
		return 4;
	case 8:
		return 2;
	default:
		DRM_ERROR("unknown cpp: %d\n", cpp);
		return 1;
	}
}

/** Return the height in pixels of a 64-byte microtile. */
static uint32_t
utile_height(int cpp)
{
	switch (cpp) {
	case 1:
		return 8;
	case 2:
	case 4:
	case 8:
		return 4;
	default:
		DRM_ERROR("unknown cpp: %d\n", cpp);
		return 1;
	}
}

/**
 * size_is_lt() - Returns whether a miplevel of the given size will
 * use the lineartile (LT) tiling layout rather than the normal T
 * tiling layout.
 * @width: Width in pixels of the miplevel
 * @height: Height in pixels of the miplevel
 * @cpp: Bytes per pixel of the pixel format
 */
static bool
size_is_lt(uint32_t width, uint32_t height, int cpp)
{
	return (width <= 4 * utile_width(cpp) ||
		height <= 4 * utile_height(cpp));
}

vc4_bo*
vc4_use_bo(vc4_exec_info* exec, uint32_t hindex)
{
	if (hindex >= exec->bo_count) {
		DRM_DEBUG("BO index %d greater than BO count %d\n",
			  hindex, exec->bo_count);
		return NULL;
	}
	vc4_bo* bo = exec->bo[hindex];

	if (bo->validated_shader) {
		DRM_DEBUG("Trying to use shader BO as something other than "
			  "a shader\n");
		return NULL;
	}

	return bo;
}

static vc4_bo*
vc4_use_handle(vc4_exec_info* exec, uint32_t gem_handles_packet_index)
{
	return vc4_use_bo(exec, exec->bo_index[gem_handles_packet_index]);
}

static bool
validate_bin_pos(vc4_exec_info* exec, uint8_t* untrusted, uint32_t pos)
{
	/* Note that the untrusted pointer passed to these functions is
	 * incremented past the packet byte.
	 */
	return (untrusted - 1 == exec->bin_u + pos);
}

static uint32_t
gl_shader_rec_size(uint32_t pointer_bits)
{
	uint32_t attribute_count = pointer_bits & 7;
	bool extended = pointer_bits & 8;

	if (attribute_count == 0)
		attribute_count = 8;

	if (extended)
		return 100 + attribute_count * 4;
	else
		return 36 + attribute_count * 8;
}

bool
vc4_check_tex_size(vc4_exec_info* exec, vc4_bo* fbo,
		   uint32_t offset, uint8_t tiling_format,
		   uint32_t width, uint32_t height, uint8_t cpp)
{
	uint32_t aligned_width, aligned_height, stride, size;
	uint32_t utile_w = utile_width(cpp);
	uint32_t utile_h = utile_height(cpp);

	/* The shaded vertex format stores signed 12.4 fixed point
	 * (-2048,2047) offsets from the viewport center, so we should
	 * never have a render target larger than 4096.  The texture
	 * unit can only sample from 2048x2048, so it's even more
	 * restricted.  This lets us avoid worrying about overflow in
	 * our math.
	 */
	if (width > 4096 || height > 4096) {
		DRM_DEBUG("Surface dimensions (%d,%d) too large",
			  width, height);
		return false;
	}

	switch (tiling_format) {
	case VC4_TILING_FORMAT_LINEAR:
		aligned_width = ROUNDUP(width, utile_w);
		aligned_height = height;
		break;
	case VC4_TILING_FORMAT_T:
		aligned_width = ROUNDUP(width, utile_w * 8);
		aligned_height = ROUNDUP(height, utile_h * 8);
		break;
	case VC4_TILING_FORMAT_LT:
		aligned_width = ROUNDUP(width, utile_w);
		aligned_height = ROUNDUP(height, utile_h);
		break;
	default:
		DRM_DEBUG("buffer tiling %d unsupported\n", tiling_format);
		return false;
	}

	stride = aligned_width * cpp;
	size = stride * aligned_height;

	if (size + offset < size ||
	    size + offset > fbo->size) {
		DRM_DEBUG("Overflow in %dx%d (%dx%d) fbo size (%d + %d > %d)\n",
			  width, height,
			  aligned_width, aligned_height,
			  size, offset, fbo->size);
		return false;
	}

	return true;
}

static int
validate_flush(VALIDATE_ARGS)
{
	if (!validate_bin_pos(exec, untrusted, exec->args->bin_cl_size - 1)) {
		DRM_DEBUG("Bin CL must end with VC4_PACKET_FLUSH\n");
		return B_BAD_VALUE;
	}
	exec->found_flush = true;

	return 0;
}

static int
validate_start_tile_binning(VALIDATE_ARGS)
{
	if (exec->found_start_tile_binning_packet) {
		DRM_DEBUG("Duplicate VC4_PACKET_START_TILE_BINNING\n");
		return B_BAD_VALUE;
	}
	exec->found_start_tile_binning_packet = true;

	if (!exec->found_tile_binning_mode_config_packet) {
		DRM_DEBUG("missing VC4_PACKET_TILE_BINNING_MODE_CONFIG\n");
		return B_BAD_VALUE;
	}

	return 0;
}

static int
validate_increment_semaphore(VALIDATE_ARGS)
{
	if (!validate_bin_pos(exec, untrusted, exec->args->bin_cl_size - 2)) {
		DRM_DEBUG("Bin CL must end with "
			  "VC4_PACKET_INCREMENT_SEMAPHORE\n");
		return B_BAD_VALUE;
	}
	exec->found_increment_semaphore_packet = true;

	return 0;
}

static int
validate_indexed_prim_list(VALIDATE_ARGS)
{
	vc4_bo* ib;
	uint32_t length = get_u32(untrusted + 1);
	uint32_t offset = get_u32(untrusted + 5);
	uint32_t max_index = get_u32(untrusted + 9);
	uint32_t index_size = (*(uint8_t *)(untrusted + 0) >> 4) ? 2 : 1;
	vc4_exec_info::vc4_shader_state* shader_state;

	/* Check overflow condition */
	if (exec->shader_state_count == 0) {
		DRM_DEBUG("shader state must precede primitives\n");
		return B_BAD_VALUE;
	}
	shader_state = &exec->shader_state[exec->shader_state_count - 1];

	if (max_index > shader_state->max_index)
		shader_state->max_index = max_index;

	ib = vc4_use_handle(exec, 0);
	if (!ib)
		return B_BAD_VALUE;

	exec->bin_dep_seqno = std::max(exec->bin_dep_seqno, ib->write_seqno);

	if (offset > ib->size ||
	    (ib->size - offset) / index_size < length) {
		DRM_DEBUG("IB access overflow (%d + %d*%d > %d)\n",
			  offset, length, index_size, ib->size);
		return B_BAD_VALUE;
	}

	put_u32(validated + 5, ib->paddr + offset);

	return 0;
}

static int
validate_gl_array_primitive(VALIDATE_ARGS)
{
	uint32_t length = get_u32(untrusted + 1);
	uint32_t base_index = get_u32(untrusted + 5);
	uint32_t max_index;
	vc4_exec_info::vc4_shader_state* shader_state;

	/* Check overflow condition */
	if (exec->shader_state_count == 0) {
		DRM_DEBUG("shader state must precede primitives\n");
		return B_BAD_VALUE;
	}
	shader_state = &exec->shader_state[exec->shader_state_count - 1];

	if (length + base_index < length) {
		DRM_DEBUG("primitive vertex count overflow\n");
		return B_BAD_VALUE;
	}
	max_index = length + base_index - 1;

	if (max_index > shader_state->max_index)
		shader_state->max_index = max_index;

	return 0;
}

static int
validate_gl_shader_state(VALIDATE_ARGS)
{
	uint32_t i = exec->shader_state_count++;

	if (i >= exec->shader_state_size) {
		DRM_DEBUG("More requests for shader states than declared\n");
		return B_BAD_VALUE;
	}

	exec->shader_state[i].addr = get_u32(untrusted);
	exec->shader_state[i].max_index = 0;

	if (exec->shader_state[i].addr & ~0xf) {
		DRM_DEBUG("high bits set in GL shader rec reference\n");
		return B_BAD_VALUE;
	}

	put_u32(validated, exec->shader_rec_p + exec->shader_state[i].addr);

	exec->shader_rec_p +=
		ROUNDUP(gl_shader_rec_size(exec->shader_state[i].addr), 16);

	return 0;
}

static int
validate_tile_binning_config(VALIDATE_ARGS)
{
	uint8_t flags;
	uint32_t tile_state_size;
	uint32_t tile_count, bin_addr;
	int bin_slot;

	if (exec->found_tile_binning_mode_config_packet) {
		DRM_DEBUG("Duplicate VC4_PACKET_TILE_BINNING_MODE_CONFIG\n");
		return B_BAD_VALUE;
	}
	exec->found_tile_binning_mode_config_packet = true;

	exec->bin_tiles_x = *(uint8_t *)(untrusted + 12);
	exec->bin_tiles_y = *(uint8_t *)(untrusted + 13);
	tile_count = exec->bin_tiles_x * exec->bin_tiles_y;
	flags = *(uint8_t *)(untrusted + 14);

	if (exec->bin_tiles_x == 0 ||
	    exec->bin_tiles_y == 0) {
		DRM_DEBUG("Tile binning config of %dx%d too small\n",
			  exec->bin_tiles_x, exec->bin_tiles_y);
		return B_BAD_VALUE;
	}

	if (flags & (VC4_BIN_CONFIG_DB_NON_MS |
		     VC4_BIN_CONFIG_TILE_BUFFER_64BIT)) {
		DRM_DEBUG("unsupported binning config flags 0x%02x\n", flags);
		return B_BAD_VALUE;
	}

	/* air/OS: the tile state (48 bytes a tile) and the first 32 byte block
	 * of each tile's list must fit the slot (Linux does not check: 255x255
	 * tiles would have the binner write megabytes past it). Mesa's largest
	 * frame, 2048x2048 in 32x32 tiles, takes 320 KB of the 512. */
	if (ROUNDUP(48 * tile_count, 4096u) + 32 * tile_count
	    > vc4_bin_alloc_size(exec->dev)) {
		DRM_DEBUG("Tile binning config of %dx%d too large\n",
			  exec->bin_tiles_x, exec->bin_tiles_y);
		return B_BAD_VALUE;
	}

	bin_slot = vc4_get_bin_slot(exec->dev);
	if (bin_slot < 0) {
		if (bin_slot != B_INTERRUPTED) {
			DRM_ERROR("Failed to allocate binner memory: %d\n",
				  bin_slot);
		}
		return bin_slot;
	}

	/* The slot we allocated will only be used by this job, and is
	 * free when the job completes rendering.
	 */
	exec->bin_slots |= BIT(bin_slot);
	bin_addr = vc4_bin_slot_address(exec->dev, bin_slot);

	/* The tile state data array is 48 bytes per tile, and we put it at
	 * the start of a BO containing both it and the tile alloc.
	 */
	tile_state_size = 48 * tile_count;

	/* Since the tile alloc array will follow us, align. */
	exec->tile_alloc_offset = bin_addr + ROUNDUP(tile_state_size, 4096);

	*(uint8_t *)(validated + 14) =
		((flags & ~(VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_MASK |
			    VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_MASK)) |
		 VC4_BIN_CONFIG_AUTO_INIT_TSDA |
		 VC4_SET_FIELD(VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_32,
			       VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE) |
		 VC4_SET_FIELD(VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_128,
			       VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE));

	/* tile alloc address. */
	put_u32(validated + 0, exec->tile_alloc_offset);
	/* tile alloc size. */
	put_u32(validated + 4, bin_addr + vc4_bin_alloc_size(exec->dev)
		- exec->tile_alloc_offset);
	/* tile state address. */
	put_u32(validated + 8, bin_addr);

	return 0;
}

static int
validate_gem_handles(VALIDATE_ARGS)
{
	memcpy(exec->bo_index, untrusted, sizeof(exec->bo_index));
	return 0;
}

struct cmd_info {
	uint16_t len;
	const char *name;
	int (*func)(vc4_exec_info* exec, uint8_t* validated,
		    uint8_t* untrusted);
};

#define VC4_DEFINE_PACKET(packet, func) \
	case packet: \
	{ \
		static const cmd_info info = { packet ## _SIZE, #packet, func }; \
		return &info; \
	}

/* The packets the binner list may contain (Linux: the cmd_info table). */
static const cmd_info*
lookup_cmd_info(uint8_t cmd)
{
	switch (cmd) {
	VC4_DEFINE_PACKET(VC4_PACKET_HALT, NULL)
	VC4_DEFINE_PACKET(VC4_PACKET_NOP, NULL)
	VC4_DEFINE_PACKET(VC4_PACKET_FLUSH, validate_flush)
	VC4_DEFINE_PACKET(VC4_PACKET_FLUSH_ALL, NULL)
	VC4_DEFINE_PACKET(VC4_PACKET_START_TILE_BINNING,
			  validate_start_tile_binning)
	VC4_DEFINE_PACKET(VC4_PACKET_INCREMENT_SEMAPHORE,
			  validate_increment_semaphore)

	VC4_DEFINE_PACKET(VC4_PACKET_GL_INDEXED_PRIMITIVE,
			  validate_indexed_prim_list)
	VC4_DEFINE_PACKET(VC4_PACKET_GL_ARRAY_PRIMITIVE,
			  validate_gl_array_primitive)

	VC4_DEFINE_PACKET(VC4_PACKET_PRIMITIVE_LIST_FORMAT, NULL)

	VC4_DEFINE_PACKET(VC4_PACKET_GL_SHADER_STATE, validate_gl_shader_state)

	VC4_DEFINE_PACKET(VC4_PACKET_CONFIGURATION_BITS, NULL)
	VC4_DEFINE_PACKET(VC4_PACKET_FLAT_SHADE_FLAGS, NULL)
	VC4_DEFINE_PACKET(VC4_PACKET_POINT_SIZE, NULL)
	VC4_DEFINE_PACKET(VC4_PACKET_LINE_WIDTH, NULL)
	VC4_DEFINE_PACKET(VC4_PACKET_RHT_X_BOUNDARY, NULL)
	VC4_DEFINE_PACKET(VC4_PACKET_DEPTH_OFFSET, NULL)
	VC4_DEFINE_PACKET(VC4_PACKET_CLIP_WINDOW, NULL)
	VC4_DEFINE_PACKET(VC4_PACKET_VIEWPORT_OFFSET, NULL)
	VC4_DEFINE_PACKET(VC4_PACKET_CLIPPER_XY_SCALING, NULL)
	/* Note: The docs say this was also 105, but it was 106 in the
	 * initial userland code drop.
	 */
	VC4_DEFINE_PACKET(VC4_PACKET_CLIPPER_Z_SCALING, NULL)

	VC4_DEFINE_PACKET(VC4_PACKET_TILE_BINNING_MODE_CONFIG,
			  validate_tile_binning_config)

	VC4_DEFINE_PACKET(VC4_PACKET_GEM_HANDLES, validate_gem_handles)
	}

	return NULL;
}

int
vc4_validate_bin_cl(uint8_t* validated, uint8_t* unvalidated,
		    vc4_exec_info* exec)
{
	uint32_t len = exec->args->bin_cl_size;
	uint32_t dst_offset = 0;
	uint32_t src_offset = 0;

	while (src_offset < len) {
		uint8_t* dst_pkt = validated + dst_offset;
		uint8_t* src_pkt = unvalidated + src_offset;
		uint8_t cmd = *(uint8_t *)src_pkt;
		const cmd_info* info;

		info = lookup_cmd_info(cmd);
		if (!info) {
			DRM_DEBUG("0x%08x: packet %d invalid\n",
				  src_offset, cmd);
			return B_BAD_VALUE;
		}

		if (src_offset + info->len > len) {
			DRM_DEBUG("0x%08x: packet %d (%s) length 0x%08x "
				  "exceeds bounds (0x%08x)\n",
				  src_offset, cmd, info->name, info->len,
				  src_offset + len);
			return B_BAD_VALUE;
		}

		if (cmd != VC4_PACKET_GEM_HANDLES)
			memcpy(dst_pkt, src_pkt, info->len);

		int ret = info->func ? info->func(exec, dst_pkt + 1,
						  src_pkt + 1) : 0;
		if (ret) {
			DRM_DEBUG("0x%08x: packet %d (%s) failed to validate\n",
				  src_offset, cmd, info->name);
			/* air/OS: the handler's error, an interrupted wait for
			 * a bin slot included (Linux: -EINVAL) */
			return ret;
		}

		src_offset += info->len;
		/* GEM handle loading doesn't produce HW packets. */
		if (cmd != VC4_PACKET_GEM_HANDLES)
			dst_offset += info->len;

		/* When the CL hits halt, it'll stop reading anything else. */
		if (cmd == VC4_PACKET_HALT)
			break;
	}

	exec->ct0ea = exec->ct0ca + dst_offset;

	if (!exec->found_start_tile_binning_packet) {
		DRM_DEBUG("Bin CL missing VC4_PACKET_START_TILE_BINNING\n");
		return B_BAD_VALUE;
	}

	/* The bin CL must be ended with INCREMENT_SEMAPHORE and FLUSH.  The
	 * semaphore is used to trigger the render CL to start up, and the
	 * FLUSH is what caps the bin lists with
	 * VC4_PACKET_RETURN_FROM_SUB_LIST (so they jump back to the main
	 * render CL when they get called to) and actually triggers the queued
	 * semaphore increment.
	 */
	if (!exec->found_increment_semaphore_packet || !exec->found_flush) {
		DRM_DEBUG("Bin CL missing VC4_PACKET_INCREMENT_SEMAPHORE + "
			  "VC4_PACKET_FLUSH\n");
		return B_BAD_VALUE;
	}

	return 0;
}

static bool
reloc_tex(vc4_exec_info* exec,
	  uint8_t* uniform_data_u,
	  vc4_texture_sample_info* sample,
	  uint32_t texture_handle_index, bool is_cs)
{
	vc4_bo* tex;
	uint32_t p0 = get_u32(uniform_data_u + sample->p_offset[0]);
	uint32_t p1 = get_u32(uniform_data_u + sample->p_offset[1]);
	uint32_t p2 = (sample->p_offset[2] != ~0u ?
		       get_u32(uniform_data_u + sample->p_offset[2]) : 0);
	uint32_t p3 = (sample->p_offset[3] != ~0u ?
		       get_u32(uniform_data_u + sample->p_offset[3]) : 0);
	uint8_t* validated_p0 = exec->uniforms_v + sample->p_offset[0];
	uint32_t offset = p0 & VC4_TEX_P0_OFFSET_MASK;
	uint32_t miplevels = VC4_GET_FIELD(p0, VC4_TEX_P0_MIPLVLS);
	uint32_t width = VC4_GET_FIELD(p1, VC4_TEX_P1_WIDTH);
	uint32_t height = VC4_GET_FIELD(p1, VC4_TEX_P1_HEIGHT);
	uint32_t cpp, tiling_format, utile_w, utile_h;
	uint32_t i;
	uint32_t cube_map_stride = 0;
	uint32_t type;

	tex = vc4_use_bo(exec, texture_handle_index);
	if (!tex)
		return false;

	if (sample->is_direct) {
		uint32_t remaining_size = tex->size - p0;

		if (p0 > tex->size - 4) {
			DRM_DEBUG("UBO offset greater than UBO size\n");
			goto fail;
		}
		if (p1 > remaining_size - 4) {
			DRM_DEBUG("UBO clamp would allow reads "
				  "outside of UBO\n");
			goto fail;
		}
		put_u32(validated_p0, tex->paddr + p0);
		return true;
	}

	if (width == 0)
		width = 2048;
	if (height == 0)
		height = 2048;

	if (p0 & VC4_TEX_P0_CMMODE_MASK) {
		if (VC4_GET_FIELD(p2, VC4_TEX_P2_PTYPE) ==
		    VC4_TEX_P2_PTYPE_CUBE_MAP_STRIDE)
			cube_map_stride = p2 & VC4_TEX_P2_CMST_MASK;
		if (VC4_GET_FIELD(p3, VC4_TEX_P2_PTYPE) ==
		    VC4_TEX_P2_PTYPE_CUBE_MAP_STRIDE) {
			if (cube_map_stride) {
				DRM_DEBUG("Cube map stride set twice\n");
				goto fail;
			}

			cube_map_stride = p3 & VC4_TEX_P2_CMST_MASK;
		}
		if (!cube_map_stride) {
			DRM_DEBUG("Cube map stride not set\n");
			goto fail;
		}
	}

	type = (VC4_GET_FIELD(p0, VC4_TEX_P0_TYPE) |
		(VC4_GET_FIELD(p1, VC4_TEX_P1_TYPE4) << 4));

	switch (type) {
	case VC4_TEXTURE_TYPE_RGBA8888:
	case VC4_TEXTURE_TYPE_RGBX8888:
	case VC4_TEXTURE_TYPE_RGBA32R:
		cpp = 4;
		break;
	case VC4_TEXTURE_TYPE_RGBA4444:
	case VC4_TEXTURE_TYPE_RGBA5551:
	case VC4_TEXTURE_TYPE_RGB565:
	case VC4_TEXTURE_TYPE_LUMALPHA:
	case VC4_TEXTURE_TYPE_S16F:
	case VC4_TEXTURE_TYPE_S16:
		cpp = 2;
		break;
	case VC4_TEXTURE_TYPE_LUMINANCE:
	case VC4_TEXTURE_TYPE_ALPHA:
	case VC4_TEXTURE_TYPE_S8:
		cpp = 1;
		break;
	case VC4_TEXTURE_TYPE_ETC1:
		/* ETC1 is arranged as 64-bit blocks, where each block is 4x4
		 * pixels.
		 */
		cpp = 8;
		width = (width + 3) >> 2;
		height = (height + 3) >> 2;
		break;
	case VC4_TEXTURE_TYPE_BW1:
	case VC4_TEXTURE_TYPE_A4:
	case VC4_TEXTURE_TYPE_A1:
	case VC4_TEXTURE_TYPE_RGBA64:
	case VC4_TEXTURE_TYPE_YUV422R:
	default:
		DRM_DEBUG("Texture format %d unsupported\n", type);
		goto fail;
	}
	utile_w = utile_width(cpp);
	utile_h = utile_height(cpp);

	if (type == VC4_TEXTURE_TYPE_RGBA32R) {
		tiling_format = VC4_TILING_FORMAT_LINEAR;
	} else {
		if (size_is_lt(width, height, cpp))
			tiling_format = VC4_TILING_FORMAT_LT;
		else
			tiling_format = VC4_TILING_FORMAT_T;
	}

	/* air/OS: the faces' offsets in 32 bits (Linux lets them wrap) */
	if ((uint64_t)offset + (uint64_t)cube_map_stride * 5 > UINT32_MAX) {
		DRM_DEBUG("Cube map stride 0x%08x too large\n", cube_map_stride);
		goto fail;
	}

	if (!vc4_check_tex_size(exec, tex, offset + cube_map_stride * 5,
				tiling_format, width, height, cpp)) {
		goto fail;
	}

	/* The mipmap levels are stored before the base of the texture.  Make
	 * sure there is actually space in the BO.
	 */
	for (i = 1; i <= miplevels; i++) {
		uint32_t level_width = std::max(width >> i, 1u);
		uint32_t level_height = std::max(height >> i, 1u);
		uint32_t aligned_width, aligned_height;
		uint32_t level_size;

		/* Once the levels get small enough, they drop from T to LT. */
		if (tiling_format == VC4_TILING_FORMAT_T &&
		    size_is_lt(level_width, level_height, cpp)) {
			tiling_format = VC4_TILING_FORMAT_LT;
		}

		switch (tiling_format) {
		case VC4_TILING_FORMAT_T:
			aligned_width = ROUNDUP(level_width, utile_w * 8);
			aligned_height = ROUNDUP(level_height, utile_h * 8);
			break;
		case VC4_TILING_FORMAT_LT:
			aligned_width = ROUNDUP(level_width, utile_w);
			aligned_height = ROUNDUP(level_height, utile_h);
			break;
		default:
			aligned_width = ROUNDUP(level_width, utile_w);
			aligned_height = level_height;
			break;
		}

		level_size = aligned_width * cpp * aligned_height;

		if (offset < level_size) {
			DRM_DEBUG("Level %d (%dx%d -> %dx%d) size %db "
				  "overflowed buffer bounds (offset %d)\n",
				  i, level_width, level_height,
				  aligned_width, aligned_height,
				  level_size, offset);
			goto fail;
		}

		offset -= level_size;
	}

	put_u32(validated_p0, tex->paddr + p0);

	if (is_cs) {
		exec->bin_dep_seqno = std::max(exec->bin_dep_seqno,
			tex->write_seqno);
	}

	return true;
 fail:
	DRM_INFO("Texture p0 at %d: 0x%08x\n", sample->p_offset[0], p0);
	DRM_INFO("Texture p1 at %d: 0x%08x\n", sample->p_offset[1], p1);
	DRM_INFO("Texture p2 at %d: 0x%08x\n", sample->p_offset[2], p2);
	DRM_INFO("Texture p3 at %d: 0x%08x\n", sample->p_offset[3], p3);
	return false;
}

static int
validate_gl_shader_rec(vc4_exec_info* exec,
		       vc4_exec_info::vc4_shader_state* state)
{
	uint32_t *src_handles;
	uint8_t *pkt_u, *pkt_v;
	static const uint32_t shader_reloc_offsets[] = {
		4, /* fs */
		16, /* vs */
		28, /* cs */
	};
	const uint32_t shader_reloc_count = B_COUNT_OF(shader_reloc_offsets);
	vc4_bo* bo[B_COUNT_OF(shader_reloc_offsets) + 8];
	uint32_t nr_attributes, nr_relocs, packet_size;
	uint32_t i;

	nr_attributes = state->addr & 0x7;
	if (nr_attributes == 0)
		nr_attributes = 8;
	packet_size = gl_shader_rec_size(state->addr);

	nr_relocs = shader_reloc_count + nr_attributes;
	if (nr_relocs * 4 > exec->shader_rec_size) {
		DRM_DEBUG("overflowed shader recs reading %d handles "
			  "from %d bytes left\n",
			  nr_relocs, exec->shader_rec_size);
		return B_BAD_VALUE;
	}
	src_handles = (uint32_t*)exec->shader_rec_u;
	exec->shader_rec_u += nr_relocs * 4;
	exec->shader_rec_size -= nr_relocs * 4;

	if (packet_size > exec->shader_rec_size) {
		DRM_DEBUG("overflowed shader recs copying %db packet "
			  "from %d bytes left\n",
			  packet_size, exec->shader_rec_size);
		return B_BAD_VALUE;
	}
	pkt_u = exec->shader_rec_u;
	pkt_v = exec->shader_rec_v;
	memcpy(pkt_v, pkt_u, packet_size);
	exec->shader_rec_u += packet_size;
	/* Shader recs have to be aligned to 16 bytes (due to the attribute
	 * flags being in the low bytes), so round the next validated shader
	 * rec address up.  This should be safe, since we've got so many
	 * relocations in a shader rec packet.
	 */
	if (ROUNDUP(packet_size, 16) - packet_size > nr_relocs * 4)
		return B_ERROR;
	exec->shader_rec_v += ROUNDUP(packet_size, 16);
	exec->shader_rec_size -= packet_size;

	for (i = 0; i < shader_reloc_count; i++) {
		uint32_t handle = get_u32(&src_handles[i]);
		if (handle >= exec->bo_count) {
			DRM_DEBUG("Shader handle %d too big\n", handle);
			return B_BAD_VALUE;
		}

		bo[i] = exec->bo[handle];
		if (!bo[i] || !bo[i]->validated_shader) {
			DRM_DEBUG("Shader handle %d is no shader\n", handle);
			return B_BAD_VALUE;
		}
	}
	for (i = shader_reloc_count; i < nr_relocs; i++) {
		bo[i] = vc4_use_bo(exec, get_u32(&src_handles[i]));
		if (!bo[i])
			return B_BAD_VALUE;
	}

	if (((get_u16(pkt_u) & VC4_SHADER_FLAG_FS_SINGLE_THREAD) == 0) !=
	    bo[0]->validated_shader->is_threaded) {
		DRM_DEBUG("Thread mode of CL and FS do not match\n");
		return B_BAD_VALUE;
	}

	if (bo[1]->validated_shader->is_threaded ||
	    bo[2]->validated_shader->is_threaded) {
		DRM_DEBUG("cs and vs cannot be threaded\n");
		return B_BAD_VALUE;
	}

	for (i = 0; i < shader_reloc_count; i++) {
		vc4_validated_shader_info* validated_shader;
		uint32_t o = shader_reloc_offsets[i];
		uint32_t src_offset = get_u32(pkt_u + o);
		uint32_t *texture_handles_u;
		uint8_t *uniform_data_u;
		uint32_t tex, uni;

		put_u32(pkt_v + o, bo[i]->paddr + src_offset);

		if (src_offset != 0) {
			DRM_DEBUG("Shaders must be at offset 0 of "
				  "the BO.\n");
			return B_BAD_VALUE;
		}

		validated_shader = bo[i]->validated_shader;

		if (validated_shader->uniforms_src_size >
		    exec->uniforms_size) {
			DRM_DEBUG("Uniforms src buffer overflow\n");
			return B_BAD_VALUE;
		}

		texture_handles_u = (uint32_t*)exec->uniforms_u;
		uniform_data_u = (uint8_t*)(texture_handles_u +
				  validated_shader->num_texture_samples);

		memcpy(exec->uniforms_v, uniform_data_u,
		       validated_shader->uniforms_size);

		for (tex = 0;
		     tex < validated_shader->num_texture_samples;
		     tex++) {
			if (!reloc_tex(exec,
				       uniform_data_u,
				       &validated_shader->texture_samples[tex],
				       get_u32(&texture_handles_u[tex]),
				       i == 2)) {
				return B_BAD_VALUE;
			}
		}

		/* Fill in the uniform slots that need this shader's
		 * start-of-uniforms address (used for resetting the uniform
		 * stream in the presence of control flow).
		 */
		for (uni = 0;
		     uni < validated_shader->num_uniform_addr_offsets;
		     uni++) {
			uint32_t o = validated_shader->uniform_addr_offsets[uni];
			put_u32(exec->uniforms_v + o * 4, exec->uniforms_p);
		}

		put_u32(pkt_v + o + 4, exec->uniforms_p);

		exec->uniforms_u += validated_shader->uniforms_src_size;
		exec->uniforms_size -= validated_shader->uniforms_src_size;
		exec->uniforms_v += validated_shader->uniforms_size;
		exec->uniforms_p += validated_shader->uniforms_size;
	}

	for (i = 0; i < nr_attributes; i++) {
		vc4_bo* vbo = bo[shader_reloc_count + i];
		uint32_t o = 36 + i * 8;
		uint32_t offset = get_u32(pkt_u + o + 0);
		uint32_t attr_size = *(uint8_t *)(pkt_u + o + 4) + 1;
		uint32_t stride = *(uint8_t *)(pkt_u + o + 5);
		uint32_t max_index;

		exec->bin_dep_seqno = std::max(exec->bin_dep_seqno,
			vbo->write_seqno);

		if (state->addr & 0x8)
			stride |= get_u32(pkt_u + 100 + i * 4) & ~0xff;

		if (vbo->size < offset ||
		    vbo->size - offset < attr_size) {
			DRM_DEBUG("BO offset overflow (%d + %d > %d)\n",
				  offset, attr_size, vbo->size);
			return B_BAD_VALUE;
		}

		if (stride != 0) {
			max_index = ((vbo->size - offset - attr_size) /
				     stride);
			if (state->max_index > max_index) {
				DRM_DEBUG("primitives use index %d out of "
					  "supplied %d\n",
					  state->max_index, max_index);
				return B_BAD_VALUE;
			}
		}

		put_u32(pkt_v + o, vbo->paddr + offset);
	}

	return 0;
}

int
vc4_validate_shader_recs(vc4_exec_info* exec)
{
	uint32_t i;
	int ret = 0;

	for (i = 0; i < exec->shader_state_count; i++) {
		ret = validate_gl_shader_rec(exec, &exec->shader_state[i]);
		if (ret)
			return ret;
	}

	return ret;
}
