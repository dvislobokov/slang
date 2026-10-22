/* std_fs.c - std.fs (zell_fs_*): files, directories and slash-separated
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

static inline int64_t slen(const zell_string *s) { return s ? s->len : 0; }
/* NUL-terminated: these bytes go to C APIs (views are copied, ABI.md 4.1). */
static inline const char *sdata(const zell_string *s) { return zell_string_cstr(s); }

static zell_error *mkerr(const char *op, const char *path, int e) {
    return zell_error_errno(op, path, e);
}
static void set_err(zell_error **err, const char *op, const char *path, int e) {
    if (err) *err = mkerr(op, path, e);
}

/* Paths with embedded NUL bytes cannot name a file. */
static bool bad_path(const zell_string *p) { return (int64_t)strlen(sdata(p)) != slen(p); }

#define CHECK_PATH(op, p, ret) do { if (bad_path(p)) { set_err(err, op, sdata(p), EINVAL); return ret; } } while (0)

/* Separators and volumes of the platform (see "paths" below). */
static char psep(void) { return zell__plat_path_sep(); }
static bool is_sep(char c) { return c == '/' || (c == '\\' && psep() == '\\'); }
static int64_t vol_len(const char *p, int64_t n) {
    if (psep() == '\\' && n >= 2 && p[1] == ':' && ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z'))) return 2;
    return 0;
}

/* ---- queries ---- */

bool zell_fs_exists(const zell_string *path) {
    zell__plat_stat st;
    return !bad_path(path) && slen(path) > 0 && zell__plat_stat_path(sdata(path), true, &st) == 0;
}

bool zell_fs_is_dir(const zell_string *path) {
    zell__plat_stat st;
    return !bad_path(path) && slen(path) > 0 && zell__plat_stat_path(sdata(path), true, &st) == 0 && st.kind == ZELL_PLAT_KIND_DIR;
}

int64_t zell_fs_size(const zell_string *path, zell_error **err) {
    CHECK_PATH("stat", path, 0);
    zell__plat_stat st;
    int e = zell__plat_stat_path(sdata(path), true, &st);
    if (e) { set_err(err, "stat", sdata(path), e); return 0; }
    return st.size;
}

int64_t zell_fs_mod_time(const zell_string *path, zell_error **err) {
    CHECK_PATH("stat", path, 0);
    zell__plat_stat st;
    int e = zell__plat_stat_path(sdata(path), true, &st);
    if (e) { set_err(err, "stat", sdata(path), e); return 0; }
    return st.mtime_sec * 1000 + st.mtime_nsec / 1000000;
}

static int cmp_str(const void *a, const void *b) {
    return zell_string_compare(*(zell_string *const *)a, *(zell_string *const *)b);
}

void zell_fs_read_dir(const zell_string *path, zell_slice *out, zell_error **err) {
    memset(out, 0, sizeof *out);
    CHECK_PATH("open", path, );
    int e2 = 0;
    zell__plat_dir *d = zell__plat_dir_open(sdata(path), &e2);
    if (!d) { set_err(err, "open", sdata(path), e2); return; }
    zell_slice res;
    zell_slice_new_ops(&res, 8, 0, 8, &zell_ops_ref);
    const char *name;
    int kind;
    while ((name = zell__plat_dir_next(d, &kind, &e2)) != NULL) {
        zell_string *s = zell_string_from_cstr(name);
        zell_slice_append(&res, 8, &zell_ops_ref, &s);
        zell_release(s);
    }
    zell__plat_dir_close(d);
    if (e2) {
        zell_slice_release(&res);
        set_err(err, "readdir", sdata(path), e2);
        return;
    }
    if (res.len > 1) qsort(res.data, (size_t)res.len, 8, cmp_str);
    *out = res;
}

/* ---- directory / file mutation ---- */

void zell_fs_mkdir(const zell_string *path, zell_error **err) {
    CHECK_PATH("mkdir", path, );
    int e = zell__plat_mkdir(sdata(path));
    if (e) set_err(err, "mkdir", sdata(path), e);
}

void zell_fs_mkdir_all(const zell_string *path, zell_error **err) {
    CHECK_PATH("mkdir", path, );
    int64_t n = slen(path);
    if (n == 0) { set_err(err, "mkdir", "", ENOENT); return; }
    char *buf = zell_alloc(n + 1);
    memcpy(buf, sdata(path), (size_t)n);
    for (int64_t i = 1; i <= n; i++) {
        if (i < n && !is_sep(buf[i])) continue;
        if (is_sep(buf[i - 1])) continue;  /* repeated or trailing slash */
        if (i == vol_len(buf, n)) continue;  /* "C:" */
        char save = buf[i];
        buf[i] = 0;
        zell__plat_stat st;
        int e;
        if (zell__plat_stat_path(buf, true, &st) == 0) {
            if (st.kind != ZELL_PLAT_KIND_DIR) {
                set_err(err, "mkdir", buf, ENOTDIR);
                zell_free(buf);
                return;
            }
        } else if ((e = zell__plat_mkdir(buf)) != 0 && e != EEXIST) {
            set_err(err, "mkdir", buf, e);
            zell_free(buf);
            return;
        }
        buf[i] = save;
    }
    zell_free(buf);
}

void zell_fs_remove(const zell_string *path, zell_error **err) {
    CHECK_PATH("remove", path, );
    int e = zell__plat_remove(sdata(path));
    if (e) set_err(err, "remove", sdata(path), e);
}

/* Recursive delete of a NUL-terminated path; does not follow symlinks. */
static int remove_tree(const char *p) {
    zell__plat_stat st;
    int e = zell__plat_stat_path(p, false, &st);
    if (e) return e == ENOENT ? 0 : e;
    if (st.kind != ZELL_PLAT_KIND_DIR) {
        e = zell__plat_unlink_file(p);
        return e == ENOENT ? 0 : e;
    }
    zell__plat_dir *d = zell__plat_dir_open(p, &e);
    if (!d) return e;
    size_t pl = strlen(p);
    int rc = 0;
    const char *name;
    int kind;
    while ((name = zell__plat_dir_next(d, &kind, &e)) != NULL) {
        size_t nl = strlen(name);
        char *child = zell_alloc((int64_t)(pl + nl + 2));
        memcpy(child, p, pl);
        child[pl] = '/';
        memcpy(child + pl + 1, name, nl + 1);
        int r = remove_tree(child);
        zell_free(child);
        if (r && !rc) rc = r;
    }
    zell__plat_dir_close(d);
    if (rc) return rc;
    e = zell__plat_rmdir(p);
    return e == ENOENT ? 0 : e;
}

void zell_fs_remove_all(const zell_string *path, zell_error **err) {
    CHECK_PATH("remove", path, );
    if (slen(path) == 0) return;   /* Go: RemoveAll("") is a no-op */
    int e = remove_tree(sdata(path));
    if (e) set_err(err, "remove", sdata(path), e);
}

void zell_fs_rename(const zell_string *from, const zell_string *to, zell_error **err) {
    CHECK_PATH("rename", from, );
    CHECK_PATH("rename", to, );
    int e = zell__plat_rename(sdata(from), sdata(to));
    if (e) set_err(err, "rename", sdata(from), e);
}

void zell_fs_copy(const zell_string *from, const zell_string *to, zell_error **err) {
    CHECK_PATH("open", from, );
    CHECK_PATH("open", to, );
    int64_t in = zell__plat_open(sdata(from), ZELL_PLAT_O_RDONLY, 0);
    if (in < 0) { set_err(err, "open", sdata(from), (int)-in); return; }
    zell__plat_stat st;
    if (zell__plat_fstat(in, &st) == 0 && st.kind == ZELL_PLAT_KIND_DIR) {
        zell__plat_close(in);
        set_err(err, "copy", sdata(from), EISDIR);
        return;
    }
    int64_t outfd = zell__plat_open(sdata(to), ZELL_PLAT_O_WRONLY | ZELL_PLAT_O_CREATE | ZELL_PLAT_O_TRUNC, 0666);
    if (outfd < 0) { set_err(err, "open", sdata(to), (int)-outfd); zell__plat_close(in); return; }
    char buf[65536];
    for (;;) {
        int64_t r = zell__plat_read(in, buf, sizeof buf);
        if (r < 0) {
            set_err(err, "read", sdata(from), (int)-r);
            break;
        }
        if (r == 0) break;
        int e = zell__plat_write_all(outfd, buf, r);
        if (e) { set_err(err, "write", sdata(to), e); break; }
    }
    zell__plat_close(in);
    int e = zell__plat_close(outfd);
    if (e && err && !*err) set_err(err, "close", sdata(to), e);
}

void zell_fs_append_file(const zell_string *path, const zell_string *data, zell_error **err) {
    CHECK_PATH("open", path, );
    int64_t fd = zell__plat_open(sdata(path), ZELL_PLAT_O_WRONLY | ZELL_PLAT_O_CREATE | ZELL_PLAT_O_APPEND, 0666);
    if (fd < 0) { set_err(err, "open", sdata(path), (int)-fd); return; }
    int e = zell__plat_write_all(fd, sdata(data), slen(data));
    if (e) set_err(err, "write", sdata(path), e);
    int ce = zell__plat_close(fd);
    if (ce && !e) set_err(err, "close", sdata(path), ce);
}

/* ---- paths (pure string functions) ----
 * Go's path/filepath for the platform: on Windows ('\\' separator) both '/'
 * and '\\' separate, a drive letter ("C:") is a volume prefix, and results
 * use '\\'; elsewhere only '/' and no volumes. */


/* Go filepath.Clean of p[0..n) into a new owned string. */
static zell_string *clean(const char *p, int64_t n) {
    int64_t v = vol_len(p, n);
    char sep = psep();
    char *o = zell_alloc(n + 4);
    memcpy(o, p, (size_t)v);
    char *q = o + v;  /* the path after the volume */
    const char *in = p + v;
    int64_t m = n - v;
    if (m == 0) {
        q[0] = '.';
        zell_string *res = zell_string_new(o, v + 1);
        zell_free(o);
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
    zell_string *res = zell_string_new(o, v + w);
    zell_free(o);
    return res;
}

zell_string *zell_fs_join(const zell_string *a, const zell_string *b) {
    int64_t na = slen(a), nb = slen(b);
    if (na == 0 && nb == 0) return zell__string_alloc(0);
    if (na == 0) return clean(sdata(b), nb);
    if (nb == 0) return clean(sdata(a), na);
    char *buf = zell_alloc(na + nb + 1);
    memcpy(buf, sdata(a), (size_t)na);
    buf[na] = psep();
    memcpy(buf + na + 1, sdata(b), (size_t)nb);
    zell_string *r = clean(buf, na + nb + 1);
    zell_free(buf);
    return r;
}

zell_string *zell_fs_base(const zell_string *path) {
    const char *p = sdata(path);
    int64_t n = slen(path);
    if (n == 0) return zell_string_new(".", 1);
    while (n > 0 && is_sep(p[n - 1])) n--;
    int64_t v = vol_len(p, n);
    p += v;
    n -= v;
    if (n == 0) {
        char sep = psep();
        return zell_string_new(&sep, 1);
    }
    int64_t i = n;
    while (i > 0 && !is_sep(p[i - 1])) i--;
    return zell_string_new(p + i, n - i);
}

zell_string *zell_fs_dir(const zell_string *path) {
    const char *p = sdata(path);
    int64_t n = slen(path);
    int64_t v = vol_len(p, n);
    int64_t i = n;
    while (i > v && !is_sep(p[i - 1])) i--;
    if (v == 0) return clean(p, i);
    /* volume + Clean(rest) */
    zell_string *rest = clean(p + v, i - v);
    int64_t rl = zell_string_len(rest);
    char *buf = zell_alloc(v + rl + 1);
    memcpy(buf, p, (size_t)v);
    memcpy(buf + v, zell_string_data(rest), (size_t)rl);
    zell_string *r = zell_string_new(buf, v + rl);
    zell_free(buf);
    zell_release(rest);
    return r;
}

zell_string *zell_fs_ext(const zell_string *path) {
    const char *p = sdata(path);
    int64_t n = slen(path);
    for (int64_t i = n - 1; i >= 0 && !is_sep(p[i]); i--)
        if (p[i] == '.') return zell_string_new(p + i, n - i);
    return zell__string_alloc(0);
}

static bool is_abs(const char *p, int64_t n) {
    if (psep() != '\\') return n > 0 && p[0] == '/';
    int64_t v = vol_len(p, n);
    if (v) return n > v && is_sep(p[v]);
    return n > 1 && is_sep(p[0]) && is_sep(p[1]);  /* \\server\share */
}

zell_string *zell_fs_abs(const zell_string *path, zell_error **err) {
    int64_t n = slen(path);
    if (is_abs(sdata(path), n)) return clean(sdata(path), n);
    int ce = 0;
    char *cwd = zell__plat_getcwd(&ce);
    if (!cwd) { set_err(err, "getwd", sdata(path), ce); return NULL; }
    size_t cl = strlen(cwd);
    char *buf = zell_alloc((int64_t)cl + n + 2);
    memcpy(buf, cwd, cl);
    buf[cl] = psep();
    memcpy(buf + cl + 1, sdata(path), (size_t)n);
    free(cwd);
    zell_string *r = clean(buf, (int64_t)cl + 1 + n);
    zell_free(buf);
    return r;
}

zell_string *zell_fs_temp_dir(void) {
    char *t = zell__plat_temp_dir();
    zell_string *r = zell_string_from_cstr(t ? t : "/tmp");
    free(t);
    return r;
}
