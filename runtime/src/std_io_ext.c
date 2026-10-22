/* std_io_ext.c - io additions (zell_io_*): stdin to EOF, line files and
 * stderr output. Same `T!` / `void!` conventions as std_io.c. */
#include "rt_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

zell_string *zell_io_read_all(zell_error **err) {
    fflush(stdout);
    int64_t cap = 4096, len = 0;
    char *buf = zell_alloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = zell_realloc(buf, cap); }
        size_t n = fread(buf + len, 1, (size_t)(cap - len), stdin);
        len += (int64_t)n;
        if (n == 0) break;
    }
    if (ferror(stdin)) {
        clearerr(stdin);
        zell_free(buf);
        if (err) *err = zell_error_new_cstr("read stdin: I/O error");
        return NULL;
    }
    zell_string *s = zell_string_new(buf, len);
    zell_free(buf);
    return s;
}

/* Lines of a file like bufio.Scanner: split on '\n', a trailing '\r' is
 * dropped, a final '\n' does not start an extra empty line. */
void zell_io_read_lines(const zell_string *path, zell_slice *out, zell_error **err) {
    memset(out, 0, sizeof *out);
    zell_error *e = NULL;
    zell_string *data = zell_read_file(path, &e);
    if (e) {
        if (err) *err = e; else zell_release(e);
        return;
    }
    const char *p = zell_string_data(data);
    int64_t n = zell_string_len(data), count = 0;
    for (int64_t i = 0; i < n; i++) if (p[i] == '\n') count++;
    if (n > 0 && p[n - 1] != '\n') count++;
    zell_slice_new_ops(out, 8, count, count, &zell_ops_ref);
    zell_string **slots = out->data;
    int64_t start = 0, k = 0;
    for (int64_t i = 0; i <= n && k < count; i++) {
        if (i == n || p[i] == '\n') {
            int64_t end = i;
            if (end > start && p[end - 1] == '\r') end--;
            slots[k++] = zell_string_new(p + start, end - start);
            start = i + 1;
        }
    }
    zell_release(data);
}

void zell_io_write_lines(const zell_string *path, const zell_slice *lines, zell_error **err) {
    int64_t cnt = lines ? lines->len : 0, total = 0;
    zell_string *const *el = cnt ? lines->data : NULL;
    for (int64_t i = 0; i < cnt; i++) total += zell_string_len(el[i]) + 1;
    zell_string *buf = zell__string_alloc(total);
    char *w = buf->data;
    for (int64_t i = 0; i < cnt; i++) {
        int64_t l = zell_string_len(el[i]);
        if (l) memcpy(w, zell_string_data(el[i]), (size_t)l);
        w += l;
        *w++ = '\n';
    }
    zell_error *e = zell_write_file(path, buf);
    zell_release(buf);
    if (err) *err = e; else zell_release(e);
}

void zell_io_eprint(const zell_string *s) {
    fflush(stdout);
    zell_eprint_str(s);
}

void zell_io_eprintln(const zell_string *s) {
    fflush(stdout);
    zell_eprintln_str(s);
}
