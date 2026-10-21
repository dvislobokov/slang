/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_MACH_MACH_TIME_H
#define SLANG_STUB_MACH_MACH_TIME_H
#include <stdint.h>
typedef struct mach_timebase_info { uint32_t numer; uint32_t denom; } *mach_timebase_info_t;
typedef struct mach_timebase_info mach_timebase_info_data_t;
typedef int kern_return_t;
kern_return_t mach_timebase_info(mach_timebase_info_t);
uint64_t mach_absolute_time(void);
uint64_t mach_continuous_time(void);
#endif
