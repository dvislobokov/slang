/* std_os_file.c - file descriptors for std.os.File and std.os.ReadDir
 * (compiler/src/sema/stdlib_src/os.sl), plus the std.io.EOF sentinel.
 * POSIX calls directly, like std_fs.c. Errors are "<op> <path>: <strerror>"
 * (slang_error_errno; ENOENT wraps fs.ErrNotFound). Reads and writes
 * retry on EINTR; writes loop until everything is written. */
#include "rt_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

slang_error *slang_io_err_eof(void);
int64_t slang_os_rt_open(const slang_string *path, int64_t flags, int64_t perm, slang_error **err);
int64_t slang_os_rt_read(int64_t fd, const slang_slice *buf, slang_error **err);
int64_t slang_os_rt_pread(int64_t fd, const slang_slice *buf, int64_t off, slang_error **err);
int64_t slang_os_rt_write(int64_t fd, const slang_slice *buf, slang_error **err);
int64_t slang_os_rt_write_str(int64_t fd, const slang_string *s, slang_error **err);
int64_t slang_os_rt_seek(int64_t fd, int64_t off, int64_t whence, slang_error **err);
void slang_os_rt_close(int64_t fd, slang_error **err);
void slang_os_rt_sync(int64_t fd, slang_error **err);
void slang_os_rt_truncate(int64_t fd, int64_t size, slang_error **err);
void slang_os_rt_fstat(int64_t fd, slang_slice *out, slang_error **err);
void slang_os_rt_stat(const slang_string *path, bool follow, slang_slice *out, slang_error **err);
void slang_os_rt_read_dir(const slang_string *path, slang_slice *out, slang_error **err);
void slang_os_rt_chmod(const slang_string *path, int64_t mode, slang_error **err);

slang_error *slang_io_err_eof(void) { return slang_err_eof; }

static const char *sdata(const slang_string *s) { return s ? s->data : ""; }
/* With a path: "<op> <path>: <strerror>"; without (descriptor ops, the
 * Slang side adds "<op> <name>" context): just the strerror text. */
static void set_err(slang_error **err, const char *op, const char *path, int e) {
    if (!err) return;
    *err = path ? slang_error_errno(op, path, e) : slang_error_new_cstr(strerror(e));
}

/* Portable flag bits of os.sl (RDONLY 0, WRONLY 1, RDWR 2, APPEND 8,
 * CREATE 64, EXCL 128, SYNC 256, TRUNC 512). */
static int open_flags(int64_t f) {
    int o = (f & 3) == 1 ? O_WRONLY : (f & 3) == 2 ? O_RDWR : O_RDONLY;
    if (f & 8) o |= O_APPEND;
    if (f & 64) o |= O_CREAT;
    if (f & 128) o |= O_EXCL;
    if (f & 256) o |= O_SYNC;
    if (f & 512) o |= O_TRUNC;
    return o | O_CLOEXEC;
}

int64_t slang_os_rt_open(const slang_string *path, int64_t flags, int64_t perm, slang_error **err) {
    if (err) *err = NULL;
    const char *p = sdata(path);
    if ((int64_t)strlen(p) != (path ? path->len : 0) || !*p) {
        set_err(err, "open", p, EINVAL);
        return -1;
    }
    int fd;
    do fd = open(p, open_flags(flags), (mode_t)perm);
    while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        set_err(err, "open", p, errno);
        return -1;
    }
    return fd;
}

int64_t slang_os_rt_read(int64_t fd, const slang_slice *buf, slang_error **err) {
    if (err) *err = NULL;
    if (!buf || buf->len == 0) return 0;
    ssize_t n;
    do n = read((int)fd, buf->data, (size_t)buf->len);
    while (n < 0 && errno == EINTR);
    if (n < 0) {
        set_err(err, "read", NULL, errno);
        return 0;
    }
    return n;
}

int64_t slang_os_rt_pread(int64_t fd, const slang_slice *buf, int64_t off, slang_error **err) {
    if (err) *err = NULL;
    if (!buf || buf->len == 0) return 0;
    ssize_t n;
    do n = pread((int)fd, buf->data, (size_t)buf->len, (off_t)off);
    while (n < 0 && errno == EINTR);
    if (n < 0) {
        set_err(err, "read", NULL, errno);
        return 0;
    }
    return n;
}

static int64_t write_all(int64_t fd, const char *p, int64_t len, slang_error **err) {
    if (err) *err = NULL;
    /* keep order with println, which goes through stdio */
    if (fd == 1) fflush(stdout);
    if (fd == 2) fflush(stderr);
    int64_t done = 0;
    while (done < len) {
        ssize_t n = write((int)fd, p + done, (size_t)(len - done));
        if (n < 0) {
            if (errno == EINTR) continue;
            set_err(err, "write", NULL, errno);
            return done;
        }
        done += n;
    }
    return done;
}

int64_t slang_os_rt_write(int64_t fd, const slang_slice *buf, slang_error **err) {
    return write_all(fd, buf && buf->len ? buf->data : "", buf ? buf->len : 0, err);
}

int64_t slang_os_rt_write_str(int64_t fd, const slang_string *s, slang_error **err) {
    return write_all(fd, sdata(s), s ? s->len : 0, err);
}

int64_t slang_os_rt_seek(int64_t fd, int64_t off, int64_t whence, slang_error **err) {
    if (err) *err = NULL;
    int w = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
    off_t r = lseek((int)fd, (off_t)off, w);
    if (r < 0) {
        set_err(err, "seek", NULL, errno);
        return 0;
    }
    return (int64_t)r;
}

void slang_os_rt_close(int64_t fd, slang_error **err) {
    if (err) *err = NULL;
    if (fd < 0) return;
    if (close((int)fd) != 0 && errno != EINTR) set_err(err, "close", NULL, errno);
}

void slang_os_rt_sync(int64_t fd, slang_error **err) {
    if (err) *err = NULL;
    if (fsync((int)fd) != 0) set_err(err, "sync", NULL, errno);
}

void slang_os_rt_truncate(int64_t fd, int64_t size, slang_error **err) {
    if (err) *err = NULL;
    if (ftruncate((int)fd, (off_t)size) != 0) set_err(err, "truncate", NULL, errno);
}

/* [size, mode (permission bits), kind (0 file, 1 dir, 2 symlink, 3 other),
 *  mtime seconds, mtime nanoseconds] */
static void stat_out(const struct stat *st, slang_slice *out) {
    slang_slice_new(out, 8, 5, 5);
    int64_t *o = out->data;
    o[0] = (int64_t)st->st_size;
    o[1] = (int64_t)(st->st_mode & 07777);
    o[2] = S_ISREG(st->st_mode) ? 0 : S_ISDIR(st->st_mode) ? 1 : S_ISLNK(st->st_mode) ? 2 : 3;
    o[3] = (int64_t)st->st_mtim.tv_sec;
    o[4] = (int64_t)st->st_mtim.tv_nsec;
}

void slang_os_rt_fstat(int64_t fd, slang_slice *out, slang_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    struct stat st;
    if (fstat((int)fd, &st) != 0) {
        set_err(err, "stat", NULL, errno);
        return;
    }
    stat_out(&st, out);
}

void slang_os_rt_stat(const slang_string *path, bool follow, slang_slice *out, slang_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    struct stat st;
    const char *p = sdata(path);
    if ((follow ? stat(p, &st) : lstat(p, &st)) != 0) {
        set_err(err, follow ? "stat" : "lstat", p, errno);
        return;
    }
    stat_out(&st, out);
}

static int cmp_names(const void *a, const void *b) {
    const slang_string *x = *(slang_string *const *)a, *y = *(slang_string *const *)b;
    return strcmp(x->data + 1, y->data + 1);
}

/* Entries sorted by name; each is "<kind><name>" with kind 'f' (file),
 * 'd' (directory), 'l' (symlink) or '?' (other). */
void slang_os_rt_read_dir(const slang_string *path, slang_slice *out, slang_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    const char *p = sdata(path);
    DIR *d = opendir(p);
    if (!d) {
        set_err(err, "open", p, errno);
        return;
    }
    slang_slice_new_ops(out, 8, 0, 8, &slang_ops_ref);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char kind = '?';
        unsigned char t = e->d_type;
        if (t == DT_UNKNOWN) {
            char full[4096];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", p, e->d_name);
            if (lstat(full, &st) == 0) t = S_ISREG(st.st_mode) ? DT_REG : S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : DT_UNKNOWN;
        }
        if (t == DT_REG) kind = 'f';
        else if (t == DT_DIR) kind = 'd';
        else if (t == DT_LNK) kind = 'l';
        char buf[1024];
        int n = snprintf(buf, sizeof buf, "%c%s", kind, e->d_name);
        slang_string *s = slang_string_new(buf, n);
        slang_slice_append(out, 8, &slang_ops_ref, &s);
        slang_release(s);
    }
    closedir(d);
    if (out->len > 1) qsort(out->data, (size_t)out->len, 8, cmp_names);
}

void slang_os_rt_chmod(const slang_string *path, int64_t mode, slang_error **err) {
    if (err) *err = NULL;
    if (chmod(sdata(path), (mode_t)mode) != 0) set_err(err, "chmod", sdata(path), errno);
}
