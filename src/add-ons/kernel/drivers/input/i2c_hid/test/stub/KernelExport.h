#ifndef _TEST_KERNEL_EXPORT_H
#define _TEST_KERNEL_EXPORT_H
#include <SupportDefs.h>
#include <stdio.h>
#include <string.h>
#define dprintf test_dprintf
typedef struct { int32 v; } spinlock;
typedef int cpu_status;
#define B_INITIALIZE_SPINLOCK(s) ((s)->v = 0)
#define B_ANY_KERNEL_ADDRESS 0
#define B_KERNEL_READ_AREA 1
#define B_KERNEL_WRITE_AREA 2
#define B_RELATIVE_TIMEOUT 8
#define B_CAN_INTERRUPT 1
#define B_DO_NOT_RESCHEDULE 2
enum { B_UNHANDLED_INTERRUPT = 0, B_HANDLED_INTERRUPT = 1, B_INVOKE_SCHEDULER = 2 };
typedef int32 (*interrupt_handler)(void*);
extern "C" {
int dprintf(const char* format, ...);
area_id map_physical_memory(const char*, phys_addr_t, size_t, uint32, uint32, void**);
status_t delete_area(area_id);
status_t install_io_interrupt_handler(int32, interrupt_handler, void*, uint32);
status_t remove_io_interrupt_handler(int32, interrupt_handler, void*);
sem_id create_sem(int32, const char*);
status_t delete_sem(sem_id);
status_t release_sem_etc(sem_id, int32, uint32);
status_t acquire_sem_etc(sem_id, int32, uint32, bigtime_t);
}
const char* test_strerror(status_t);
#define strerror test_strerror
#endif
