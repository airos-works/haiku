/* Copyright 2026, air/OS. Distributed under the terms of the MIT License.
 * LLVM gfx803. One 64-lane wave. s0:1=output, s2=pattern seed,
 * s3=operation (0 read, 1 write/read, 2 atomic add), s4=byte offset,
 * s5=M0, s6=marker. All attempted GDS targets lie within the seeded 4 KiB.
 */
.text
.p2align 8
shader_gds:
    v_lshlrev_b32 v1, 5, v0
    v_add_u32 v2, vcc, s0, v1
    v_mov_b32 v3, s1
    v_addc_u32 v3, vcc, v3, 0, vcc
    v_xor_b32 v4, s6, v0
    flat_store_dword v[2:3], v4
    s_waitcnt vmcnt(0) lgkmcnt(0)
    s_getreg_b32 s8, hwreg(HW_REG_TRAPSTS, 0, 9)
    v_mov_b32 v5, s8
    v_lshlrev_b32 v1, 2, v0
    v_add_u32 v1, vcc, s4, v1
    s_mov_b32 m0, s5
    s_nop 1
    s_cmp_eq_u32 s3, 1
    s_cbranch_scc1 write
    s_cmp_eq_u32 s3, 2
    s_cbranch_scc1 atomic
    ds_read_b32 v4, v1 gds
    s_branch finish
write:
    v_lshrrev_b32 v4, 2, v1
    s_mov_b32 s9, 0x01020408
    v_mul_lo_u32 v4, v4, s9
    v_xor_b32 v4, s2, v4
    ds_write_b32 v1, v4 gds
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    ds_read_b32 v4, v1 gds
    s_branch finish
atomic:
    v_mov_b32 v4, 0x101
    ds_add_rtn_u32 v4, v1, v4 gds
finish:
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    s_getreg_b32 s8, hwreg(HW_REG_TRAPSTS, 0, 9)
    v_mov_b32 v6, s8
    v_mov_b32 v7, 0x6d5c001
    v_add_u32 v2, vcc, 4, v2
    v_addc_u32 v3, vcc, v3, 0, vcc
    flat_store_dwordx4 v[2:3], v[4:7]
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    s_endpgm
