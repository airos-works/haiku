/* Copyright 2026, air/OS. Distributed under the terms of the MIT License.
 * LLVM gfx803, one wave. s0:1=output, s2=initial value, s3=operation
 * (0 INIT, 1 V, 2 P, 3 BR, 4 RELEASE_ALL, 5 BARRIER), s4=resource id << 16,
 * s5=marker, s6=attempt limit. P requires a positive counter; the one-wave
 * BARRIER requires counter zero. The host checks those before dispatch.
 * GFX8 needs the TRAPSTS.MEM_VIOL retry sequence used by LLVM's
 * emitGWSMemViolTestLoop, but this diagnostic bounds attempts explicitly.
 */
.text
.p2align 8
shader_gws:
    v_lshlrev_b32 v1, 5, v0
    v_add_u32 v2, vcc, s0, v1
    v_mov_b32 v3, s1
    v_addc_u32 v3, vcc, v3, 0, vcc
    v_xor_b32 v4, s5, v0
    flat_store_dword v[2:3], v4
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    s_getreg_b32 s8, hwreg(HW_REG_TRAPSTS, 0, 9)
    v_mov_b32 v5, s8
    v_mov_b32 v4, s2
    s_mov_b32 m0, s4
    s_mov_b32 s9, 0
attempt:
    s_add_u32 s9, s9, 1
    s_setreg_imm32_b32 hwreg(HW_REG_TRAPSTS, 8, 1), 0
    s_cmp_eq_u32 s3, 1
    s_cbranch_scc1 increment
    s_cmp_eq_u32 s3, 2
    s_cbranch_scc1 decrement
    s_cmp_eq_u32 s3, 3
    s_cbranch_scc1 bulk
    s_cmp_eq_u32 s3, 4
    s_cbranch_scc1 release
    s_cmp_eq_u32 s3, 5
    s_cbranch_scc1 barrier
    ds_gws_init v4 gds
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    s_branch check
increment:
    ds_gws_sema_v gds
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    s_branch check
decrement:
    ds_gws_sema_p gds
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    s_branch check
bulk:
    ds_gws_sema_br v4 gds
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    s_branch check
release:
    ds_gws_sema_release_all gds
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    s_branch check
barrier:
    ds_gws_barrier v4 gds
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
check:
    s_getreg_b32 s10, hwreg(HW_REG_TRAPSTS, 0, 9)
    s_cmp_eq_u32 s10, 0
    s_cbranch_scc1 finish
    s_cmp_lt_u32 s9, s6
    s_cbranch_scc1 attempt
finish:
    v_mov_b32 v4, s9
    v_mov_b32 v6, s10
    v_mov_b32 v7, 0x675c001
    v_add_u32 v2, vcc, 4, v2
    v_addc_u32 v3, vcc, v3, 0, vcc
    flat_store_dwordx4 v[2:3], v[4:7]
    s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
    s_endpgm
