/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_MACH_O_NLIST_H
#define SLANG_STUB_MACH_O_NLIST_H
#include <stdint.h>
struct nlist_64 { union { uint32_t n_strx; } n_un; uint8_t n_type; uint8_t n_sect; uint16_t n_desc; uint64_t n_value; };
#define N_STAB 0xe0
#define N_TYPE 0x0e
#define N_SECT 0xe
#define N_EXT 0x01
#endif
