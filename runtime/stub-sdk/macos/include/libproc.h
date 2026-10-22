/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_LIBPROC_H
#define ZELL_STUB_LIBPROC_H
#include <sys/types.h>
#define PROC_PIDPATHINFO_MAXSIZE 4096
int proc_pidpath(int, void *, uint32_t);
#endif
