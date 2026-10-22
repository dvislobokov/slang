/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_WCHAR_H
#define ZELL_STUB_WCHAR_H
#include <stddef.h>
typedef int wint_t;
typedef struct { char __mbstate8[128]; long long _mbstateL; } mbstate_t;
size_t wcslen(const wchar_t *);
#endif
