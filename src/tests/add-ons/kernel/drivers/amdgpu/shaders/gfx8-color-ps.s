// Copyright 2026, air/OS. Distributed under the terms of the MIT License.
// llvm-mc -triple=amdgcn-amd-amdhsa -mcpu=gfx803 -filetype=obj
.text
v_mov_b32 v0, s0
v_mov_b32 v1, s1
v_mov_b32 v2, s2
v_mov_b32 v3, s3
exp mrt0 v0, v1, v2, v3 done vm
s_endpgm
