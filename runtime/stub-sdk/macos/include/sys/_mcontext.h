/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_SYS__MCONTEXT_H
#define ZELL_STUB_SYS__MCONTEXT_H
#include <sys/_types.h>
#if defined(__aarch64__) || defined(__arm64__)
struct __darwin_arm_exception_state64 { __uint64_t __far; __uint32_t __esr; __uint32_t __exception; };
struct __darwin_arm_thread_state64 { __uint64_t __x[29]; __uint64_t __fp; __uint64_t __lr; __uint64_t __sp; __uint64_t __pc; __uint32_t __cpsr; __uint32_t __pad; };
struct __darwin_arm_neon_state64 { __uint128_t __v[32]; __uint32_t __fpsr; __uint32_t __fpcr; };
struct __darwin_mcontext64 { struct __darwin_arm_exception_state64 __es; struct __darwin_arm_thread_state64 __ss; struct __darwin_arm_neon_state64 __ns; };
#else
struct __darwin_x86_exception_state64 { __uint16_t __trapno; __uint16_t __cpu; __uint32_t __err; __uint64_t __faultvaddr; };
struct __darwin_x86_thread_state64 {
    __uint64_t __rax, __rbx, __rcx, __rdx, __rdi, __rsi, __rbp, __rsp, __r8, __r9, __r10, __r11, __r12, __r13, __r14, __r15, __rip, __rflags, __cs, __fs, __gs;
};
struct __darwin_x86_float_state64 { char __opaque[524]; };
struct __darwin_mcontext64 { struct __darwin_x86_exception_state64 __es; struct __darwin_x86_thread_state64 __ss; struct __darwin_x86_float_state64 __fs; };
#endif
typedef struct __darwin_mcontext64 *mcontext_t;
#endif
