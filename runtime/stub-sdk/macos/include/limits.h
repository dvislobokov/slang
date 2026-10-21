/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#pragma once
#include_next <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define NAME_MAX 255
#define IOV_MAX 1024
#define HOST_NAME_MAX 255
#define SSIZE_MAX LONG_MAX
#define NGROUPS_MAX 16
