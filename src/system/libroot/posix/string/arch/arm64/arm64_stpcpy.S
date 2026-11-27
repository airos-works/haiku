/*
 * stpcpy - copy a string returning pointer to end.
 *
 * Copyright (c) 2020, Arm Limited.
 * SPDX-License-Identifier: MIT OR Apache-2.0 WITH LLVM-exception
 */

// Imported from ARM-software/optimized-routines, revision
// 503fafe311c177de0e571c458c7c337b1ca5f522, string/aarch64/stpcpy.S.
// Builds stpcpy() from arm64_strcpy.S, as upstream does.
// The instruction sequence is unchanged. ARM64 user pages are at least 4 KiB
// and Advanced SIMD is enabled for user threads, including the runtime loader.
// Kernel and boot strings retain their separate implementations.

#define BUILD_STPCPY 1

#include "arm64_strcpy.S"
