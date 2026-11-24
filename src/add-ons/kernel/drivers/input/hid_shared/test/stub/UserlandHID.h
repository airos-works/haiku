#ifndef HID_USERLAND_H
#define HID_USERLAND_H
#include <SupportDefs.h>
// what the parser complains about is counted, not printed (the fuzzing case
// would print megabytes)
extern int gTraceCount;
#define TRACE(x...)
#define TRACE_ALWAYS(x...)	(gTraceCount++)
#endif
