/* Copyright 2026, air/OS. Distributed under the terms of the MIT License.
 * Assemble with llvm-mc -triple=amdgcn -mcpu=gfx803 -filetype=obj.
 * AMD GCN3 ISA rev 1.1: STATUS.PRIV bit 5, table 5.7, section 3.10.
 * s[0:1]: owned output address, s2: seed, s3: workgroup, v0: lane.
 * 64-byte records, 15 written DWORDs plus an untouched guard per lane.
 * Only wave-local registers and owned GPU memory are accessed.
 */
.text
.p2align 8
shader_privilege:
    v_mad_u32_u24 v1, s3, 64, v0
    v_xor_b32 v4, s2, v1
    v_lshlrev_b32 v2, 6, v1
    v_add_u32 v2, vcc, s0, v2
    v_mov_b32 v3, s1
    v_addc_u32 v3, vcc, v3, 0, vcc

    s_getreg_b32 s4, hwreg(HW_REG_STATUS, 5, 1)
    v_mov_b32 v5, s4
    s_mov_b32 s5, 1
    s_setreg_b32 hwreg(HW_REG_STATUS, 5, 1), s5
    s_nop 4
    s_getreg_b32 s4, hwreg(HW_REG_STATUS, 5, 1)
    v_mov_b32 v6, s4
    s_setreg_imm32_b32 hwreg(HW_REG_STATUS, 5, 1), 1
    s_nop 4
    s_getreg_b32 s4, hwreg(HW_REG_STATUS, 5, 1)
    v_mov_b32 v7, s4
    // If the attempted escalation worked, restore the original zero before
    // touching trap registers. The recorded nonzero still fails the test.
    s_setreg_imm32_b32 hwreg(HW_REG_STATUS, 5, 1), 0
    s_nop 4

    // Both SETREG forms must actually execute: change an ordinary wave-local
    // rounding field, read it back, and restore it. No FP operations here.
    s_getreg_b32 s4, hwreg(HW_REG_MODE, 0, 2)
    v_mov_b32 v8, s4
    s_mov_b32 s5, 1
    s_setreg_b32 hwreg(HW_REG_MODE, 0, 2), s5
    s_nop 4
    s_getreg_b32 s5, hwreg(HW_REG_MODE, 0, 2)
    v_mov_b32 v9, s5
    s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 2), 3
    s_nop 4
    s_getreg_b32 s5, hwreg(HW_REG_MODE, 0, 2)
    v_mov_b32 v10, s5
    s_setreg_b32 hwreg(HW_REG_MODE, 0, 2), s4
    s_nop 4
    s_getreg_b32 s5, hwreg(HW_REG_MODE, 0, 2)
    v_mov_b32 v11, s5

    // Attempt to change all 16 trap SGPRs, restoring the original values even
    // if writes unexpectedly succeed. Report a bit per changed register.
    s_mov_b32 s8, 0
.macro trap_register reg, bit
    s_mov_b32 s6, \reg
    s_xor_b32 s7, s6, 0x55aa55aa
    s_mov_b32 \reg, s7
    s_nop 4
    s_mov_b32 s7, \reg
    s_cmp_lg_u32 s6, s7
    s_cselect_b32 s7, \bit, 0
    s_or_b32 s8, s8, s7
    s_mov_b32 \reg, s6
    s_nop 4
.endm
    trap_register tba_lo, 1
    trap_register tba_hi, 2
    trap_register tma_lo, 4
    trap_register tma_hi, 8
    trap_register ttmp0, 16
    trap_register ttmp1, 32
    trap_register ttmp2, 64
    trap_register ttmp3, 128
    trap_register ttmp4, 256
    trap_register ttmp5, 512
    trap_register ttmp6, 1024
    trap_register ttmp7, 2048
    trap_register ttmp8, 4096
    trap_register ttmp9, 8192
    trap_register ttmp10, 16384
    trap_register ttmp11, 32768
    v_mov_b32 v12, s8

    // Positive control for special SGPR MOV writes, followed by restoration.
    s_mov_b32 s4, m0
    s_mov_b32 m0, 0x13579bdf
    s_nop 4
    s_mov_b32 s5, m0
    v_mov_b32 v13, s5
    s_mov_b32 m0, s4
    s_nop 4
    s_mov_b32 s5, m0
    s_xor_b32 s5, s5, s4
    v_mov_b32 v14, s5
    v_mov_b32 v15, s3
    v_mov_b32 v16, v0
    v_mov_b32 v17, s2
    v_mov_b32 v18, 0x51005100

    flat_store_dwordx4 v[2:3], v[4:7]
    v_add_u32 v2, vcc, 16, v2
    v_addc_u32 v3, vcc, v3, 0, vcc
    flat_store_dwordx4 v[2:3], v[8:11]
    v_add_u32 v2, vcc, 16, v2
    v_addc_u32 v3, vcc, v3, 0, vcc
    flat_store_dwordx4 v[2:3], v[12:15]
    v_add_u32 v2, vcc, 16, v2
    v_addc_u32 v3, vcc, v3, 0, vcc
    flat_store_dwordx3 v[2:3], v[16:18]
    s_waitcnt vmcnt(0)
    s_endpgm
