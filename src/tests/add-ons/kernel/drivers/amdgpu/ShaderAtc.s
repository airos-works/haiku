/* Copyright 2026, air/OS. Distributed under the terms of the MIT License.
 * LLVM gfx803. s[0:1]=owned output, s[2:3]=dual-owned source address,
 * s4=ATC descriptor bit (0 or 1<<24), s5=marker, v0=lane (one 64-lane wave).
 * No input writes. Every source address is owned on both GPUVM and physical
 * paths. Output always uses the previously qualified GPUVM flat path.
 */
.text
.p2align 8
shader_atc:
    v_lshlrev_b32 v1, 4, v0
    v_add_u32 v2, vcc, s0, v1
    v_mov_b32 v3, s1
    v_addc_u32 v3, vcc, v3, 0, vcc
    v_xor_b32 v4, s5, v0
    flat_store_dword v[2:3], v4
    s_waitcnt vmcnt(0) lgkmcnt(0)
    s_mov_b32 s8, s2
    s_mov_b32 s9, s3
    s_mov_b32 s10, 256
    s_or_b32 s11, s4, (4 | (5 << 3) | (6 << 6) | (7 << 9) | (4 << 12) | (4 << 15))
    v_lshlrev_b32 v1, 2, v0
    buffer_load_dword v4, v1, s[8:11], 0 offen glc
    s_waitcnt vmcnt(0)
    v_add_u32 v2, vcc, 4, v2
    v_addc_u32 v3, vcc, v3, 0, vcc
    v_mov_b32 v5, 0x51a7c001
    flat_store_dwordx2 v[2:3], v[4:5]
    s_waitcnt vmcnt(0) lgkmcnt(0)
    s_endpgm
