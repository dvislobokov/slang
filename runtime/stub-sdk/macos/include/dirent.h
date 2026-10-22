/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_DIRENT_H
#define ZELL_STUB_DIRENT_H
#include <sys/types.h>
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14
struct dirent { __uint64_t d_ino; __uint64_t d_seekoff; __uint16_t d_reclen; __uint16_t d_namlen; __uint8_t d_type; char d_name[1024]; };
typedef struct __dirdesc DIR;
/* x86_64: the 64-bit-inode struct dirent goes with the $INODE64 entry
 * points (the SDK's __DARWIN_INODE64 and __DARWIN_ALIAS_I). */
#if defined(__x86_64__)
#define ZELL_STUB_DIR_INODE64(sym) __asm("_" #sym "$INODE64")
#else
#define ZELL_STUB_DIR_INODE64(sym)
#endif
DIR *opendir(const char *) ZELL_STUB_DIR_INODE64(opendir);
DIR *fdopendir(int) ZELL_STUB_DIR_INODE64(fdopendir);
struct dirent *readdir(DIR *) ZELL_STUB_DIR_INODE64(readdir);
int readdir_r(DIR *, struct dirent *, struct dirent **) ZELL_STUB_DIR_INODE64(readdir_r);
int closedir(DIR *);
void rewinddir(DIR *) ZELL_STUB_DIR_INODE64(rewinddir);
int dirfd(DIR *);
#endif
