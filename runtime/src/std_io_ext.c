/* std_io_ext.c - std.io additions (slang_io_*): stdin to EOF, line files and
 * stderr output. Same `T!` / `void!` conventions as std_io.c. */
#include "rt_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

slang_string *slang_io_read_all(slang_error **err) {
    fflush(stdout);
    int64_t cap = 4096, len = 0;
    char *buf = slang_alloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = slang_realloc(buf, cap); }
        size_t n = fread(buf + len, 1, (size_t)(cap - len), stdin);
        len += (int64_t)n;
        if (n == 0) break;
    }
    if (ferror(stdin)) {
        clearerr(stdin);
        slang_free(buf);
        if (err) *err = slang_error_new_cstr("read stdin: I/O error");
        return NULL;
    }
    slang_string *s = slang_string_new(buf, len);
    slang_free(buf);
    return s;
}

/* Lines of a file like bufio.Scanner: split on '\n', a trailing '\r' is
 * dropped, a final '\n' does not start an extra empty line. */
void slang_io_read_lines(const slang_string *path, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    slang_error *e = NULL;
    slang_string *data = slang_read_file(path, &e);
    if (e) {
        if (err) *err = e; else slang_release(e);
        return;
    }
    const char *p = slang_string_data(data);
    int64_t n = slang_string_len(data), count = 0;
    for (int64_t i = 0; i < n; i++) if (p[i] == '\n') count++;
    if (n > 0 && p[n - 1] != '\n') count++;
    slang_slice_new_ops(out, 8, count, count, &slang_ops_ref);
    slang_string **slots = out->data;
    int64_t start = 0, k = 0;
    for (int64_t i = 0; i <= n && k < count; i++) {
        if (i == n || p[i] == '\n') {
            int64_t end = i;
            if (end > start && p[end - 1] == '\r') end--;
            slots[k++] = slang_string_new(p + start, end - start);
            start = i + 1;
        }
    }
    slang_release(data);
}

void slang_io_write_lines(const slang_string *path, const slang_slice *lines, slang_error **err) {
    int64_t cnt = lines ? lines->len : 0, total = 0;
    slang_string *const *el = cnt ? lines->data : NULL;
    for (int64_t i = 0; i < cnt; i++) total += slang_string_len(el[i]) + 1;
    slang_string *buf = slang__string_alloc(total);
    char *w = buf->data;
    for (int64_t i = 0; i < cnt; i++) {
        int64_t l = slang_string_len(el[i]);
        if (l) memcpy(w, slang_string_data(el[i]), (size_t)l);
        w += l;
        *w++ = '\n';
    }
    slang_error *e = slang_write_file(path, buf);
    slang_release(buf);
    if (err) *err = e; else slang_release(e);
}

void slang_io_eprint(const slang_string *s) {
    fflush(stdout);
    slang_eprint_str(s);
}

void slang_io_eprintln(const slang_string *s) {
    fflush(stdout);
    slang_eprintln_str(s);
}
