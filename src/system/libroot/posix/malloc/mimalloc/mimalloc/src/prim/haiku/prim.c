/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Primitives for mimalloc as the allocator of Haiku's libroot. (A mimalloc
// that an application brings along uses unix/prim.c.)

#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/prim.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <OS.h>

// libroot calls the process and thread hooks itself (see the glue code)
#define MI_PRIM_HAS_PROCESS_ATTACH 1


//---------------------------------------------
// init
//---------------------------------------------

// How memory is accounted for:
//
// - "overcommit" (the default): everything is mapped readable and writable
//   with MAP_NORESERVE. The kernel then commits each page when it is first
//   touched, and releases the commitment when the page is given back
//   (MADV_FREE). Committing costs no system call, and the areas keep a single
//   protection, which fork() copies fast. When memory runs out, the touch
//   fails (SIGSEGV) where malloc() would have returned NULL.
//
// - "strict" (MIMALLOC_HAIKU_OVERCOMMIT=0 in the environment, or built with
//   MI_HAIKU_OVERCOMMIT=0): ranges are reserved PROT_NONE and committed with
//   mprotect(), which fails cleanly when memory runs out. The areas then have
//   a protection per page: every commit is a system call, and fork() has to
//   walk the pages.
#ifndef MI_HAIKU_OVERCOMMIT
#define MI_HAIKU_OVERCOMMIT 1
#endif

static bool mi_haiku_overcommit = (MI_HAIKU_OVERCOMMIT != 0);
static size_t mi_haiku_memory_limit = SIZE_MAX;   // RAM and swap space

void _mi_prim_mem_init( mi_os_mem_config_t* config )
{
  system_info info;
  config->page_size = B_PAGE_SIZE;
  config->alloc_granularity = B_PAGE_SIZE;
  config->large_page_size = 0;
  if (get_system_info(&info) == B_OK) {
    config->physical_memory_in_kib = (size_t)info.max_pages * (B_PAGE_SIZE / MI_KiB);
    mi_haiku_memory_limit = ((size_t)info.max_pages + (size_t)info.max_swap_pages) * B_PAGE_SIZE;
  }
  char value[64];
  if (_mi_prim_getenv("MIMALLOC_HAIKU_OVERCOMMIT", value, sizeof(value))) {
    mi_haiku_overcommit = (value[0] != '0');
  }
  config->has_overcommit = mi_haiku_overcommit;
  config->has_partial_free = true;     // munmap() cuts areas
  config->has_virtual_reserve = true;
  config->has_transparent_huge_pages = false;
}


//---------------------------------------------
// free
//---------------------------------------------

int _mi_prim_free(void* addr, size_t size ) {
  if (size==0) return 0;
  return (munmap(addr, size) == 0 ? 0 : errno);
}


//---------------------------------------------
// mmap
//---------------------------------------------

int _mi_prim_alloc(void* hint_addr, size_t size, size_t try_alignment, bool commit, bool allow_large, bool* is_large, bool* is_zero, void** addr) {
  MI_UNUSED(allow_large);
  int protection = (commit ? (PROT_READ | PROT_WRITE) : PROT_NONE);
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if (mi_haiku_overcommit) {
    // Nothing is committed here, so any size would succeed. Refuse what can
    // never be backed, so that malloc() of an absurd size still returns NULL.
    // (Arenas are reserved in chunks that can be larger than what is free
    // right now; that is fine, only touched pages count.)
    if (size > mi_haiku_memory_limit && commit) {
      *addr = NULL;
      return ENOMEM;
    }
    protection = PROT_READ | PROT_WRITE;
    flags |= MAP_NORESERVE;
  }
  void* p = MAP_FAILED;
  *is_large = false;
  *is_zero = true;
  if (hint_addr == NULL) {
    // an address hint is where the kernel starts to look, so an aligned hint
    // usually gives an aligned range
    hint_addr = _mi_os_get_aligned_hint(try_alignment, size);
  }
  if (hint_addr != NULL) {
    p = mmap(hint_addr, size, protection, flags, -1, 0);
  }
  if (p == MAP_FAILED) {
    p = mmap(NULL, size, protection, flags, -1, 0);
  }
  if (p == MAP_FAILED) {
    *addr = NULL;
    return errno;
  }
  *addr = p;
  return 0;
}


//---------------------------------------------
// commit, decommit and reset
//---------------------------------------------

int _mi_prim_commit(void* start, size_t size, bool* is_zero) {
  *is_zero = false;
  if (mi_haiku_overcommit) return 0;
  // fails with ENOMEM when the memory cannot be committed
  return (mprotect(start, size, PROT_READ | PROT_WRITE) == 0 ? 0 : errno);
}

int _mi_prim_reuse(void* start, size_t size) {
  MI_UNUSED(start); MI_UNUSED(size);
  return 0;
}

int _mi_prim_decommit(void* start, size_t size, bool* needs_recommit) {
  int err = 0;
  if (mi_haiku_overcommit) {
    // giving the pages back releases their commitment
    *needs_recommit = false;
    return (madvise(start, size, MADV_FREE) == 0 ? 0 : errno);
  }
  // Give the pages back first: making a range inaccessible only releases the
  // commitment of the pages that are not there.
  if (madvise(start, size, MADV_FREE) != 0) err = errno;
  if (mprotect(start, size, PROT_NONE) != 0) err = errno;
  *needs_recommit = true;
  return err;
}

int _mi_prim_reset(void* start, size_t size) {
  // MADV_FREE discards the pages at once; they read as zeroes afterwards and
  // stay committed.
  return (madvise(start, size, MADV_FREE) == 0 ? 0 : errno);
}

int _mi_prim_protect(void* start, size_t size, bool protect) {
  return (mprotect(start, size, protect ? PROT_NONE : (PROT_READ | PROT_WRITE)) == 0 ? 0 : errno);
}


//---------------------------------------------
// huge pages and NUMA: none
//---------------------------------------------

int _mi_prim_alloc_huge_os_pages(void* hint_addr, size_t size, int numa_node, bool* is_zero, void** addr) {
  MI_UNUSED(hint_addr); MI_UNUSED(size); MI_UNUSED(numa_node);
  *is_zero = false;
  *addr = NULL;
  return ENOMEM;
}

size_t _mi_prim_numa_node(void) {
  return 0;
}

size_t _mi_prim_numa_node_count(void) {
  return 1;
}


//---------------------------------------------
// clock and process info
//---------------------------------------------

mi_msecs_t _mi_prim_clock_now(void) {
  return (mi_msecs_t)(system_time() / 1000);
}

void _mi_prim_process_info(mi_process_info_t* pinfo)
{
  team_usage_info usage;
  if (get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &usage) == B_OK) {
    pinfo->utime = (mi_msecs_t)(usage.user_time / 1000);
    pinfo->stime = (mi_msecs_t)(usage.kernel_time / 1000);
  }
  // sum up the areas of this team
  area_info area;
  ssize_t cookie = 0;
  size_t rss = 0;
  while (get_next_area_info(B_CURRENT_TEAM, &cookie, &area) == B_OK) {
    rss += area.ram_size;
  }
  pinfo->current_rss = rss;
  pinfo->peak_rss = rss;
}


//---------------------------------------------
// output, environment, random
//---------------------------------------------

void _mi_prim_out_stderr( const char* msg ) {
  // not stdio: it allocates
  const size_t len = _mi_strlen(msg);
  if (write(STDERR_FILENO, msg, len) < 0) { /* ignore */ }
}

extern char** environ;

bool _mi_prim_getenv(const char* name, char* result, size_t result_size) {
  if (name==NULL) return false;
  const size_t len = _mi_strlen(name);
  if (len == 0) return false;
  char** env = environ;
  if (env == NULL) return false;
  // compare up to 10000 entries
  for (int i = 0; i < 10000 && env[i] != NULL; i++) {
    const char* s = env[i];
    if (_mi_strnicmp(name, s, len) == 0 && s[len] == '=') { // case insensitive
      _mi_strlcpy(result, s + len + 1, result_size);
      return true;
    }
  }
  return false;
}

bool _mi_prim_random_buf(void* buf, size_t buf_len) {
  // No kernel random source without opening a device; mimalloc falls back to
  // its weak seed (address, time), as the previous allocator did.
  MI_UNUSED(buf); MI_UNUSED(buf_len);
  return false;
}


//---------------------------------------------
// thread init and done: libroot calls __heap_thread_exit()
//---------------------------------------------

void _mi_prim_thread_init_auto_done(void) {
}

void _mi_prim_thread_done_auto_done(void) {
}

void _mi_prim_thread_associate_default_theap(mi_theap_t* theap) {
  MI_UNUSED(theap);
}

bool _mi_prim_thread_is_in_threadpool(void) {
  return false;
}
