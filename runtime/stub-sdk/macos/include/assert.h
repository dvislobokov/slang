/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_ASSERT_H
#define SLANG_STUB_ASSERT_H
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
void __assert_rtn(const char *, const char *, int, const char *) __attribute__((noreturn));
#define assert(e) ((e) ? (void)0 : __assert_rtn(__func__, __FILE__, __LINE__, #e))
#endif
#define static_assert _Static_assert
#endif
