/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_MACH_O_DYLD_H
#define SLANG_STUB_MACH_O_DYLD_H
#include <stdint.h>
struct mach_header;
int _NSGetExecutablePath(char *, uint32_t *);
uint32_t _dyld_image_count(void);
const struct mach_header *_dyld_get_image_header(uint32_t);
intptr_t _dyld_get_image_vmaddr_slide(uint32_t);
const char *_dyld_get_image_name(uint32_t);
#endif
