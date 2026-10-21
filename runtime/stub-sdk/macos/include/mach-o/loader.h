/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_MACH_O_LOADER_H
#define SLANG_STUB_MACH_O_LOADER_H
#include <stdint.h>
struct mach_header_64 { uint32_t magic; int32_t cputype; int32_t cpusubtype; uint32_t filetype; uint32_t ncmds; uint32_t sizeofcmds; uint32_t flags; uint32_t reserved; };
struct load_command { uint32_t cmd; uint32_t cmdsize; };
struct segment_command_64 { uint32_t cmd; uint32_t cmdsize; char segname[16]; uint64_t vmaddr; uint64_t vmsize; uint64_t fileoff; uint64_t filesize; int32_t maxprot; int32_t initprot; uint32_t nsects; uint32_t flags; };
struct symtab_command { uint32_t cmd; uint32_t cmdsize; uint32_t symoff; uint32_t nsyms; uint32_t stroff; uint32_t strsize; };
#define MH_MAGIC_64 0xfeedfacf
#define LC_SEGMENT_64 0x19
#define LC_SYMTAB 0x2
#endif
