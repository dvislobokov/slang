/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_COPYFILE_H
#define ZELL_STUB_COPYFILE_H
#include <stdint.h>
typedef uint32_t copyfile_flags_t;
typedef struct _copyfile_state *copyfile_state_t;
#define COPYFILE_ACL (1 << 0)
#define COPYFILE_STAT (1 << 1)
#define COPYFILE_XATTR (1 << 2)
#define COPYFILE_DATA (1 << 3)
#define COPYFILE_ALL (COPYFILE_METADATA | COPYFILE_DATA)
#define COPYFILE_METADATA (COPYFILE_SECURITY | COPYFILE_XATTR)
#define COPYFILE_SECURITY (COPYFILE_STAT | COPYFILE_ACL)
#define COPYFILE_CLONE (1 << 24)
int copyfile(const char *, const char *, copyfile_state_t, copyfile_flags_t);
int fcopyfile(int, int, copyfile_state_t, copyfile_flags_t);
#endif
