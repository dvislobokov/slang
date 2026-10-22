/* std_os_file.c - file descriptors for os.File and os.ReadDir
 * (compiler/src/sema/stdlib_src/os.zl), plus the io.EOF sentinel.
 * System calls through platform/platform.h, like std_fs.c. Errors are "<op> <path>: <strerror>"
 * (zell_error_errno; ENOENT wraps fs.ErrNotFound). Reads and writes
 * retry on EINTR; writes loop until everything is written. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

zell_error *zell_io_err_eof(void);
int64_t zell_os_rt_open(const zell_string *path, int64_t flags, int64_t perm, zell_error **err);
int64_t zell_os_rt_read(int64_t fd, const zell_slice *buf, zell_error **err);
int64_t zell_os_rt_pread(int64_t fd, const zell_slice *buf, int64_t off, zell_error **err);
int64_t zell_os_rt_write(int64_t fd, const zell_slice *buf, zell_error **err);
int64_t zell_os_rt_write_str(int64_t fd, const zell_string *s, zell_error **err);
int64_t zell_os_rt_seek(int64_t fd, int64_t off, int64_t whence, zell_error **err);
void zell_os_rt_close(int64_t fd, zell_error **err);
void zell_os_rt_sync(int64_t fd, zell_error **err);
void zell_os_rt_truncate(int64_t fd, int64_t size, zell_error **err);
void zell_os_rt_fstat(int64_t fd, zell_slice *out, zell_error **err);
void zell_os_rt_stat(const zell_string *path, bool follow, zell_slice *out, zell_error **err);
void zell_os_rt_read_dir(const zell_string *path, zell_slice *out, zell_error **err);
void zell_os_rt_chmod(const zell_string *path, int64_t mode, zell_error **err);

zell_error *zell_io_err_eof(void) { return zell_err_eof; }

/* NUL-terminated: these bytes go to C APIs (views are copied, ABI.md 4.1). */
static const char *sdata(const zell_string *s) { return zell_string_cstr(s); }
/* With a path: "<op> <path>: <strerror>"; without (descriptor ops, the
 * Zell side adds "<op> <name>" context): just the strerror text. */
static void set_err(zell_error **err, const char *op, const char *path, int e) {
    if (!err) return;
    *err = path ? zell_error_errno(op, path, e) : zell_error_new_cstr(strerror(e));
}

/* The flag bits of os.zl (RDONLY 0, WRONLY 1, RDWR 2, APPEND 8, CREATE 64,
 * EXCL 128, SYNC 256, TRUNC 512) are the platform layer's ZELL_PLAT_O_*. */
int64_t zell_os_rt_open(const zell_string *path, int64_t flags, int64_t perm, zell_error **err) {
    if (err) *err = NULL;
    const char *p = sdata(path);
    if ((int64_t)strlen(p) != (path ? path->len : 0) || !*p) {
        set_err(err, "open", p, EINVAL);
        return -1;
    }
    int64_t fd = zell__plat_open(p, (int)flags, (int)perm);
    if (fd < 0) {
        set_err(err, "open", p, (int)-fd);
        return -1;
    }
    return fd;
}

int64_t zell_os_rt_read(int64_t fd, const zell_slice *buf, zell_error **err) {
    if (err) *err = NULL;
    if (!buf || buf->len == 0) return 0;
    int64_t n = zell__plat_read(fd, buf->data, buf->len);
    if (n < 0) {
        set_err(err, "read", NULL, (int)-n);
        return 0;
    }
    return n;
}

int64_t zell_os_rt_pread(int64_t fd, const zell_slice *buf, int64_t off, zell_error **err) {
    if (err) *err = NULL;
    if (!buf || buf->len == 0) return 0;
    int64_t n = zell__plat_pread(fd, buf->data, buf->len, off);
    if (n < 0) {
        set_err(err, "read", NULL, (int)-n);
        return 0;
    }
    return n;
}

static int64_t write_all(int64_t fd, const char *p, int64_t len, zell_error **err) {
    if (err) *err = NULL;
    /* keep order with println, which goes through stdio */
    if (fd == 1) fflush(stdout);
    if (fd == 2) fflush(stderr);
    int64_t done = 0;
    while (done < len) {
        int64_t n = zell__plat_write(fd, p + done, len - done);
        if (n < 0) {
            set_err(err, "write", NULL, (int)-n);
            return done;
        }
        done += n;
    }
    return done;
}

int64_t zell_os_rt_write(int64_t fd, const zell_slice *buf, zell_error **err) {
    return write_all(fd, buf && buf->len ? buf->data : "", buf ? buf->len : 0, err);
}

int64_t zell_os_rt_write_str(int64_t fd, const zell_string *s, zell_error **err) {
    return write_all(fd, sdata(s), s ? s->len : 0, err);
}

int64_t zell_os_rt_seek(int64_t fd, int64_t off, int64_t whence, zell_error **err) {
    if (err) *err = NULL;
    int64_t r = zell__plat_seek(fd, off, (int)whence);
    if (r < 0) {
        set_err(err, "seek", NULL, (int)-r);
        return 0;
    }
    return r;
}

void zell_os_rt_close(int64_t fd, zell_error **err) {
    if (err) *err = NULL;
    if (fd < 0) return;
    int e = zell__plat_close(fd);
    if (e) set_err(err, "close", NULL, e);
}

void zell_os_rt_sync(int64_t fd, zell_error **err) {
    if (err) *err = NULL;
    int e = zell__plat_fsync(fd);
    if (e) set_err(err, "sync", NULL, e);
}

void zell_os_rt_truncate(int64_t fd, int64_t size, zell_error **err) {
    if (err) *err = NULL;
    int e = zell__plat_ftruncate(fd, size);
    if (e) set_err(err, "truncate", NULL, e);
}

/* [size, mode (permission bits), kind (0 file, 1 dir, 2 symlink, 3 other),
 *  mtime seconds, mtime nanoseconds] */
static void stat_out(const zell__plat_stat *st, zell_slice *out) {
    zell_slice_new(out, 8, 5, 5);
    int64_t *o = out->data;
    o[0] = st->size;
    o[1] = (int64_t)st->perm;
    o[2] = st->kind;
    o[3] = st->mtime_sec;
    o[4] = st->mtime_nsec;
}

void zell_os_rt_fstat(int64_t fd, zell_slice *out, zell_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    zell__plat_stat st;
    int e = zell__plat_fstat(fd, &st);
    if (e) {
        set_err(err, "stat", NULL, e);
        return;
    }
    stat_out(&st, out);
}

void zell_os_rt_stat(const zell_string *path, bool follow, zell_slice *out, zell_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    zell__plat_stat st;
    const char *p = sdata(path);
    int e = zell__plat_stat_path(p, follow, &st);
    if (e) {
        set_err(err, follow ? "stat" : "lstat", p, e);
        return;
    }
    stat_out(&st, out);
}

static int cmp_names(const void *a, const void *b) {
    const zell_string *x = *(zell_string *const *)a, *y = *(zell_string *const *)b;
    int64_t n = (x->len < y->len ? x->len : y->len) - 1;
    int c = n > 0 ? memcmp(x->data + 1, y->data + 1, (size_t)n) : 0;
    return c ? c : (x->len > y->len) - (x->len < y->len);
}

/* Entries sorted by name; each is "<kind><name>" with kind 'f' (file),
 * 'd' (directory), 'l' (symlink) or '?' (other). */
void zell_os_rt_read_dir(const zell_string *path, zell_slice *out, zell_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    const char *p = sdata(path);
    int e = 0;
    zell__plat_dir *d = zell__plat_dir_open(p, &e);
    if (!d) {
        set_err(err, "open", p, e);
        return;
    }
    zell_slice_new_ops(out, 8, 0, 8, &zell_ops_ref);
    const char *name;
    int t;
    while ((name = zell__plat_dir_next(d, &t, &e)) != NULL) {
        if (t < 0) {
            char full[4096];
            zell__plat_stat st;
            snprintf(full, sizeof full, "%s/%s", p, name);
            t = zell__plat_stat_path(full, false, &st) == 0 ? st.kind : ZELL_PLAT_KIND_OTHER;
        }
        char kind = t == ZELL_PLAT_KIND_FILE ? 'f' : t == ZELL_PLAT_KIND_DIR ? 'd' : t == ZELL_PLAT_KIND_LINK ? 'l' : '?';
        char buf[1024];
        int n = snprintf(buf, sizeof buf, "%c%s", kind, name);
        zell_string *s = zell_string_new(buf, n);
        zell_slice_append(out, 8, &zell_ops_ref, &s);
        zell_release(s);
    }
    zell__plat_dir_close(d);
    if (out->len > 1) qsort(out->data, (size_t)out->len, 8, cmp_names);
}

void zell_os_rt_chmod(const zell_string *path, int64_t mode, zell_error **err) {
    if (err) *err = NULL;
    int e = zell__plat_chmod(sdata(path), (int)mode);
    if (e) set_err(err, "chmod", sdata(path), e);
}
