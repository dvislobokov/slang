/* io.c - console output, files, stdin. */
#include "rt_internal.h"

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
    if (n > 0) fwrite(p, 1, (size_t)n, stdout);
}

void slang_print_str(const slang_string *s) { out(slang_string_data(s), slang_string_len(s)); }
void slang_print_cstr(const char *s) { if (s) out(s, (int64_t)strlen(s)); }
void slang_print_int(int64_t v) { printf("%" PRId64, v); }
void slang_print_uint(uint64_t v) { printf("%" PRIu64, v); }
void slang_print_float(double v) {
    char buf[64];
    int n = slang__format_float(buf, sizeof buf, v);
    out(buf, n);
}
void slang_print_bool(bool v) { fputs(v ? "true" : "false", stdout); }
void slang_print_rune(int32_t r) {
    char buf[4];
    out(buf, slang__utf8_encode(buf, r));
}
void slang_print_space(void) { putchar(' '); }
void slang_print_newline(void) { putchar('\n'); }

/* println_X: value + '\n' written under one stdio lock so lines don't interleave. */
#define LINE(stmt) do { flockfile(stdout); stmt; putchar_unlocked('\n'); funlockfile(stdout); } while (0)
void slang_println_str(const slang_string *s) { LINE(slang_print_str(s)); }
void slang_println_cstr(const char *s) { LINE(slang_print_cstr(s)); }
void slang_println_int(int64_t v) { LINE(slang_print_int(v)); }
void slang_println_uint(uint64_t v) { LINE(slang_print_uint(v)); }
void slang_println_float(double v) { LINE(slang_print_float(v)); }
void slang_println_bool(bool v) { LINE(slang_print_bool(v)); }
void slang_println_rune(int32_t r) { LINE(slang_print_rune(r)); }

void slang_eprint_str(const slang_string *s) {
    int64_t n = slang_string_len(s);
    if (n) fwrite(slang_string_data(s), 1, (size_t)n, stderr);
}
void slang_eprintln_str(const slang_string *s) {
    flockfile(stderr);
    slang_eprint_str(s);
    putc_unlocked('\n', stderr);
    funlockfile(stderr);
}

static slang_error *errno_error(const char *op, const slang_string *path, int e) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s %s: %s", op, slang_string_data(path), strerror(e));
    return slang_error_new_cstr(buf);
}

slang_string *slang_read_file(const slang_string *path, slang_error **err) {
    if (err) *err = NULL;
    FILE *f = fopen(slang_string_data(path), "rb");
    if (!f) { if (err) *err = errno_error("open", path, errno); return NULL; }
    int64_t cap = 4096, len = 0;
    char *buf = slang_alloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = slang_realloc(buf, cap); }
        size_t n = fread(buf + len, 1, (size_t)(cap - len), f);
        len += (int64_t)n;
        if (n == 0) {
            if (ferror(f)) {
                int e = errno;
                fclose(f);
                slang_free(buf);
                if (err) *err = errno_error("read", path, e);
                return NULL;
            }
            break;
        }
    }
    fclose(f);
    slang_string *s = slang_string_new(buf, len);
    slang_free(buf);
    return s;
}

static slang_error *write_bytes(const slang_string *path, const void *data, int64_t n) {
    FILE *f = fopen(slang_string_data(path), "wb");
    if (!f) return errno_error("open", path, errno);
    if (n > 0 && fwrite(data, 1, (size_t)n, f) != (size_t)n) {
        int e = errno;
        fclose(f);
        return errno_error("write", path, e);
    }
    if (fclose(f) != 0) return errno_error("close", path, errno);
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
