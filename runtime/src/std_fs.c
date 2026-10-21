/* std_fs.c - std.fs (slang_fs_*): files, directories and slash-separated
 * paths. Path helpers follow Go's path/filepath (Join/Dir/Abs clean their
 * result; on Windows both separators and drive letters, see below).
 * Failing calls give "<op> <path>: <strerror>" errors. System calls go
 * through platform/platform.h (POSIX or Win32 with UTF-16 paths). */
#include "rt_internal.h"
#include "platform/platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int64_t slen(const slang_string *s) { return s ? s->len : 0; }
/* NUL-terminated: these bytes go to C APIs (views are copied, ABI.md 4.1). */
static inline const char *sdata(const slang_string *s) { return slang_string_cstr(s); }

static slang_error *mkerr(const char *op, const char *path, int e) {
    return slang_error_errno(op, path, e);
}
static void set_err(slang_error **err, const char *op, const char *path, int e) {
    if (err) *err = mkerr(op, path, e);
}

/* Paths with embedded NUL bytes cannot name a file. */
static bool bad_path(const slang_string *p) { return (int64_t)strlen(sdata(p)) != slen(p); }

#define CHECK_PATH(op, p, ret) do { if (bad_path(p)) { set_err(err, op, sdata(p), EINVAL); return ret; } } while (0)

/* Separators and volumes of the platform (see "paths" below). */
static char psep(void) { return slang__plat_path_sep(); }
static bool is_sep(char c) { return c == '/' || (c == '\\' && psep() == '\\'); }
static int64_t vol_len(const char *p, int64_t n) {
    if (psep() == '\\' && n >= 2 && p[1] == ':' && ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z'))) return 2;
    return 0;
}

/* ---- queries ---- */

bool slang_fs_exists(const slang_string *path) {
    slang__plat_stat st;
    return !bad_path(path) && slen(path) > 0 && slang__plat_stat_path(sdata(path), true, &st) == 0;
}

bool slang_fs_is_dir(const slang_string *path) {
    slang__plat_stat st;
    return !bad_path(path) && slen(path) > 0 && slang__plat_stat_path(sdata(path), true, &st) == 0 && st.kind == SLANG_PLAT_KIND_DIR;
}

int64_t slang_fs_size(const slang_string *path, slang_error **err) {
    CHECK_PATH("stat", path, 0);
    slang__plat_stat st;
    int e = slang__plat_stat_path(sdata(path), true, &st);
    if (e) { set_err(err, "stat", sdata(path), e); return 0; }
    return st.size;
}

int64_t slang_fs_mod_time(const slang_string *path, slang_error **err) {
    CHECK_PATH("stat", path, 0);
    slang__plat_stat st;
    int e = slang__plat_stat_path(sdata(path), true, &st);
    if (e) { set_err(err, "stat", sdata(path), e); return 0; }
    return st.mtime_sec * 1000 + st.mtime_nsec / 1000000;
}

static int cmp_str(const void *a, const void *b) {
    return slang_string_compare(*(slang_string *const *)a, *(slang_string *const *)b);
}

void slang_fs_read_dir(const slang_string *path, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    CHECK_PATH("open", path, );
    int e2 = 0;
    slang__plat_dir *d = slang__plat_dir_open(sdata(path), &e2);
    if (!d) { set_err(err, "open", sdata(path), e2); return; }
    slang_slice res;
    slang_slice_new_ops(&res, 8, 0, 8, &slang_ops_ref);
    const char *name;
    int kind;
    while ((name = slang__plat_dir_next(d, &kind, &e2)) != NULL) {
        slang_string *s = slang_string_from_cstr(name);
        slang_slice_append(&res, 8, &slang_ops_ref, &s);
        slang_release(s);
    }
    slang__plat_dir_close(d);
    if (e2) {
        slang_slice_release(&res);
        set_err(err, "readdir", sdata(path), e2);
        return;
    }
    if (res.len > 1) qsort(res.data, (size_t)res.len, 8, cmp_str);
    *out = res;
}

/* ---- directory / file mutation ---- */

void slang_fs_mkdir(const slang_string *path, slang_error **err) {
    CHECK_PATH("mkdir", path, );
    int e = slang__plat_mkdir(sdata(path));
    if (e) set_err(err, "mkdir", sdata(path), e);
}

void slang_fs_mkdir_all(const slang_string *path, slang_error **err) {
    CHECK_PATH("mkdir", path, );
    int64_t n = slen(path);
    if (n == 0) { set_err(err, "mkdir", "", ENOENT); return; }
    char *buf = slang_alloc(n + 1);
    memcpy(buf, sdata(path), (size_t)n);
    for (int64_t i = 1; i <= n; i++) {
        if (i < n && !is_sep(buf[i])) continue;
        if (is_sep(buf[i - 1])) continue;  /* repeated or trailing slash */
        if (i == vol_len(buf, n)) continue;  /* "C:" */
        char save = buf[i];
        buf[i] = 0;
        slang__plat_stat st;
        int e;
        if (slang__plat_stat_path(buf, true, &st) == 0) {
            if (st.kind != SLANG_PLAT_KIND_DIR) {
                set_err(err, "mkdir", buf, ENOTDIR);
                slang_free(buf);
                return;
            }
        } else if ((e = slang__plat_mkdir(buf)) != 0 && e != EEXIST) {
            set_err(err, "mkdir", buf, e);
            slang_free(buf);
            return;
        }
        buf[i] = save;
    }
    slang_free(buf);
}

void slang_fs_remove(const slang_string *path, slang_error **err) {
    CHECK_PATH("remove", path, );
    int e = slang__plat_remove(sdata(path));
    if (e) set_err(err, "remove", sdata(path), e);
}

/* Recursive delete of a NUL-terminated path; does not follow symlinks. */
static int remove_tree(const char *p) {
    slang__plat_stat st;
    int e = slang__plat_stat_path(p, false, &st);
    if (e) return e == ENOENT ? 0 : e;
    if (st.kind != SLANG_PLAT_KIND_DIR) {
        e = slang__plat_unlink_file(p);
        return e == ENOENT ? 0 : e;
    }
    slang__plat_dir *d = slang__plat_dir_open(p, &e);
    if (!d) return e;
    size_t pl = strlen(p);
    int rc = 0;
    const char *name;
    int kind;
    while ((name = slang__plat_dir_next(d, &kind, &e)) != NULL) {
        size_t nl = strlen(name);
        char *child = slang_alloc((int64_t)(pl + nl + 2));
        memcpy(child, p, pl);
        child[pl] = '/';
        memcpy(child + pl + 1, name, nl + 1);
        int r = remove_tree(child);
        slang_free(child);
        if (r && !rc) rc = r;
    }
    slang__plat_dir_close(d);
    if (rc) return rc;
    e = slang__plat_rmdir(p);
    return e == ENOENT ? 0 : e;
}

void slang_fs_remove_all(const slang_string *path, slang_error **err) {
    CHECK_PATH("remove", path, );
    if (slen(path) == 0) return;   /* Go: RemoveAll("") is a no-op */
    int e = remove_tree(sdata(path));
    if (e) set_err(err, "remove", sdata(path), e);
}

void slang_fs_rename(const slang_string *from, const slang_string *to, slang_error **err) {
    CHECK_PATH("rename", from, );
    CHECK_PATH("rename", to, );
    int e = slang__plat_rename(sdata(from), sdata(to));
    if (e) set_err(err, "rename", sdata(from), e);
}

void slang_fs_copy(const slang_string *from, const slang_string *to, slang_error **err) {
    CHECK_PATH("open", from, );
    CHECK_PATH("open", to, );
    int64_t in = slang__plat_open(sdata(from), SLANG_PLAT_O_RDONLY, 0);
    if (in < 0) { set_err(err, "open", sdata(from), (int)-in); return; }
    slang__plat_stat st;
    if (slang__plat_fstat(in, &st) == 0 && st.kind == SLANG_PLAT_KIND_DIR) {
        slang__plat_close(in);
        set_err(err, "copy", sdata(from), EISDIR);
        return;
    }
    int64_t outfd = slang__plat_open(sdata(to), SLANG_PLAT_O_WRONLY | SLANG_PLAT_O_CREATE | SLANG_PLAT_O_TRUNC, 0666);
    if (outfd < 0) { set_err(err, "open", sdata(to), (int)-outfd); slang__plat_close(in); return; }
    char buf[65536];
    for (;;) {
        int64_t r = slang__plat_read(in, buf, sizeof buf);
        if (r < 0) {
            set_err(err, "read", sdata(from), (int)-r);
            break;
        }
        if (r == 0) break;
        int e = slang__plat_write_all(outfd, buf, r);
        if (e) { set_err(err, "write", sdata(to), e); break; }
    }
    slang__plat_close(in);
    int e = slang__plat_close(outfd);
    if (e && err && !*err) set_err(err, "close", sdata(to), e);
}

void slang_fs_append_file(const slang_string *path, const slang_string *data, slang_error **err) {
    CHECK_PATH("open", path, );
    int64_t fd = slang__plat_open(sdata(path), SLANG_PLAT_O_WRONLY | SLANG_PLAT_O_CREATE | SLANG_PLAT_O_APPEND, 0666);
    if (fd < 0) { set_err(err, "open", sdata(path), (int)-fd); return; }
    int e = slang__plat_write_all(fd, sdata(data), slen(data));
    if (e) set_err(err, "write", sdata(path), e);
    int ce = slang__plat_close(fd);
    if (ce && !e) set_err(err, "close", sdata(path), ce);
}

/* ---- paths (pure string functions) ----
 * Go's path/filepath for the platform: on Windows ('\\' separator) both '/'
 * and '\\' separate, a drive letter ("C:") is a volume prefix, and results
 * use '\\'; elsewhere only '/' and no volumes. */


/* Go filepath.Clean of p[0..n) into a new owned string. */
static slang_string *clean(const char *p, int64_t n) {
    int64_t v = vol_len(p, n);
    char sep = psep();
    char *o = slang_alloc(n + 4);
    memcpy(o, p, (size_t)v);
    char *q = o + v;  /* the path after the volume */
    const char *in = p + v;
    int64_t m = n - v;
    if (m == 0) {
        q[0] = '.';
        slang_string *res = slang_string_new(o, v + 1);
        slang_free(o);
        return res;
    }
    bool rooted = is_sep(in[0]);
    int64_t r = 0, w = 0, dotdot = 0;
    if (rooted) { q[w++] = sep; r = 1; dotdot = 1; }
    while (r < m) {
        if (is_sep(in[r])) {
            r++;
        } else if (in[r] == '.' && (r + 1 == m || is_sep(in[r + 1]))) {
            r++;
        } else if (in[r] == '.' && r + 1 < m && in[r + 1] == '.' && (r + 2 == m || is_sep(in[r + 2]))) {
            r += 2;
            if (w > dotdot) {
                w--;
                while (w > dotdot && q[w] != sep) w--;
            } else if (!rooted) {
                if (w > 0) q[w++] = sep;
                q[w++] = '.';
                q[w++] = '.';
                dotdot = w;
            }
        } else {
            if ((rooted && w != 1) || (!rooted && w != 0)) q[w++] = sep;
            while (r < m && !is_sep(in[r])) q[w++] = in[r++];
        }
    }
    if (w == 0) q[w++] = '.';
    slang_string *res = slang_string_new(o, v + w);
    slang_free(o);
    return res;
}

slang_string *slang_fs_join(const slang_string *a, const slang_string *b) {
    int64_t na = slen(a), nb = slen(b);
    if (na == 0 && nb == 0) return slang__string_alloc(0);
    if (na == 0) return clean(sdata(b), nb);
    if (nb == 0) return clean(sdata(a), na);
    char *buf = slang_alloc(na + nb + 1);
    memcpy(buf, sdata(a), (size_t)na);
    buf[na] = psep();
    memcpy(buf + na + 1, sdata(b), (size_t)nb);
    slang_string *r = clean(buf, na + nb + 1);
    slang_free(buf);
    return r;
}

slang_string *slang_fs_base(const slang_string *path) {
    const char *p = sdata(path);
    int64_t n = slen(path);
    if (n == 0) return slang_string_new(".", 1);
    while (n > 0 && is_sep(p[n - 1])) n--;
    int64_t v = vol_len(p, n);
    p += v;
    n -= v;
    if (n == 0) {
        char sep = psep();
        return slang_string_new(&sep, 1);
    }
    int64_t i = n;
    while (i > 0 && !is_sep(p[i - 1])) i--;
    return slang_string_new(p + i, n - i);
}

slang_string *slang_fs_dir(const slang_string *path) {
    const char *p = sdata(path);
    int64_t n = slen(path);
    int64_t v = vol_len(p, n);
    int64_t i = n;
    while (i > v && !is_sep(p[i - 1])) i--;
    if (v == 0) return clean(p, i);
    /* volume + Clean(rest) */
    slang_string *rest = clean(p + v, i - v);
    int64_t rl = slang_string_len(rest);
    char *buf = slang_alloc(v + rl + 1);
    memcpy(buf, p, (size_t)v);
    memcpy(buf + v, slang_string_data(rest), (size_t)rl);
    slang_string *r = slang_string_new(buf, v + rl);
    slang_free(buf);
    slang_release(rest);
    return r;
}

slang_string *slang_fs_ext(const slang_string *path) {
    const char *p = sdata(path);
    int64_t n = slen(path);
    for (int64_t i = n - 1; i >= 0 && !is_sep(p[i]); i--)
        if (p[i] == '.') return slang_string_new(p + i, n - i);
    return slang__string_alloc(0);
}

static bool is_abs(const char *p, int64_t n) {
    if (psep() != '\\') return n > 0 && p[0] == '/';
    int64_t v = vol_len(p, n);
    if (v) return n > v && is_sep(p[v]);
    return n > 1 && is_sep(p[0]) && is_sep(p[1]);  /* \\server\share */
}

slang_string *slang_fs_abs(const slang_string *path, slang_error **err) {
    int64_t n = slen(path);
    if (is_abs(sdata(path), n)) return clean(sdata(path), n);
    int ce = 0;
    char *cwd = slang__plat_getcwd(&ce);
    if (!cwd) { set_err(err, "getwd", sdata(path), ce); return NULL; }
    size_t cl = strlen(cwd);
    char *buf = slang_alloc((int64_t)cl + n + 2);
    memcpy(buf, cwd, cl);
    buf[cl] = psep();
    memcpy(buf + cl + 1, sdata(path), (size_t)n);
    free(cwd);
    slang_string *r = clean(buf, (int64_t)cl + 1 + n);
    slang_free(buf);
    return r;
}

slang_string *slang_fs_temp_dir(void) {
    char *t = slang__plat_temp_dir();
    slang_string *r = slang_string_from_cstr(t ? t : "/tmp");
    free(t);
    return r;
}
