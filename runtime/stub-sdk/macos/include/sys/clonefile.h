/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_SYS_CLONEFILE_H
#define ZELL_STUB_SYS_CLONEFILE_H
#include <stdint.h>
#define CLONE_NOFOLLOW 0x0001
int clonefile(const char *, const char *, uint32_t);
#endif
