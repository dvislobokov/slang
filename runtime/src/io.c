/* io.c - console output, files, stdin. Output goes through
 * zell__plat_stream_write (WriteConsoleW on a Windows console). */
#include "rt_internal.h"
#include "platform/platform.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zell__io_flush_all(void) {
    fflush(stdout);
    fflush(stderr);
}

void zell_flush(void) { fflush(stdout); }

static void out(const char *p, int64_t n) {
    if (n > 0) zell__plat_stream_write(stdout, p, (size_t)n);
}

void zell_print_str(const zell_string *s) { out(zell_string_data(s), zell_string_len(s)); }
void zell_print_cstr(const char *s) { if (s) out(s, (int64_t)strlen(s)); }
void zell_print_int(int64_t v) {
    char buf[24];
    out(buf, snprintf(buf, sizeof buf, "%" PRId64, v));
}
void zell_print_uint(uint64_t v) {
    char buf[24];
    out(buf, snprintf(buf, sizeof buf, "%" PRIu64, v));
}
void zell_print_float(double v) {
    char buf[64];
    int n = zell__format_float(buf, sizeof buf, v);
    out(buf, n);
}
void zell_print_bool(bool v) { out(v ? "true" : "false", v ? 4 : 5); }
void zell_print_rune(int32_t r) {
    char buf[4];
    out(buf, zell__utf8_encode(buf, r));
}
void zell_print_space(void) { out(" ", 1); }
void zell_print_newline(void) { out("\n", 1); }

/* println_X: value + '\n' written under one stdio lock so lines don't interleave. */
#define LINE(stmt) do { zell__plat_flockfile(stdout); stmt; out("\n", 1); zell__plat_funlockfile(stdout); } while (0)
void zell_println_str(const zell_string *s) { LINE(zell_print_str(s)); }
void zell_println_cstr(const char *s) { LINE(zell_print_cstr(s)); }
void zell_println_int(int64_t v) { LINE(zell_print_int(v)); }
void zell_println_uint(uint64_t v) { LINE(zell_print_uint(v)); }
void zell_println_float(double v) { LINE(zell_print_float(v)); }
void zell_println_bool(bool v) { LINE(zell_print_bool(v)); }
void zell_println_rune(int32_t r) { LINE(zell_print_rune(r)); }

void zell_eprint_str(const zell_string *s) {
    int64_t n = zell_string_len(s);
    if (n) zell__plat_stream_write(stderr, zell_string_data(s), (size_t)n);
}
void zell_eprintln_str(const zell_string *s) {
    zell__plat_flockfile(stderr);
    zell_eprint_str(s);
    zell__plat_stream_write(stderr, "\n", 1);
    zell__plat_funlockfile(stderr);
}

static zell_error *errno_error(const char *op, const zell_string *path, int e) {
    return zell_error_errno(op, zell_string_cstr(path), e);
}

zell_string *zell_read_file(const zell_string *path, zell_error **err) {
    if (err) *err = NULL;
    int64_t fd = zell__plat_open(zell_string_cstr(path), ZELL_PLAT_O_RDONLY, 0);
    if (fd < 0) { if (err) *err = errno_error("open", path, (int)-fd); return NULL; }
    int64_t cap = 4096, len = 0;
    char *buf = zell_alloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = zell_realloc(buf, cap); }
        int64_t n = zell__plat_read(fd, buf + len, cap - len);
        if (n < 0) {
            zell__plat_close(fd);
            zell_free(buf);
            if (err) *err = errno_error("read", path, (int)-n);
            return NULL;
        }
        if (n == 0) break;
        len += n;
    }
    zell__plat_close(fd);
    zell_string *s = zell_string_new(buf, len);
    zell_free(buf);
    return s;
}

static zell_error *write_bytes(const zell_string *path, const void *data, int64_t n) {
    int64_t fd = zell__plat_open(zell_string_cstr(path), ZELL_PLAT_O_WRONLY | ZELL_PLAT_O_CREATE | ZELL_PLAT_O_TRUNC, 0666);
    if (fd < 0) return errno_error("open", path, (int)-fd);
    int e = zell__plat_write_all(fd, data, n);
    if (e) {
        zell__plat_close(fd);
        return errno_error("write", path, e);
    }
    e = zell__plat_close(fd);
    if (e) return errno_error("close", path, e);
    return NULL;
}

zell_error *zell_write_file(const zell_string *path, const zell_string *data) {
    return write_bytes(path, zell_string_data(data), zell_string_len(data));
}

zell_error *zell_write_file_bytes(const zell_string *path, const zell_slice *data) {
    return write_bytes(path, data->data, data->len);
}

zell_string *zell_read_line(zell_error **err) {
    if (err) *err = NULL;
    fflush(stdout);  /* make prompts visible */
    int64_t cap = 128, len = 0;
    char *buf = zell_alloc(cap);
    int c;
    bool any = false;
    while ((c = getchar()) != EOF) {
        any = true;
        if (c == '\n') break;
        if (len + 1 >= cap) { cap *= 2; buf = zell_realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    if (!any) {
        zell_free(buf);
        if (err) *err = ferror(stdin) ? zell_error_new_cstr("read stdin: I/O error")
                                      : zell_err_eof;
        return NULL;
    }
    if (len > 0 && buf[len - 1] == '\r') len--;
    zell_string *s = zell_string_new(buf, len);
    zell_free(buf);
    return s;
}
