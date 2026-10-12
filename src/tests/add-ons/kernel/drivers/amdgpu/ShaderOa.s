/* Copyright 2026, air/OS. Distributed under the terms of the MIT License.
 * LLVM gfx803. Initial ordered-append prerequisite: no DS/GWS/OA instruction.
 * One64-lane wave, s0:1=owned output, s6=marker. Records all guards and
 * TRAPSTS around a marker-only operation so the trusted reset prefix can be
 * qualified independently before an ordered append is attempted.
 */
.text
.p2align 8
shader_oa:
    v_lshlrev_b32 v1, 5, v0
    v_add_u32 v2, vcc, s0, v1
    v_mov_b32 v3, s1
    v_addc_u32 v3, vcc, v3, 0, vcc
    v_xor_b32 v4, s6, v0
    flat_store_dword v[2:3], v4
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    s_getreg_b32 s8, hwreg(HW_REG_TRAPSTS, 0, 9)
    v_mov_b32 v5, s8
    v_mov_b32 v4, 0xa0c0001
    s_getreg_b32 s8, hwreg(HW_REG_TRAPSTS, 0, 9)
    v_mov_b32 v6, s8
    v_mov_b32 v7, 0xa0c0ffe
    v_add_u32 v2, vcc, 4, v2
    v_addc_u32 v3, vcc, v3, 0, vcc
    flat_store_dwordx4 v[2:3], v[4:7]
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    s_endpgm
