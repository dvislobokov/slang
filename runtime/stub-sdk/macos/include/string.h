/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_STRING_H
#define SLANG_STUB_STRING_H
#include <stddef.h>
void *memcpy(void *restrict, const void *restrict, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);
void *memmem(const void *, size_t, const void *, size_t);
void *memrchr(const void *, int, size_t);
size_t strlen(const char *);
size_t strnlen(const char *, size_t);
char *strcpy(char *restrict, const char *restrict);
char *strncpy(char *restrict, const char *restrict, size_t);
char *stpcpy(char *restrict, const char *restrict);
size_t strlcpy(char *restrict, const char *restrict, size_t);
size_t strlcat(char *restrict, const char *restrict, size_t);
char *strcat(char *restrict, const char *restrict);
char *strncat(char *restrict, const char *restrict, size_t);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
int strcoll(const char *, const char *);
char *strchr(const char *, int);
char *strrchr(const char *, int);
char *strstr(const char *, const char *);
char *strcasestr(const char *, const char *);
char *strpbrk(const char *, const char *);
size_t strspn(const char *, const char *);
size_t strcspn(const char *, const char *);
char *strtok(char *restrict, const char *restrict);
char *strtok_r(char *restrict, const char *restrict, char **restrict);
char *strsep(char **, const char *);
char *strdup(const char *);
char *strndup(const char *, size_t);
char *strerror(int);
int strerror_r(int, char *, size_t);
char *strsignal(int);
void explicit_bzero(void *, size_t);
int timingsafe_bcmp(const void *, const void *, size_t);
#include <strings.h>
#endif
