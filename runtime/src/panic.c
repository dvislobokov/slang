/* panic.c - fatal errors: message + best-effort backtrace, exit code 2. */
#include "rt_internal.h"

#include <execinfo.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_panicking;

static SLANG_NORETURN void die(const char *msg, int64_t len) {
    /* A second panic (e.g. inside a destructor run during the first) aborts quietly. */
    if (__atomic_exchange_n(&g_panicking, 1, __ATOMIC_SEQ_CST)) {
        for (;;) pause();  /* another thread is printing the first panic and will _exit */
    }
    slang__io_flush_all();
    fputs("panic: ", stderr);
    fwrite(msg, 1, (size_t)len, stderr);
    fputc('\n', stderr);
    if (!getenv("SLANG_NO_BACKTRACE")) {
        void *frames[64];
        int n = backtrace(frames, 64);
        fputs("\nbacktrace (most recent call first):\n", stderr);
        fflush(stderr);
        /* skip die() and the slang_panic* entry point */
        if (n > 2) backtrace_symbols_fd(frames + 2, n - 2, STDERR_FILENO);
    }
    fflush(stderr);
    _exit(2);
}

void slang_panic(const char *msg) {
    if (!msg) msg = "";
    die(msg, (int64_t)strlen(msg));
}

void slang_panic_str(const slang_string *msg) {
    die(slang_string_data(msg), slang_string_len(msg));
}

void slang_panic_error(const slang_error *err) {
    slang_string *m = slang_error_message(err);
    die(slang_string_data(m), slang_string_len(m));
}

void slang__panicf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    die(buf, n);
}

void slang_panic_nil(void) {
    slang__panicf("runtime error: invalid memory address or nil pointer dereference");
}

void slang_panic_bounds(int64_t idx, int64_t len) {
    slang__panicf("runtime error: index out of range [%" PRId64 "] with length %" PRId64,
                  idx, len);
}

void slang_panic_slice_bounds(int64_t lo, int64_t hi, int64_t cap) {
    slang__panicf("runtime error: slice bounds out of range [%" PRId64 ":%" PRId64
                  "] with capacity %" PRId64, lo, hi, cap);
}
