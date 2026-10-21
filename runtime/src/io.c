/* io.c - console output, files, stdin. Output goes through
 * slang__plat_stream_write (WriteConsoleW on a Windows console). */
#include "rt_internal.h"
#include "platform/platform.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void slang__io_flush_all(void) {
    fflush(stdout);
    fflush(stderr);
}

void slang_flush(void) { fflush(stdout); }

static void out(const char *p, int64_t n) {
    if (n > 0) slang__plat_stream_write(stdout, p, (size_t)n);
}

void slang_print_str(const slang_string *s) { out(slang_string_data(s), slang_string_len(s)); }
void slang_print_cstr(const char *s) { if (s) out(s, (int64_t)strlen(s)); }
void slang_print_int(int64_t v) {
    char buf[24];
    out(buf, snprintf(buf, sizeof buf, "%" PRId64, v));
}
void slang_print_uint(uint64_t v) {
    char buf[24];
    out(buf, snprintf(buf, sizeof buf, "%" PRIu64, v));
}
void slang_print_float(double v) {
    char buf[64];
    int n = slang__format_float(buf, sizeof buf, v);
    out(buf, n);
}
void slang_print_bool(bool v) { out(v ? "true" : "false", v ? 4 : 5); }
void slang_print_rune(int32_t r) {
    char buf[4];
    out(buf, slang__utf8_encode(buf, r));
}
void slang_print_space(void) { out(" ", 1); }
void slang_print_newline(void) { out("\n", 1); }

/* println_X: value + '\n' written under one stdio lock so lines don't interleave. */
#define LINE(stmt) do { slang__plat_flockfile(stdout); stmt; out("\n", 1); slang__plat_funlockfile(stdout); } while (0)
void slang_println_str(const slang_string *s) { LINE(slang_print_str(s)); }
void slang_println_cstr(const char *s) { LINE(slang_print_cstr(s)); }
void slang_println_int(int64_t v) { LINE(slang_print_int(v)); }
void slang_println_uint(uint64_t v) { LINE(slang_print_uint(v)); }
void slang_println_float(double v) { LINE(slang_print_float(v)); }
void slang_println_bool(bool v) { LINE(slang_print_bool(v)); }
void slang_println_rune(int32_t r) { LINE(slang_print_rune(r)); }

void slang_eprint_str(const slang_string *s) {
    int64_t n = slang_string_len(s);
    if (n) slang__plat_stream_write(stderr, slang_string_data(s), (size_t)n);
}
void slang_eprintln_str(const slang_string *s) {
    slang__plat_flockfile(stderr);
    slang_eprint_str(s);
    slang__plat_stream_write(stderr, "\n", 1);
    slang__plat_funlockfile(stderr);
}

static slang_error *errno_error(const char *op, const slang_string *path, int e) {
    return slang_error_errno(op, slang_string_cstr(path), e);
}

slang_string *slang_read_file(const slang_string *path, slang_error **err) {
    if (err) *err = NULL;
    int64_t fd = slang__plat_open(slang_string_cstr(path), SLANG_PLAT_O_RDONLY, 0);
    if (fd < 0) { if (err) *err = errno_error("open", path, (int)-fd); return NULL; }
    int64_t cap = 4096, len = 0;
    char *buf = slang_alloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = slang_realloc(buf, cap); }
        int64_t n = slang__plat_read(fd, buf + len, cap - len);
        if (n < 0) {
            slang__plat_close(fd);
            slang_free(buf);
            if (err) *err = errno_error("read", path, (int)-n);
            return NULL;
        }
        if (n == 0) break;
        len += n;
    }
    slang__plat_close(fd);
    slang_string *s = slang_string_new(buf, len);
    slang_free(buf);
    return s;
}

static slang_error *write_bytes(const slang_string *path, const void *data, int64_t n) {
    int64_t fd = slang__plat_open(slang_string_cstr(path), SLANG_PLAT_O_WRONLY | SLANG_PLAT_O_CREATE | SLANG_PLAT_O_TRUNC, 0666);
    if (fd < 0) return errno_error("open", path, (int)-fd);
    int e = slang__plat_write_all(fd, data, n);
    if (e) {
        slang__plat_close(fd);
        return errno_error("write", path, e);
    }
    e = slang__plat_close(fd);
    if (e) return errno_error("close", path, e);
    return NULL;
}

slang_error *slang_write_file(const slang_string *path, const slang_string *data) {
    return write_bytes(path, slang_string_data(data), slang_string_len(data));
}

slang_error *slang_write_file_bytes(const slang_string *path, const slang_slice *data) {
    return write_bytes(path, data->data, data->len);
}

slang_string *slang_read_line(slang_error **err) {
    if (err) *err = NULL;
    fflush(stdout);  /* make prompts visible */
    int64_t cap = 128, len = 0;
    char *buf = slang_alloc(cap);
    int c;
    bool any = false;
    while ((c = getchar()) != EOF) {
        any = true;
        if (c == '\n') break;
        if (len + 1 >= cap) { cap *= 2; buf = slang_realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    if (!any) {
        slang_free(buf);
        if (err) *err = ferror(stdin) ? slang_error_new_cstr("read stdin: I/O error")
                                      : slang_err_eof;
        return NULL;
    }
    if (len > 0 && buf[len - 1] == '\r') len--;
    slang_string *s = slang_string_new(buf, len);
    slang_free(buf);
    return s;
}
