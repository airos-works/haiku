// Host test of the vc4 driver's validators: tools/rpi3/vc4_host_test/run.sh.
// Copyright 2026, air/OS. All rights reserved. Distributed under the terms of
// the MIT License.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vc4_private.h"
#include "vc4_packet.h"
#include "vc4_qpu_defines.h"

struct vc4_info { int dummy; };
bool gVc4Debug = true;
static uint32 sNextPaddr = 0xc1000000;
static int sNextSlot = 0;
static int sFailures = 0;

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); sFailures++; } else printf("ok   %s\n", #x); } while (0)
#define QPU_SET_FIELD(value, field) (((uint64)(value) << field ## _SHIFT) & field ## _MASK)

static vc4_bo* make_bo(uint32 size) {
	vc4_bo* bo = (vc4_bo*)calloc(1, sizeof(vc4_bo));
	bo->size = ROUNDUP(size, 4096u);
	bo->vaddr = (uint8*)calloc(1, bo->size);
	bo->paddr = sNextPaddr; sNextPaddr += bo->size;
	bo->references = 1;
	return bo;
}
status_t vc4_bo_create_kernel(vc4_info*, uint32 size, vc4_bo** _bo) { *_bo = make_bo(size); return B_OK; }
void vc4_bo_release(vc4_info*, vc4_bo* bo) { free(bo->vaddr); free(bo); }
int vc4_get_bin_slot(vc4_info*) { return sNextSlot++; }
uint32 vc4_bin_slot_address(vc4_info*, int slot) { return 0xc8000000 + slot * 512 * 1024; }
uint32 vc4_bin_alloc_size(vc4_info*) { return 512 * 1024; }

static void dump_cl(const char* name, const uint8* cl, uint32 size) {
	printf("%s (%u bytes):", name, size);
	for (uint32 i = 0; i < size; i++) printf("%s%02x", i % 32 == 0 ? "\n  " : " ", cl[i]);
	printf("\n");
}

static uint64 qpu_nop(uint32 signal) {
	return QPU_SET_FIELD(signal, QPU_SIG) | QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_ADD)
		| QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_MUL) | QPU_SET_FIELD(QPU_A_NOP, QPU_OP_ADD)
		| QPU_SET_FIELD(QPU_M_NOP, QPU_OP_MUL) | QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_A)
		| QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_B);
}

static void init_args(drm_vc4_submit_cl& a) {
	memset(&a, 0, sizeof(a));
	a.color_read.hindex = a.zs_read.hindex = a.color_write.hindex = ~0u;
	a.msaa_color_write.hindex = a.zs_write.hindex = a.msaa_zs_write.hindex = ~0u;
}

static int run_job(drm_vc4_submit_cl& args, vc4_bo** bos, uint32 count, vc4_exec_info& exec) {
	static vc4_info info;
	memset(&exec, 0, sizeof(exec));
	exec.dev = &info; exec.args = &args; exec.bo = bos; exec.bo_count = count;
	int ret = 0;
	if (args.bin_cl_size) ret = vc4_get_bcl(&info, &exec);
	if (ret == 0) ret = vc4_get_rcl(&info, &exec);
	return ret;
}

int main() {
	vc4_bo* surface = make_bo(128 * 96 * 4 + 4096);
	vc4_bo* bos[4] = {surface};
	vc4_exec_info exec;
	drm_vc4_submit_cl args;

	// 1. clear only
	init_args(args);
	args.color_write.hindex = 0;
	args.color_write.bits = VC4_SET_FIELD(VC4_RENDER_CONFIG_FORMAT_RGBA8888, VC4_RENDER_CONFIG_FORMAT);
	args.width = 128; args.height = 96; args.max_x_tile = 1; args.max_y_tile = 1;
	args.flags = VC4_SUBMIT_CL_USE_CLEAR_COLOR; args.clear_color[0] = args.clear_color[1] = 0xff336699;
	CHECK(run_job(args, bos, 1, exec) == 0);
	vc4_bo* rcl = exec.unref_list;
	dump_cl("RCL clear", rcl->vaddr, exec.ct1ea - exec.ct1ca);
	CHECK(exec.ct1ca == rcl->paddr);
	CHECK(rcl->vaddr[0] == VC4_PACKET_CLEAR_COLORS);
	// last byte: store MS tile buffer and EOF
	CHECK(rcl->vaddr[exec.ct1ea - exec.ct1ca - 1] == VC4_PACKET_STORE_MS_TILE_BUFFER_AND_EOF);
	CHECK(exec.rcl_write_bo_count == 1 && exec.rcl_write_bo[0] == surface);

	// 2. clear with an empty binner list
	uint8 bin[16 + 3] = {};
	bin[0] = VC4_PACKET_TILE_BINNING_MODE_CONFIG; bin[13] = 2; bin[14] = 2;
	bin[16] = VC4_PACKET_START_TILE_BINNING; bin[17] = VC4_PACKET_INCREMENT_SEMAPHORE; bin[18] = VC4_PACKET_FLUSH;
	args.bin_cl = (uint64)(uintptr_t)bin; args.bin_cl_size = sizeof(bin);
	CHECK(run_job(args, bos, 1, exec) == 0);
	vc4_bo* execbo = exec.exec_bo;
	dump_cl("BCL validated", execbo->vaddr, exec.ct0ea - exec.ct0ca);
	CHECK(exec.ct0ea - exec.ct0ca == sizeof(bin));
	CHECK(get_u32(execbo->vaddr + 1 + 8) == 0xc8000000);   // tile state: slot 0
	CHECK(get_u32(execbo->vaddr + 1 + 0) == 0xc8000000 + 4096); // tile alloc after a page
	CHECK(execbo->vaddr[15] == (VC4_BIN_CONFIG_AUTO_INIT_TSDA | VC4_SET_FIELD(VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_128, VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE)));
	CHECK(get_u32(execbo->vaddr + 1 + 4) == 512 * 1024 - 4096);
	printf("bin flags %#x\n", execbo->vaddr[15]);
	rcl = exec.unref_list;
	dump_cl("RCL with bin", rcl->vaddr, exec.ct1ea - exec.ct1ca);

	// 3. bounds
	args.bin_cl_size = 0;
	args.max_x_tile = 2;
	CHECK(run_job(args, bos, 1, exec) == B_BAD_VALUE);
	args.max_x_tile = 1; args.width = 1024; args.height = 1024; args.max_x_tile = 15; args.max_y_tile = 15;
	CHECK(run_job(args, bos, 1, exec) == B_BAD_VALUE);

	// 4. shaders
	uint64 good[3] = {qpu_nop(QPU_SIG_PROG_END), qpu_nop(QPU_SIG_NONE), qpu_nop(QPU_SIG_NONE)};
	vc4_validated_shader_info* fs = vc4_validate_shader(good, sizeof(good));
	CHECK(fs != NULL && fs->uniforms_size == 0 && !fs->is_threaded);
	uint64 vpm[4] = {qpu_nop(QPU_SIG_NONE), qpu_nop(QPU_SIG_PROG_END), qpu_nop(QPU_SIG_NONE), qpu_nop(QPU_SIG_NONE)};
	vpm[0] = (vpm[0] & ~QPU_WADDR_ADD_MASK) | QPU_SET_FIELD(QPU_W_VPM_ADDR, QPU_WADDR_ADD);
	CHECK(vc4_validate_shader(vpm, sizeof(vpm)) == NULL);
	uint64 endless[2] = {qpu_nop(QPU_SIG_NONE), qpu_nop(QPU_SIG_NONE)};
	CHECK(vc4_validate_shader(endless, sizeof(endless)) == NULL);
	// a uniform read: raddr_a = UNIF
	uint64 unif[3] = {qpu_nop(QPU_SIG_PROG_END), qpu_nop(QPU_SIG_NONE), qpu_nop(QPU_SIG_NONE)};
	unif[0] = (unif[0] & ~QPU_RADDR_A_MASK) | QPU_SET_FIELD(QPU_R_UNIF, QPU_RADDR_A);
	vc4_validated_shader_info* vs = vc4_validate_shader(unif, sizeof(unif));
	CHECK(vs != NULL && vs->uniforms_size == 4 && vs->uniforms_src_size == 4);

	// 5. a draw: shader state with one attribute, 3 vertices
	vc4_bo* fsbo = make_bo(4096); fsbo->validated_shader = fs;
	vc4_bo* vsbo = make_bo(4096); vsbo->validated_shader = vs;
	vc4_bo* csbo = make_bo(4096); csbo->validated_shader = vc4_validate_shader(good, sizeof(good));
	vc4_bo* vbo = make_bo(4096);
	vc4_bo* draw_bos[5] = {surface, fsbo, vsbo, csbo, vbo};
	uint8 dbin[16 + 1 + 5 + 10 + 1 + 1] = {};
	uint32 o = 0;
	dbin[o] = VC4_PACKET_TILE_BINNING_MODE_CONFIG; dbin[o + 13] = 2; dbin[o + 14] = 2; o += 16;
	dbin[o++] = VC4_PACKET_START_TILE_BINNING;
	dbin[o] = VC4_PACKET_GL_SHADER_STATE; put_u32(dbin + o + 1, 1); o += 5;
	dbin[o] = VC4_PACKET_GL_ARRAY_PRIMITIVE; dbin[o + 1] = 4; put_u32(dbin + o + 2, 3); put_u32(dbin + o + 6, 0); o += 10;
	dbin[o++] = VC4_PACKET_INCREMENT_SEMAPHORE;
	dbin[o++] = VC4_PACKET_FLUSH;
	uint8 rec[16 + 44] = {};
	put_u32(rec + 0, 1); put_u32(rec + 4, 2); put_u32(rec + 8, 3); put_u32(rec + 12, 4);
	uint8* r = rec + 16;
	put_u16(r, VC4_SHADER_FLAG_FS_SINGLE_THREAD);
	r[36 + 4] = 11; r[36 + 5] = 12;   // 12 byte attribute, stride 12
	uint32 uniforms[1] = {0x3f800000};   // the VS's one uniform
	init_args(args);
	args.color_write.hindex = 0;
	args.color_write.bits = VC4_SET_FIELD(VC4_RENDER_CONFIG_FORMAT_RGBA8888, VC4_RENDER_CONFIG_FORMAT);
	args.width = 128; args.height = 96; args.max_x_tile = 1; args.max_y_tile = 1;
	args.bin_cl = (uint64)(uintptr_t)dbin; args.bin_cl_size = o;
	args.shader_rec = (uint64)(uintptr_t)rec; args.shader_rec_size = sizeof(rec); args.shader_rec_count = 1;
	args.uniforms = (uint64)(uintptr_t)uniforms; args.uniforms_size = sizeof(uniforms);
	CHECK(run_job(args, draw_bos, 5, exec) == 0);
	execbo = exec.exec_bo;
	uint32 recOffset = ROUNDUP(o, 16u);
	uint8* vrec = execbo->vaddr + recOffset;
	dump_cl("validated shader record", vrec, 44);
	CHECK(get_u32(execbo->vaddr + 17 + 1) == execbo->paddr + recOffset + 1);  // GL_SHADER_STATE -> record | attr count
	CHECK(get_u32(vrec + 4) == fsbo->paddr && get_u32(vrec + 16) == vsbo->paddr && get_u32(vrec + 28) == csbo->paddr);
	uint32 uniformsP = execbo->paddr + recOffset + sizeof(rec);
	CHECK(get_u32(vrec + 8) == uniformsP);           // FS: no uniforms
	CHECK(get_u32(vrec + 20) == uniformsP);          // VS: one
	CHECK(get_u32(vrec + 32) == uniformsP + 4);      // CS after it
	CHECK(get_u32(vrec + 36) == vbo->paddr);
	CHECK(get_u32(execbo->vaddr + recOffset + sizeof(rec)) == 0x3f800000);
	// too many vertices for the buffer: 4096/12 = 341 + 1
	put_u32(dbin + 22 + 2, 343);
	CHECK(run_job(args, draw_bos, 5, exec) == B_BAD_VALUE);
	put_u32(dbin + 22 + 2, 3);
	// two shader records' uniforms from one's worth of data
	put_u32(rec + 8, 2);   // CS = the VS (one uniform) as well: needs 8 bytes
	CHECK(run_job(args, draw_bos, 5, exec) == B_BAD_VALUE);
	put_u32(rec + 8, 3);
	// a handle one past the end
	put_u32(rec + 0, 5);
	CHECK(run_job(args, draw_bos, 5, exec) == B_BAD_VALUE);
	put_u32(rec + 0, 1);
	// the VBO as a shader
	put_u32(rec + 0, 4);
	CHECK(run_job(args, draw_bos, 5, exec) == B_BAD_VALUE);
	put_u32(rec + 0, 1);
	// a shader as the VBO
	put_u32(rec + 12, 1);
	CHECK(run_job(args, draw_bos, 5, exec) == B_BAD_VALUE);
	put_u32(rec + 12, 4);
	CHECK(run_job(args, draw_bos, 5, exec) == 0);

	printf("%d failures\n", sFailures);
	return sFailures;
}
