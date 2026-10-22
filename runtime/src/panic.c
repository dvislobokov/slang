/* panic.c - fatal errors: message + best-effort backtrace, exit code 2. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_panicking;

static ZELL_NORETURN void die(const char *msg, int64_t len) {
    /* testing.Panics: a capture scope on this thread takes the panic. */
    zell__panic_capture *cap = zell__panic_capture_tls;
    if (cap) {
        zell__panic_capture_tls = NULL;
        cap->msg = malloc((size_t)len + 1);
        if (cap->msg) {
            memcpy(cap->msg, msg, (size_t)len);
            cap->msg[len] = 0;
            cap->len = len;
        }
        longjmp(cap->jb, 1);
    }
    /* The debug agent stops here first (exception breakpoint "panic"). */
    if (__atomic_load_n(&zell_dbg_on, __ATOMIC_RELAXED) && zell__dbg_panic_fn) zell__dbg_panic_fn(msg, len);
    /* A second panic (e.g. inside a destructor run during the first) aborts quietly. */
    if (__atomic_exchange_n(&g_panicking, 1, __ATOMIC_SEQ_CST)) {
        for (;;) zell__plat_sleep_ns((int64_t)1000000000);  /* another thread is printing the first panic and will _exit */
    }
    zell__io_flush_all();
    fputs("panic: ", stderr);
    fwrite(msg, 1, (size_t)len, stderr);
    fputc('\n', stderr);
    if (!getenv("ZELL_NO_BACKTRACE")) {
        fputs("\nbacktrace (most recent call first):\n", stderr);
        fflush(stderr);
        /* skip zell__plat_backtrace, die() and the zell_panic* entry point */
        zell__plat_backtrace(2, 3);
    }
    fflush(stderr);
    /* An attached debugger learns that the program ends (DAP `exited` +
     * `terminated`), also after `continue` from a panic stop. */
    if (__atomic_load_n(&zell_dbg_on, __ATOMIC_RELAXED)) zell_dbg_exit(2);
    _Exit(2);
}

void zell_panic(const char *msg) {
    if (!msg) msg = "";
    die(msg, (int64_t)strlen(msg));
}

void zell_panic_str(const zell_string *msg) {
    die(zell_string_data(msg), zell_string_len(msg));
}

void zell_panic_error(const zell_error *err) {
    zell_string *m = zell_error_message(err);
    die(zell_string_data(m), zell_string_len(m));
}

void zell__panicf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    die(buf, n);
}

void zell_panic_nil(void) {
    zell__panicf("runtime error: invalid memory address or nil pointer dereference");
}

void zell_panic_bounds(int64_t idx, int64_t len) {
    zell__panicf("runtime error: index out of range [%" PRId64 "] with length %" PRId64,
                  idx, len);
}

void zell_panic_slice_bounds(int64_t lo, int64_t hi, int64_t cap) {
    zell__panicf("runtime error: slice bounds out of range [%" PRId64 ":%" PRId64
                  "] with capacity %" PRId64, lo, hi, cap);
}
