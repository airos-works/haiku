/* Exercise Mesa's actual GENX(csf_cleanup_context) on a lost context: the
 * last job ended in an error, and the kernel refuses or has already done the
 * destroys (airos-works/haiku#209). The teardown must neither abort nor leak.
 * The function is cut out of pan_csf.c by test-cleanup-context.py. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define GENX(name) name
#define INT64_MAX_ INT64_MAX

struct drm_panthor_tiler_heap_destroy { uint32_t handle, pad; };
struct drm_panthor_group_destroy { uint32_t group_handle, pad; };
struct panfrost_bo { int refs; };
struct pipe_context { void *screen; };
struct panfrost_device { struct { void *dev; } kmod; };
struct panfrost_context {
   struct pipe_context base;
   uint32_t syncobj;
   struct {
      bool is_init;
      uint32_t group_handle;
      struct { uint32_t handle; struct panfrost_bo *desc_bo; } heap;
      struct panfrost_bo *tmp_geom_bo;
      struct { struct panfrost_bo *cs_bo, *save_bo; } tiler_oom_handler;
   } csf;
};

static struct panfrost_device device;
static struct panfrost_device *pan_device(void *screen) { (void)screen; return &device; }
static int panfrost_device_fd(struct panfrost_device *dev) { (void)dev; return 3; }

/* What the kernel answers, per call. */
static int wait_errors[4], wait_calls, heap_error, group_error;
static int heap_destroys, group_destroys, warnings;

static int pan_sync_wait(int fd, uint32_t *handles, unsigned count,
                         int64_t deadline, unsigned flags, uint32_t *first)
{
   (void)fd; (void)handles; (void)count; (void)deadline; (void)flags;
   (void)first;
   int error = wait_errors[wait_calls < 4 ? wait_calls : 3];
   wait_calls++;
   if (error) { errno = error; return -1; }
   return 0;
}
static int pan_csf_heap_destroy(void *dev, struct drm_panthor_tiler_heap_destroy *d)
{
   (void)dev; assert(d->handle == 7); heap_destroys++;
   if (heap_error) { errno = heap_error; return -1; }
   return 0;
}
static int pan_csf_group_destroy(void *dev, struct drm_panthor_group_destroy *d)
{
   (void)dev; assert(d->group_handle == 9); group_destroys++;
   if (group_error) { errno = group_error; return -1; }
   return 0;
}
static void panfrost_bo_unreference(struct panfrost_bo *bo)
{ assert(bo->refs > 0); bo->refs--; }
#define mesa_logw(...) (warnings++)
#define mesa_loge(...) (warnings++)

#include "cleanup-function.inc"

static int run(const char *name, int wait0, int wait1, int heap, int group,
               int expected_waits)
{
   struct panfrost_bo bos[4] = {{1}, {1}, {1}, {1}};
   struct panfrost_context ctx = {0};
   ctx.csf.is_init = true;
   ctx.csf.group_handle = 9;
   ctx.csf.heap.handle = 7;
   ctx.csf.heap.desc_bo = &bos[0];
   ctx.csf.tmp_geom_bo = &bos[1];
   ctx.csf.tiler_oom_handler.cs_bo = &bos[2];
   ctx.csf.tiler_oom_handler.save_bo = &bos[3];
   wait_errors[0] = wait0; wait_errors[1] = wait1;
   wait_errors[2] = wait_errors[3] = wait1;
   wait_calls = heap_destroys = group_destroys = warnings = 0;
   heap_error = heap; group_error = group;

   csf_cleanup_context(&ctx);

   bool ok = !ctx.csf.is_init && heap_destroys == 1 && group_destroys == 1
      && wait_calls == expected_waits;
   for (int i = 0; i < 4; i++)
      ok = ok && bos[i].refs == 0;
   printf("%s: %d waits, heap/group destroyed %d/%d, %d warnings: %s\n", name,
          wait_calls, heap_destroys, group_destroys, warnings,
          ok ? "ok" : "FAILED");
   return ok ? 0 : 1;
}

int main(void)
{
   int failures = 0;
   failures += run("healthy context", 0, 0, 0, 0, 1);
   failures += run("lost context (job ended in EIO, destroys refused)",
                   EIO, EIO, EIO, EIO, 1);
   failures += run("lost context, group already retired", EIO, EIO, 0, ENOENT, 1);
   failures += run("interrupted wait is repeated", EINTR, 0, 0, 0, 2);
   if (failures == 0)
      printf("MESA_HAIKU_CLEANUP_CONTEXT_TEST_PASS\n");
   return failures != 0;
}
