/* Copyright 2026, air/OS. Distributed under the terms of the MIT License.
 * Assemble with llvm-mc -triple=amdgcn -mcpu=gfx803 -filetype=obj.
 * s[0:1]: output, s2: seed, s[3:4]: owned data, s5: workgroup, v0: lane.
 * Each workgroup owns 512 bytes: a 256-byte resource and 256-byte guard.
 * All attempted out-of-resource accesses still address owned GPUVM memory.
 * AMD GCN3 ISA rev1.1 chapters 7-10: scalar, buffer, flat and LDS accesses.
 */
.text
.p2align 8
shader_resource:
    s_mov_b32 s9, s4
    s_lshl_b32 s8, s5, 9
    s_add_u32 s8, s8, s3
    s_addc_u32 s9, s9, 0
    s_mov_b32 s10, 256
    // X/Y/Z/W destination selects, 32-bit unsigned format (gfx8 SQ_BUF_RSRC).
    s_mov_b32 s11, (4 | (5 << 3) | (6 << 6) | (7 << 9) | (4 << 12) | (4 << 15))
    v_lshlrev_b32 v1, 2, v0
    v_add_u32 v2, vcc, s8, v1
    v_mov_b32 v3, s9
    v_addc_u32 v3, vcc, v3, 0, vcc
    buffer_load_dword v4, v1, s[8:11], 0 offen glc
    flat_load_dword v5, v[2:3] glc
    s_load_dword s12, s[8:9], 0 glc
    s_buffer_load_dword s13, s[8:11], 252 glc
    s_waitcnt vmcnt(0) lgkmcnt(0)

    // Shader-generated zero-size and offset-outside-resource descriptors.
    // Every prospective address is in the owned data BO, including guards.
    s_mov_b32 s10, 0
    buffer_load_dword v6, v1, s[8:11], 0 offen glc
    s_mov_b32 s10, 256
    v_add_u32 v10, vcc, 256, v1
    buffer_load_dword v7, v10, s[8:11], 0 offen glc
    buffer_store_dword v0, v10, s[8:11], 0 offen glc
    v_mov_b32 v11, 1
    buffer_atomic_add v11, v10, s[8:11], 0 offen glc
    s_buffer_store_dword s2, s[8:11], 256 glc
    s_waitcnt vmcnt(0) lgkmcnt(0)

    // In-range vector store and LDS load/store positive controls.
    v_xor_b32 v12, s2, v4
    buffer_store_dword v12, v1, s[8:11], 0 offen glc
    s_mov_b32 m0, 256
    ds_write_b32 v1, v12
    s_waitcnt lgkmcnt(0)
    ds_read_b32 v13, v1
    s_waitcnt vmcnt(0) lgkmcnt(0)
    // Positive controls for both kinds of operation used in the OOB tests.
    v_mov_b32 v15, 1
    buffer_atomic_add v15, v1, s[8:11], 0 offen glc
    s_waitcnt vmcnt(0)
    s_buffer_store_dword s2, s[8:11], 252 glc
    s_waitcnt lgkmcnt(0)
    s_buffer_load_dword s14, s[8:11], 252 glc
    s_waitcnt lgkmcnt(0)
    v_mov_b32 v8, s12
    v_mov_b32 v9, s13

    // 16-DWORD output: 15 observations and one untouched guard.
    v_mad_u32_u24 v14, s5, 64, v0
    v_lshlrev_b32 v2, 6, v14
    v_add_u32 v2, vcc, s0, v2
    v_mov_b32 v3, s1
    v_addc_u32 v3, vcc, v3, 0, vcc
    flat_store_dwordx4 v[2:3], v[4:7]
    v_add_u32 v2, vcc, 16, v2
    v_addc_u32 v3, vcc, v3, 0, vcc
    flat_store_dwordx2 v[2:3], v[8:9]
    v_add_u32 v2, vcc, 8, v2
    v_addc_u32 v3, vcc, v3, 0, vcc
    // atomic result, vector store value, LDS result, work-item index
    flat_store_dwordx4 v[2:3], v[11:14]
    v_add_u32 v2, vcc, 16, v2
    v_addc_u32 v3, vcc, v3, 0, vcc
    s_waitcnt vmcnt(0) lgkmcnt(0)
    v_mov_b32 v4, s5
    v_mov_b32 v5, v0
    v_mov_b32 v6, s2
    v_mov_b32 v7, v15
    flat_store_dwordx4 v[2:3], v[4:7]
    v_add_u32 v2, vcc, 16, v2
    v_addc_u32 v3, vcc, v3, 0, vcc
    s_waitcnt vmcnt(0) lgkmcnt(0)
    v_mov_b32 v4, s14
    flat_store_dword v[2:3], v4
    s_waitcnt vmcnt(0) lgkmcnt(0)
    s_endpgm
