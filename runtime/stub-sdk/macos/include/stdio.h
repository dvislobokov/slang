/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_STDIO_H
#define SLANG_STUB_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>
typedef __int64_t fpos_t;
typedef struct __sFILE FILE;
extern FILE *__stdinp, *__stdoutp, *__stderrp;
#define stdin __stdinp
#define stdout __stdoutp
#define stderr __stderrp
#define EOF (-1)
#define BUFSIZ 1024
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define L_tmpnam 1024
#define FILENAME_MAX 1024
int printf(const char *restrict, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *restrict, const char *restrict, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *restrict, const char *restrict, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *restrict, size_t, const char *restrict, ...) __attribute__((format(printf, 3, 4)));
int dprintf(int, const char *restrict, ...) __attribute__((format(printf, 2, 3)));
int vprintf(const char *restrict, va_list);
int vfprintf(FILE *restrict, const char *restrict, va_list);
int vsnprintf(char *restrict, size_t, const char *restrict, va_list);
int vsprintf(char *restrict, const char *restrict, va_list);
int sscanf(const char *restrict, const char *restrict, ...);
int fscanf(FILE *restrict, const char *restrict, ...);
int fputs(const char *restrict, FILE *restrict);
int puts(const char *);
int fputc(int, FILE *);
int putc(int, FILE *);
int putchar(int);
int fgetc(FILE *);
int getc(FILE *);
int getchar(void);
int ungetc(int, FILE *);
char *fgets(char *restrict, int, FILE *restrict);
size_t fread(void *restrict, size_t, size_t, FILE *restrict);
size_t fwrite(const void *restrict, size_t, size_t, FILE *restrict);
FILE *fopen(const char *restrict, const char *restrict);
FILE *fdopen(int, const char *);
FILE *freopen(const char *restrict, const char *restrict, FILE *restrict);
FILE *popen(const char *, const char *);
int pclose(FILE *);
FILE *tmpfile(void);
FILE *fmemopen(void *restrict, size_t, const char *restrict);
FILE *open_memstream(char **, size_t *);
int fclose(FILE *);
int fflush(FILE *);
int fseek(FILE *, long, int);
int fseeko(FILE *, off_t, int);
long ftell(FILE *);
off_t ftello(FILE *);
void rewind(FILE *);
int feof(FILE *);
int ferror(FILE *);
void clearerr(FILE *);
int fileno(FILE *);
int setvbuf(FILE *restrict, char *restrict, int, size_t);
void setbuf(FILE *restrict, char *restrict);
int remove(const char *);
int rename(const char *, const char *);
void perror(const char *);
void flockfile(FILE *);
void funlockfile(FILE *);
int putc_unlocked(int, FILE *);
int getc_unlocked(FILE *);
ssize_t getline(char **restrict, size_t *restrict, FILE *restrict);
ssize_t getdelim(char **restrict, size_t *restrict, int, FILE *restrict);
#endif
