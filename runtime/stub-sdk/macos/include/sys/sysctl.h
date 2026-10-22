/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_SYS_SYSCTL_H
#define ZELL_STUB_SYS_SYSCTL_H
#include <sys/types.h>
#define CTL_KERN 1
#define CTL_HW 6
#define KERN_PROC 14
#define KERN_OSRELEASE 2
#define HW_NCPU 3
#define HW_MEMSIZE 24
int sysctl(int *, unsigned int, void *, size_t *, void *, size_t);
int sysctlbyname(const char *, void *, size_t *, void *, size_t);
int sysctlnametomib(const char *, int *, size_t *);
#endif
