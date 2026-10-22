/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_MACH_MACH_H
#define ZELL_STUB_MACH_MACH_H
#include <mach/mach_time.h>
#include <sched.h>
typedef mach_port_t thread_act_t;
typedef mach_port_t task_t;
typedef mach_port_t thread_t;
extern mach_port_t mach_task_self_;
#define mach_task_self() mach_task_self_
mach_port_t mach_thread_self(void);
#define KERN_SUCCESS 0
#endif
