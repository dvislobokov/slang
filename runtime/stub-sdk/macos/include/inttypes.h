/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_INTTYPES_H
#define SLANG_STUB_INTTYPES_H
#include <stdint.h>
#define PRId8 "hhd"
#define PRId16 "hd"
#define PRId32 "d"
#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIu8 "hhu"
#define PRIu16 "hu"
#define PRIu32 "u"
#define PRIu64 "llu"
#define PRIx8 "hhx"
#define PRIx16 "hx"
#define PRIx32 "x"
#define PRIx64 "llx"
#define PRIX32 "X"
#define PRIX64 "llX"
#define PRIo64 "llo"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIdMAX "jd"
#define PRIuMAX "ju"
#define SCNd64 "lld"
#define SCNu64 "llu"
#define SCNx64 "llx"
typedef struct { intmax_t quot, rem; } imaxdiv_t;
intmax_t strtoimax(const char *restrict, char **restrict, int);
uintmax_t strtoumax(const char *restrict, char **restrict, int);
#endif
