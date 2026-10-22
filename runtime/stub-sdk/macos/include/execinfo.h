/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_EXECINFO_H
#define ZELL_STUB_EXECINFO_H
#include <stddef.h>
int backtrace(void **, int);
char **backtrace_symbols(void *const *, int);
void backtrace_symbols_fd(void *const *, int, int);
#endif
