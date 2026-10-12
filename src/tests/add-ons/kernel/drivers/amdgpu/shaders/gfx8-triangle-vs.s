// Copyright 2026, air/OS. Distributed under the terms of the MIT License.
// llvm-mc -triple=amdgcn-amd-amdhsa -mcpu=gfx803 -filetype=obj
.text
v_cmp_eq_u32 vcc, 1, v0
v_mov_b32 v1, 0
v_mov_b32 v2, 0x42020000
v_cndmask_b32 v1, v1, v2, vcc
v_cmp_eq_u32 vcc, 2, v0
v_mov_b32 v2, 0
v_mov_b32 v3, 0x42020000
v_cndmask_b32 v2, v2, v3, vcc
v_mov_b32 v3, 0
v_mov_b32 v4, 1.0
exp pos0 v1, v2, v3, v4 done
s_endpgm
