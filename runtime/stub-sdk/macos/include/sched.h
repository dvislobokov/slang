/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_SCHED_H
#define ZELL_STUB_SCHED_H
#include <sys/types.h>
typedef __darwin_natural_t mach_port_t;
struct sched_param { int sched_priority; char __opaque[4]; };
int sched_yield(void);
int sched_get_priority_min(int);
int sched_get_priority_max(int);
#endif
