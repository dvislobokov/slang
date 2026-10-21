/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_FNMATCH_H
#define SLANG_STUB_FNMATCH_H
#define FNM_NOMATCH 1
#define FNM_NOESCAPE 0x01
#define FNM_PATHNAME 0x02
#define FNM_PERIOD 0x04
int fnmatch(const char *, const char *, int);
#endif
