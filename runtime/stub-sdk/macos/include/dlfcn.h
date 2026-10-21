/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_DLFCN_H
#define SLANG_STUB_DLFCN_H
#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_LOCAL 0x4
#define RTLD_GLOBAL 0x8
#define RTLD_NOLOAD 0x10
#define RTLD_NODELETE 0x80
#define RTLD_DEFAULT ((void *)-2)
#define RTLD_NEXT ((void *)-1)
typedef struct dl_info { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info;
void *dlopen(const char *, int);
void *dlsym(void *restrict, const char *restrict);
int dlclose(void *);
char *dlerror(void);
int dladdr(const void *, Dl_info *);
#endif
