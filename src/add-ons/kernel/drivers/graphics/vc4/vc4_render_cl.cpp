/*
 * Copyright © 2014-2015 Broadcom
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
 * DOC: Render command list generation
 *
 * In the V3D hardware, render command lists are what load and store
 * tiles of a framebuffer and optionally call out to binner-generated
 * command lists to do the 3D drawing for that tile.
 *
 * In the VC4 driver, render command list generation is performed by the
 * kernel instead of userspace.  We do this because validating a
 * user-submitted command list is hard to get right and has high CPU overhead,
 * while the number of valid configurations for render command lists is
 * actually fairly low.
 *
 * air/OS: Linux' drivers/gpu/drm/vc4/vc4_render_cl.c (rpi-6.6.y) in C++ on
 * the driver's vc4_bo. Differences: the render tiles must lie within the
 * frame (and with a binner list, within its tiles: Linux lets the maximum
 * reach one past them), and a full resolution surface must hold the last
 * tile whole.
 */

#include "vc4_packet.h"
#include "vc4_private.h"


#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))

struct vc4_rcl_setup {
	vc4_bo *color_read;
	vc4_bo *color_write;
	vc4_bo *zs_read;
	vc4_bo *zs_write;
	vc4_bo *msaa_color_write;
	vc4_bo *msaa_zs_write;

	vc4_bo *rcl;
	uint32_t next_offset;
	uint32_t size;

	uint32_t next_write_bo_index;
};

/* The writes stop at the end of the list: its size was computed from the
 * same tiles and surfaces (Linux trusts it; a mistake here would write
 * past the BO). */
static inline void rcl_u8(vc4_rcl_setup *setup, uint8_t val)
{
	if (setup->next_offset + 1 <= setup->size)
		setup->rcl->vaddr[setup->next_offset] = val;
	setup->next_offset += 1;
}

static inline void rcl_u16(vc4_rcl_setup *setup, uint16_t val)
{
	if (setup->next_offset + 2 <= setup->size)
		put_u16(setup->rcl->vaddr + setup->next_offset, val);
	setup->next_offset += 2;
}

static inline void rcl_u32(vc4_rcl_setup *setup, uint32_t val)
{
	if (setup->next_offset + 4 <= setup->size)
		put_u32(setup->rcl->vaddr + setup->next_offset, val);
	setup->next_offset += 4;
}

/*
 * Emits a no-op STORE_TILE_BUFFER_GENERAL.
 *
 * If we emit a PACKET_TILE_COORDINATES, it must be followed by a store of
 * some sort before another load is triggered.
 */
static void vc4_store_before_load(vc4_rcl_setup *setup)
{
	rcl_u8(setup, VC4_PACKET_STORE_TILE_BUFFER_GENERAL);
	rcl_u16(setup,
		VC4_SET_FIELD(VC4_LOADSTORE_TILE_BUFFER_NONE,
			      VC4_LOADSTORE_TILE_BUFFER_BUFFER) |
		VC4_STORE_TILE_BUFFER_DISABLE_COLOR_CLEAR |
		VC4_STORE_TILE_BUFFER_DISABLE_ZS_CLEAR |
		VC4_STORE_TILE_BUFFER_DISABLE_VG_MASK_CLEAR);
	rcl_u32(setup, 0); /* no address, since we're in None mode */
}

/*
 * Calculates the physical address of the start of a tile in a RCL surface.
 *
 * Unlike the other load/store packets,
 * VC4_PACKET_LOAD/STORE_FULL_RES_TILE_BUFFER don't look at the tile
 * coordinates packet, and instead just store to the address given.
 */
static uint32_t vc4_full_res_offset(vc4_exec_info *exec,
				    vc4_bo *bo,
				    struct drm_vc4_submit_rcl_surface *surf,
				    uint8_t x, uint8_t y)
{
	return bo->paddr + surf->offset + VC4_TILE_BUFFER_SIZE *
		(DIV_ROUND_UP(exec->args->width, 32) * y + x);
}

/*
 * Emits a PACKET_TILE_COORDINATES if one isn't already pending.
 *
 * The tile coordinates packet triggers a pending load if there is one, are
 * used for clipping during rendering, and determine where loads/stores happen
 * relative to their base address.
 */
static void vc4_tile_coordinates(vc4_rcl_setup *setup,
				 uint32_t x, uint32_t y)
{
	rcl_u8(setup, VC4_PACKET_TILE_COORDINATES);
	rcl_u8(setup, x);
	rcl_u8(setup, y);
}

static void emit_tile(vc4_exec_info *exec,
		      vc4_rcl_setup *setup,
		      uint8_t x, uint8_t y, bool first, bool last)
{
	struct drm_vc4_submit_cl *args = exec->args;
	bool has_bin = args->bin_cl_size != 0;

	/* Note that the load doesn't actually occur until the
	 * tile coords packet is processed, and only one load
	 * may be outstanding at a time.
	 */
	if (setup->color_read) {
		if (args->color_read.flags &
		    VC4_SUBMIT_RCL_SURFACE_READ_IS_FULL_RES) {
			rcl_u8(setup, VC4_PACKET_LOAD_FULL_RES_TILE_BUFFER);
			rcl_u32(setup,
				vc4_full_res_offset(exec, setup->color_read,
						    &args->color_read, x, y) |
				VC4_LOADSTORE_FULL_RES_DISABLE_ZS);
		} else {
			rcl_u8(setup, VC4_PACKET_LOAD_TILE_BUFFER_GENERAL);
			rcl_u16(setup, args->color_read.bits);
			rcl_u32(setup, setup->color_read->paddr +
				args->color_read.offset);
		}
	}

	if (setup->zs_read) {
		if (setup->color_read) {
			/* Exec previous load. */
			vc4_tile_coordinates(setup, x, y);
			vc4_store_before_load(setup);
		}

		if (args->zs_read.flags &
		    VC4_SUBMIT_RCL_SURFACE_READ_IS_FULL_RES) {
			rcl_u8(setup, VC4_PACKET_LOAD_FULL_RES_TILE_BUFFER);
			rcl_u32(setup,
				vc4_full_res_offset(exec, setup->zs_read,
						    &args->zs_read, x, y) |
				VC4_LOADSTORE_FULL_RES_DISABLE_COLOR);
		} else {
			rcl_u8(setup, VC4_PACKET_LOAD_TILE_BUFFER_GENERAL);
			rcl_u16(setup, args->zs_read.bits);
			rcl_u32(setup, setup->zs_read->paddr +
				args->zs_read.offset);
		}
	}

	/* Clipping depends on tile coordinates having been
	 * emitted, so we always need one here.
	 */
	vc4_tile_coordinates(setup, x, y);

	/* Wait for the binner before jumping to the first
	 * tile's lists.
	 */
	if (first && has_bin)
		rcl_u8(setup, VC4_PACKET_WAIT_ON_SEMAPHORE);

	if (has_bin) {
		rcl_u8(setup, VC4_PACKET_BRANCH_TO_SUB_LIST);
		rcl_u32(setup, (exec->tile_alloc_offset +
				(y * exec->bin_tiles_x + x) * 32));
	}

	if (setup->msaa_color_write) {
		bool last_tile_write = (!setup->msaa_zs_write &&
					!setup->zs_write &&
					!setup->color_write);
		uint32_t bits = VC4_LOADSTORE_FULL_RES_DISABLE_ZS;

		if (!last_tile_write)
			bits |= VC4_LOADSTORE_FULL_RES_DISABLE_CLEAR_ALL;
		else if (last)
			bits |= VC4_LOADSTORE_FULL_RES_EOF;
		rcl_u8(setup, VC4_PACKET_STORE_FULL_RES_TILE_BUFFER);
		rcl_u32(setup,
			vc4_full_res_offset(exec, setup->msaa_color_write,
					    &args->msaa_color_write, x, y) |
			bits);
	}

	if (setup->msaa_zs_write) {
		bool last_tile_write = (!setup->zs_write &&
					!setup->color_write);
		uint32_t bits = VC4_LOADSTORE_FULL_RES_DISABLE_COLOR;

		if (setup->msaa_color_write)
			vc4_tile_coordinates(setup, x, y);
		if (!last_tile_write)
			bits |= VC4_LOADSTORE_FULL_RES_DISABLE_CLEAR_ALL;
		else if (last)
			bits |= VC4_LOADSTORE_FULL_RES_EOF;
		rcl_u8(setup, VC4_PACKET_STORE_FULL_RES_TILE_BUFFER);
		rcl_u32(setup,
			vc4_full_res_offset(exec, setup->msaa_zs_write,
					    &args->msaa_zs_write, x, y) |
			bits);
	}

	if (setup->zs_write) {
		bool last_tile_write = !setup->color_write;

		if (setup->msaa_color_write || setup->msaa_zs_write)
			vc4_tile_coordinates(setup, x, y);

		rcl_u8(setup, VC4_PACKET_STORE_TILE_BUFFER_GENERAL);
		rcl_u16(setup, args->zs_write.bits |
			(last_tile_write ?
			 0 : VC4_STORE_TILE_BUFFER_DISABLE_COLOR_CLEAR));
		rcl_u32(setup,
			(setup->zs_write->paddr + args->zs_write.offset) |
			((last && last_tile_write) ?
			 VC4_LOADSTORE_TILE_BUFFER_EOF : 0));
	}

	if (setup->color_write) {
		if (setup->msaa_color_write || setup->msaa_zs_write ||
		    setup->zs_write) {
			vc4_tile_coordinates(setup, x, y);
		}

		if (last)
			rcl_u8(setup, VC4_PACKET_STORE_MS_TILE_BUFFER_AND_EOF);
		else
			rcl_u8(setup, VC4_PACKET_STORE_MS_TILE_BUFFER);
	}
}

static int vc4_create_rcl_bo(vc4_info *dev, vc4_exec_info *exec,
			     vc4_rcl_setup *setup)
{
	struct drm_vc4_submit_cl *args = exec->args;
	bool has_bin = args->bin_cl_size != 0;
	uint8_t min_x_tile = args->min_x_tile;
	uint8_t min_y_tile = args->min_y_tile;
	uint8_t max_x_tile = args->max_x_tile;
	uint8_t max_y_tile = args->max_y_tile;
	uint8_t xtiles = max_x_tile - min_x_tile + 1;
	uint8_t ytiles = max_y_tile - min_y_tile + 1;
	uint8_t xi, yi;
	uint32_t size, loop_body_size;
	bool positive_x = true;
	bool positive_y = true;

	if (args->flags & VC4_SUBMIT_CL_FIXED_RCL_ORDER) {
		if (!(args->flags & VC4_SUBMIT_CL_RCL_ORDER_INCREASING_X))
			positive_x = false;
		if (!(args->flags & VC4_SUBMIT_CL_RCL_ORDER_INCREASING_Y))
			positive_y = false;
	}

	size = VC4_PACKET_TILE_RENDERING_MODE_CONFIG_SIZE;
	loop_body_size = VC4_PACKET_TILE_COORDINATES_SIZE;

	if (args->flags & VC4_SUBMIT_CL_USE_CLEAR_COLOR) {
		size += VC4_PACKET_CLEAR_COLORS_SIZE +
			VC4_PACKET_TILE_COORDINATES_SIZE +
			VC4_PACKET_STORE_TILE_BUFFER_GENERAL_SIZE;
	}

	if (setup->color_read) {
		if (args->color_read.flags &
		    VC4_SUBMIT_RCL_SURFACE_READ_IS_FULL_RES) {
			loop_body_size += VC4_PACKET_LOAD_FULL_RES_TILE_BUFFER_SIZE;
		} else {
			loop_body_size += VC4_PACKET_LOAD_TILE_BUFFER_GENERAL_SIZE;
		}
	}
	if (setup->zs_read) {
		if (setup->color_read) {
			loop_body_size += VC4_PACKET_TILE_COORDINATES_SIZE;
			loop_body_size += VC4_PACKET_STORE_TILE_BUFFER_GENERAL_SIZE;
		}

		if (args->zs_read.flags &
		    VC4_SUBMIT_RCL_SURFACE_READ_IS_FULL_RES) {
			loop_body_size += VC4_PACKET_LOAD_FULL_RES_TILE_BUFFER_SIZE;
		} else {
			loop_body_size += VC4_PACKET_LOAD_TILE_BUFFER_GENERAL_SIZE;
		}
	}

	if (has_bin) {
		size += VC4_PACKET_WAIT_ON_SEMAPHORE_SIZE;
		loop_body_size += VC4_PACKET_BRANCH_TO_SUB_LIST_SIZE;
	}

	if (setup->msaa_color_write)
		loop_body_size += VC4_PACKET_STORE_FULL_RES_TILE_BUFFER_SIZE;
	if (setup->msaa_zs_write)
		loop_body_size += VC4_PACKET_STORE_FULL_RES_TILE_BUFFER_SIZE;

	if (setup->zs_write)
		loop_body_size += VC4_PACKET_STORE_TILE_BUFFER_GENERAL_SIZE;
	if (setup->color_write)
		loop_body_size += VC4_PACKET_STORE_MS_TILE_BUFFER_SIZE;

	/* We need a VC4_PACKET_TILE_COORDINATES in between each store. */
	loop_body_size += VC4_PACKET_TILE_COORDINATES_SIZE *
		((setup->msaa_color_write != NULL) +
		 (setup->msaa_zs_write != NULL) +
		 (setup->color_write != NULL) +
		 (setup->zs_write != NULL) - 1);

	size += xtiles * ytiles * loop_body_size;

	status_t status = vc4_bo_create_kernel(dev, size, &setup->rcl);
	if (status != B_OK)
		return status;
	setup->size = size;
	setup->rcl->unref_next = exec->unref_list;
	exec->unref_list = setup->rcl;

	/* The tile buffer gets cleared when the previous tile is stored.  If
	 * the clear values changed between frames, then the tile buffer has
	 * stale clear values in it, so we have to do a store in None mode (no
	 * writes) so that we trigger the tile buffer clear.
	 */
	if (args->flags & VC4_SUBMIT_CL_USE_CLEAR_COLOR) {
		rcl_u8(setup, VC4_PACKET_CLEAR_COLORS);
		rcl_u32(setup, args->clear_color[0]);
		rcl_u32(setup, args->clear_color[1]);
		rcl_u32(setup, args->clear_z);
		rcl_u8(setup, args->clear_s);

		vc4_tile_coordinates(setup, 0, 0);

		rcl_u8(setup, VC4_PACKET_STORE_TILE_BUFFER_GENERAL);
		rcl_u16(setup, VC4_LOADSTORE_TILE_BUFFER_NONE);
		rcl_u32(setup, 0); /* no address, since we're in None mode */
	}

	rcl_u8(setup, VC4_PACKET_TILE_RENDERING_MODE_CONFIG);
	rcl_u32(setup,
		(setup->color_write ? (setup->color_write->paddr +
				       args->color_write.offset) :
		 0));
	rcl_u16(setup, args->width);
	rcl_u16(setup, args->height);
	rcl_u16(setup, args->color_write.bits);

	for (yi = 0; yi < ytiles; yi++) {
		int y = positive_y ? min_y_tile + yi : max_y_tile - yi;
		for (xi = 0; xi < xtiles; xi++) {
			int x = positive_x ? min_x_tile + xi : max_x_tile - xi;
			bool first = (xi == 0 && yi == 0);
			bool last = (xi == xtiles - 1 && yi == ytiles - 1);

			emit_tile(exec, setup, x, y, first, last);
		}
	}

	if (setup->next_offset != size) {
		DRM_ERROR("render list of %u bytes instead of %u\n",
			  setup->next_offset, size);
		return B_ERROR;
	}
	exec->ct1ca = setup->rcl->paddr;
	exec->ct1ea = setup->rcl->paddr + setup->next_offset;

	return 0;
}

static int vc4_full_res_bounds_check(vc4_exec_info *exec,
				     vc4_bo *obj,
				     struct drm_vc4_submit_rcl_surface *surf)
{
	struct drm_vc4_submit_cl *args = exec->args;
	uint32_t render_tiles_stride = DIV_ROUND_UP(exec->args->width, 32);

	if (surf->offset > obj->size) {
		DRM_DEBUG("surface offset %d > BO size %u\n",
			  surf->offset, obj->size);
		return B_BAD_VALUE;
	}

	if ((obj->size - surf->offset) / VC4_TILE_BUFFER_SIZE <=
	    render_tiles_stride * args->max_y_tile + args->max_x_tile) {
		DRM_DEBUG("MSAA tile %d, %d out of bounds "
			  "(bo size %u, offset %d).\n",
			  args->max_x_tile, args->max_y_tile,
			  obj->size,
			  surf->offset);
		return B_BAD_VALUE;
	}

	return 0;
}

static int vc4_rcl_msaa_surface_setup(vc4_exec_info *exec,
				      vc4_bo **obj,
				      struct drm_vc4_submit_rcl_surface *surf)
{
	if (surf->flags != 0 || surf->bits != 0) {
		DRM_DEBUG("MSAA surface had nonzero flags/bits\n");
		return B_BAD_VALUE;
	}

	if (surf->hindex == ~0u)
		return 0;

	*obj = vc4_use_bo(exec, surf->hindex);
	if (!*obj)
		return B_BAD_VALUE;

	exec->rcl_write_bo[exec->rcl_write_bo_count++] = *obj;

	if (surf->offset & 0xf) {
		DRM_DEBUG("MSAA write must be 16b aligned.\n");
		return B_BAD_VALUE;
	}

	return vc4_full_res_bounds_check(exec, *obj, surf);
}

static int vc4_rcl_surface_setup(vc4_exec_info *exec,
				 vc4_bo **obj,
				 struct drm_vc4_submit_rcl_surface *surf,
				 bool is_write)
{
	uint8_t tiling = VC4_GET_FIELD(surf->bits,
				       VC4_LOADSTORE_TILE_BUFFER_TILING);
	uint8_t buffer = VC4_GET_FIELD(surf->bits,
				       VC4_LOADSTORE_TILE_BUFFER_BUFFER);
	uint8_t format = VC4_GET_FIELD(surf->bits,
				       VC4_LOADSTORE_TILE_BUFFER_FORMAT);
	int cpp;
	int ret;

	if (surf->flags & ~VC4_SUBMIT_RCL_SURFACE_READ_IS_FULL_RES) {
		DRM_DEBUG("Extra flags set\n");
		return B_BAD_VALUE;
	}

	if (surf->hindex == ~0u)
		return 0;

	*obj = vc4_use_bo(exec, surf->hindex);
	if (!*obj)
		return B_BAD_VALUE;

	if (is_write)
		exec->rcl_write_bo[exec->rcl_write_bo_count++] = *obj;

	if (surf->flags & VC4_SUBMIT_RCL_SURFACE_READ_IS_FULL_RES) {
		if (surf == &exec->args->zs_write) {
			DRM_DEBUG("general zs write may not be a full-res.\n");
			return B_BAD_VALUE;
		}

		if (surf->bits != 0) {
			DRM_DEBUG("load/store general bits set with "
				  "full res load/store.\n");
			return B_BAD_VALUE;
		}

		ret = vc4_full_res_bounds_check(exec, *obj, surf);
		if (ret)
			return ret;

		return 0;
	}

	if (surf->bits & ~(VC4_LOADSTORE_TILE_BUFFER_TILING_MASK |
			   VC4_LOADSTORE_TILE_BUFFER_BUFFER_MASK |
			   VC4_LOADSTORE_TILE_BUFFER_FORMAT_MASK)) {
		DRM_DEBUG("Unknown bits in load/store: 0x%04x\n",
			  surf->bits);
		return B_BAD_VALUE;
	}

	if (tiling > VC4_TILING_FORMAT_LT) {
		DRM_DEBUG("Bad tiling format\n");
		return B_BAD_VALUE;
	}

	if (buffer == VC4_LOADSTORE_TILE_BUFFER_ZS) {
		if (format != 0) {
			DRM_DEBUG("No color format should be set for ZS\n");
			return B_BAD_VALUE;
		}
		cpp = 4;
	} else if (buffer == VC4_LOADSTORE_TILE_BUFFER_COLOR) {
		switch (format) {
		case VC4_LOADSTORE_TILE_BUFFER_BGR565:
		case VC4_LOADSTORE_TILE_BUFFER_BGR565_DITHER:
			cpp = 2;
			break;
		case VC4_LOADSTORE_TILE_BUFFER_RGBA8888:
			cpp = 4;
			break;
		default:
			DRM_DEBUG("Bad tile buffer format\n");
			return B_BAD_VALUE;
		}
	} else {
		DRM_DEBUG("Bad load/store buffer %d.\n", buffer);
		return B_BAD_VALUE;
	}

	if (surf->offset & 0xf) {
		DRM_DEBUG("load/store buffer must be 16b aligned.\n");
		return B_BAD_VALUE;
	}

	if (!vc4_check_tex_size(exec, *obj, surf->offset, tiling,
				exec->args->width, exec->args->height, cpp)) {
		return B_BAD_VALUE;
	}

	return 0;
}

static int
vc4_rcl_render_config_surface_setup(vc4_exec_info *exec,
				    vc4_rcl_setup *setup,
				    vc4_bo **obj,
				    struct drm_vc4_submit_rcl_surface *surf)
{
	uint8_t tiling = VC4_GET_FIELD(surf->bits,
				       VC4_RENDER_CONFIG_MEMORY_FORMAT);
	uint8_t format = VC4_GET_FIELD(surf->bits,
				       VC4_RENDER_CONFIG_FORMAT);
	int cpp;

	if (surf->flags != 0) {
		DRM_DEBUG("No flags supported on render config.\n");
		return B_BAD_VALUE;
	}

	if (surf->bits & ~(VC4_RENDER_CONFIG_MEMORY_FORMAT_MASK |
			   VC4_RENDER_CONFIG_FORMAT_MASK |
			   VC4_RENDER_CONFIG_MS_MODE_4X |
			   VC4_RENDER_CONFIG_DECIMATE_MODE_4X)) {
		DRM_DEBUG("Unknown bits in render config: 0x%04x\n",
			  surf->bits);
		return B_BAD_VALUE;
	}

	if (surf->hindex == ~0u)
		return 0;

	*obj = vc4_use_bo(exec, surf->hindex);
	if (!*obj)
		return B_BAD_VALUE;

	exec->rcl_write_bo[exec->rcl_write_bo_count++] = *obj;

	if (tiling > VC4_TILING_FORMAT_LT) {
		DRM_DEBUG("Bad tiling format\n");
		return B_BAD_VALUE;
	}

	switch (format) {
	case VC4_RENDER_CONFIG_FORMAT_BGR565_DITHERED:
	case VC4_RENDER_CONFIG_FORMAT_BGR565:
		cpp = 2;
		break;
	case VC4_RENDER_CONFIG_FORMAT_RGBA8888:
		cpp = 4;
		break;
	default:
		DRM_DEBUG("Bad tile buffer format\n");
		return B_BAD_VALUE;
	}

	if (!vc4_check_tex_size(exec, *obj, surf->offset, tiling,
				exec->args->width, exec->args->height, cpp)) {
		return B_BAD_VALUE;
	}

	return 0;
}

int vc4_get_rcl(vc4_info *dev, vc4_exec_info *exec)
{
	vc4_rcl_setup setup = {};
	struct drm_vc4_submit_cl *args = exec->args;
	bool has_bin = args->bin_cl_size != 0;
	int ret;
	/* the tiles are 32 pixels square with 4x multisampling, 64 without
	 * (as Mesa's vc4_job.c sets them up) */
	uint32_t tile_size =
		(args->color_write.bits & VC4_RENDER_CONFIG_MS_MODE_4X) != 0
			? 32 : 64;

	if (args->min_x_tile > args->max_x_tile ||
	    args->min_y_tile > args->max_y_tile) {
		DRM_DEBUG("Bad render tile set (%d,%d)-(%d,%d)\n",
			  args->min_x_tile, args->min_y_tile,
			  args->max_x_tile, args->max_y_tile);
		return B_BAD_VALUE;
	}

	/* as vc4_check_tex_size() limits the surfaces: this keeps the tile
	 * counts in their 8 bits */
	if (args->width == 0 || args->height == 0 || args->width > 4096 ||
	    args->height > 4096) {
		DRM_DEBUG("Frame of %dx%d\n", args->width, args->height);
		return B_BAD_VALUE;
	}

	if (args->max_x_tile >= DIV_ROUND_UP(args->width, tile_size) ||
	    args->max_y_tile >= DIV_ROUND_UP(args->height, tile_size)) {
		DRM_DEBUG("Render tiles (%d,%d) outside of the %dx%d frame\n",
			  args->max_x_tile, args->max_y_tile,
			  args->width, args->height);
		return B_BAD_VALUE;
	}

	if (has_bin &&
	    (args->max_x_tile >= exec->bin_tiles_x ||
	     args->max_y_tile >= exec->bin_tiles_y)) {
		DRM_DEBUG("Render tiles (%d,%d) outside of bin config "
			  "(%d,%d)\n",
			  args->max_x_tile, args->max_y_tile,
			  exec->bin_tiles_x, exec->bin_tiles_y);
		return B_BAD_VALUE;
	}

	ret = vc4_rcl_render_config_surface_setup(exec, &setup,
						  &setup.color_write,
						  &args->color_write);
	if (ret)
		return ret;

	ret = vc4_rcl_surface_setup(exec, &setup.color_read, &args->color_read,
				    false);
	if (ret)
		return ret;

	ret = vc4_rcl_surface_setup(exec, &setup.zs_read, &args->zs_read,
				    false);
	if (ret)
		return ret;

	ret = vc4_rcl_surface_setup(exec, &setup.zs_write, &args->zs_write,
				    true);
	if (ret)
		return ret;

	ret = vc4_rcl_msaa_surface_setup(exec, &setup.msaa_color_write,
					 &args->msaa_color_write);
	if (ret)
		return ret;

	ret = vc4_rcl_msaa_surface_setup(exec, &setup.msaa_zs_write,
					 &args->msaa_zs_write);
	if (ret)
		return ret;

	/* We shouldn't even have the job submitted to us if there's no
	 * surface to write out.
	 */
	if (!setup.color_write && !setup.zs_write &&
	    !setup.msaa_color_write && !setup.msaa_zs_write) {
		DRM_DEBUG("RCL requires color or Z/S write\n");
		return B_BAD_VALUE;
	}

	return vc4_create_rcl_bo(dev, exec, &setup);
}
