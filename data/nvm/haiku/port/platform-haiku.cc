// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Platform-specific code for Haiku goes here. For the POSIX-compatible
// parts, the implementation is in platform-posix.cc.
//
// One file for the V8 versions of Node.js 10 to 26 (V8 6.8 to 14.6); the
// few entry points whose names or signatures changed are picked by version.

#include <OS.h>
#include <image.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>

#undef MAP_TYPE

#include "include/v8-version.h"
#include "src/base/macros.h"
#include "src/base/platform/platform-posix-time.h"
#include "src/base/platform/platform-posix.h"
#include "src/base/platform/platform.h"

#define HAIKU_V8_AT_LEAST(major, minor) \
  (V8_MAJOR_VERSION > (major) ||            \
   (V8_MAJOR_VERSION == (major) && V8_MINOR_VERSION >= (minor)))

#if HAIKU_V8_AT_LEAST(12, 9)
#include <optional>
#endif

namespace v8 {
namespace base {

TimezoneCache* OS::CreateTimezoneCache() {
  return new PosixDefaultTimezoneCache();
}

std::vector<OS::SharedLibraryAddress> OS::GetSharedLibraryAddresses() {
  std::vector<SharedLibraryAddress> result;
  image_info info;
  int32 cookie = 0;
  while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
    uintptr_t start = reinterpret_cast<uintptr_t>(info.text);
    result.push_back(
        SharedLibraryAddress(info.name, start, start + info.text_size));
  }
  return result;
}

void OS::SignalCodeMovingGC() {}

#if HAIKU_V8_AT_LEAST(7, 8)
void OS::AdjustSchedulingParams() {}
#endif

#if HAIKU_V8_AT_LEAST(12, 9)
std::optional<OS::MemoryRange> OS::GetFirstFreeMemoryRangeWithin(
    OS::Address boundary_start, OS::Address boundary_end, size_t minimum_size,
    size_t alignment) {
  return std::nullopt;
}
#elif V8_MAJOR_VERSION >= 10
std::vector<OS::MemoryRange> OS::GetFreeMemoryRangesWithin(
    OS::Address boundary_start, OS::Address boundary_end, size_t minimum_size,
    size_t alignment) {
  return {};
}
#endif

// The stack grows down: its start is the top, stack_end in Haiku's terms.
#if HAIKU_V8_AT_LEAST(8, 4)
#if V8_MAJOR_VERSION >= 11
Stack::StackSlot Stack::ObtainCurrentThreadStackStart() {
#elif V8_MAJOR_VERSION >= 9
Stack::StackSlot Stack::GetStackStart() {
#else
void* Stack::GetStackStart() {
#endif
  thread_info info;
  if (get_thread_info(find_thread(nullptr), &info) != B_OK) return nullptr;
  return info.stack_end;
}
#endif

}  // namespace base
}  // namespace v8
