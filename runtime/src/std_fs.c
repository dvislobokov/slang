/* std_fs.c - std.fs (slang_fs_*): files, directories and slash-separated
 * paths. Path helpers follow Go's path/filepath on Unix (Join/Dir/Abs clean
 * their result). Failing calls give "<op> <path>: <strerror>" errors. */
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

static inline int64_t slen(const slang_string *s) { return s ? s->len : 0; }
static inline const char *sdata(const slang_string *s) { return s ? s->data : ""; }

static slang_error *mkerr(const char *op, const char *path, int e) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s %.900s: %s", op, path, strerror(e));
    return slang_error_new_cstr(buf);
}
static void set_err(slang_error **err, const char *op, const char *path, int e) {
    if (err) *err = mkerr(op, path, e);
}

/* Paths with embedded NUL bytes cannot name a file. */
static bool bad_path(const slang_string *p) { return (int64_t)strlen(sdata(p)) != slen(p); }

#define CHECK_PATH(op, p, ret) do { if (bad_path(p)) { set_err(err, op, sdata(p), EINVAL); return ret; } } while (0)

/* ---- queries ---- */

bool slang_fs_exists(const slang_string *path) {
    struct stat st;
    return !bad_path(path) && slen(path) > 0 && stat(sdata(path), &st) == 0;
}

bool slang_fs_is_dir(const slang_string *path) {
    struct stat st;
    return !bad_path(path) && slen(path) > 0 && stat(sdata(path), &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t slang_fs_size(const slang_string *path, slang_error **err) {
    CHECK_PATH("stat", path, 0);
    struct stat st;
    if (stat(sdata(path), &st) != 0) { set_err(err, "stat", sdata(path), errno); return 0; }
    return (int64_t)st.st_size;
}

int64_t slang_fs_mod_time(const slang_string *path, slang_error **err) {
    CHECK_PATH("stat", path, 0);
    struct stat st;
    if (stat(sdata(path), &st) != 0) { set_err(err, "stat", sdata(path), errno); return 0; }
    return (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
}

static int cmp_str(const void *a, const void *b) {
    return slang_string_compare(*(slang_string *const *)a, *(slang_string *const *)b);
}

void slang_fs_read_dir(const slang_string *path, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    CHECK_PATH("open", path, );
    DIR *d = opendir(sdata(path));
    if (!d) { set_err(err, "open", sdata(path), errno); return; }
    slang_slice res;
    slang_slice_new_ops(&res, 8, 0, 8, &slang_ops_ref);
    struct dirent *e;
    errno = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        slang_string *s = slang_string_from_cstr(e->d_name);
        slang_slice_append(&res, 8, &slang_ops_ref, &s);
        slang_release(s);
    }
    int e2 = errno;
    closedir(d);
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
    if (mkdir(sdata(path), 0777) != 0) set_err(err, "mkdir", sdata(path), errno);
}

void slang_fs_mkdir_all(const slang_string *path, slang_error **err) {
    CHECK_PATH("mkdir", path, );
    int64_t n = slen(path);
    if (n == 0) { set_err(err, "mkdir", "", ENOENT); return; }
    char *buf = slang_alloc(n + 1);
    memcpy(buf, sdata(path), (size_t)n);
    for (int64_t i = 1; i <= n; i++) {
        if (i < n && buf[i] != '/') continue;
        if (buf[i - 1] == '/') continue;   /* repeated or trailing slash */
        char save = buf[i];
        buf[i] = 0;
        struct stat st;
        if (stat(buf, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                set_err(err, "mkdir", buf, ENOTDIR);
                slang_free(buf);
                return;
            }
        } else if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
            set_err(err, "mkdir", buf, errno);
            slang_free(buf);
            return;
        }
        buf[i] = save;
    }
    slang_free(buf);
}

void slang_fs_remove(const slang_string *path, slang_error **err) {
    CHECK_PATH("remove", path, );
    if (remove(sdata(path)) != 0) set_err(err, "remove", sdata(path), errno);
}

/* Recursive delete of a NUL-terminated path; does not follow symlinks. */
static int remove_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) return errno == ENOENT ? 0 : errno;
    if (!S_ISDIR(st.st_mode)) return unlink(p) == 0 || errno == ENOENT ? 0 : errno;
    DIR *d = opendir(p);
    if (!d) return errno;
    size_t pl = strlen(p);
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        size_t nl = strlen(e->d_name);
        char *child = slang_alloc((int64_t)(pl + nl + 2));
        memcpy(child, p, pl);
        child[pl] = '/';
        memcpy(child + pl + 1, e->d_name, nl + 1);
        int r = remove_tree(child);
        slang_free(child);
        if (r && !rc) rc = r;
    }
    closedir(d);
    if (rc) return rc;
    return rmdir(p) == 0 || errno == ENOENT ? 0 : errno;
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
    if (rename(sdata(from), sdata(to)) != 0) set_err(err, "rename", sdata(from), errno);
}

static int write_all(int fd, const char *p, int64_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, (size_t)n);
        if (w < 0) { if (errno == EINTR) continue; return errno; }
        p += w;
        n -= w;
    }
    return 0;
}

void slang_fs_copy(const slang_string *from, const slang_string *to, slang_error **err) {
    CHECK_PATH("open", from, );
    CHECK_PATH("open", to, );
    int in = open(sdata(from), O_RDONLY | O_CLOEXEC);
    if (in < 0) { set_err(err, "open", sdata(from), errno); return; }
    struct stat st;
    if (fstat(in, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(in);
        set_err(err, "copy", sdata(from), EISDIR);
        return;
    }
    int outfd = open(sdata(to), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (outfd < 0) { set_err(err, "open", sdata(to), errno); close(in); return; }
    char buf[65536];
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            set_err(err, "read", sdata(from), errno);
            break;
        }
        if (r == 0) break;
        int e = write_all(outfd, buf, r);
        if (e) { set_err(err, "write", sdata(to), e); break; }
    }
    close(in);
    if (close(outfd) != 0 && err && !*err) set_err(err, "close", sdata(to), errno);
}

void slang_fs_append_file(const slang_string *path, const slang_string *data, slang_error **err) {
    CHECK_PATH("open", path, );
    int fd = open(sdata(path), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd < 0) { set_err(err, "open", sdata(path), errno); return; }
    int e = write_all(fd, sdata(data), slen(data));
    if (e) set_err(err, "write", sdata(path), e);
    if (close(fd) != 0 && !e) set_err(err, "close", sdata(path), errno);
}

/* ---- paths (pure string functions) ---- */

/* Go path.Clean of p[0..n) into a new owned string. */
static slang_string *clean(const char *p, int64_t n) {
    if (n == 0) return slang_string_new(".", 1);
    char *o = slang_alloc(n + 4);
    bool rooted = p[0] == '/';
    int64_t r = 0, w = 0, dotdot = 0;
    if (rooted) { o[w++] = '/'; r = 1; dotdot = 1; }
    while (r < n) {
        if (p[r] == '/') {
            r++;
        } else if (p[r] == '.' && (r + 1 == n || p[r + 1] == '/')) {
            r++;
        } else if (p[r] == '.' && p[r + 1] == '.' && (r + 2 == n || p[r + 2] == '/')) {
            r += 2;
            if (w > dotdot) {
                w--;
                while (w > dotdot && o[w] != '/') w--;
            } else if (!rooted) {
                if (w > 0) o[w++] = '/';
                o[w++] = '.';
                o[w++] = '.';
                dotdot = w;
            }
        } else {
            if ((rooted && w != 1) || (!rooted && w != 0)) o[w++] = '/';
            while (r < n && p[r] != '/') o[w++] = p[r++];
        }
    }
    slang_string *res = w == 0 ? slang_string_new(".", 1) : slang_string_new(o, w);
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
    buf[na] = '/';
    memcpy(buf + na + 1, sdata(b), (size_t)nb);
    slang_string *r = clean(buf, na + nb + 1);
    slang_free(buf);
    return r;
}

slang_string *slang_fs_base(const slang_string *path) {
    const char *p = sdata(path);
    int64_t n = slen(path);
    if (n == 0) return slang_string_new(".", 1);
    while (n > 0 && p[n - 1] == '/') n--;
    if (n == 0) return slang_string_new("/", 1);
    int64_t i = n;
    while (i > 0 && p[i - 1] != '/') i--;
    return slang_string_new(p + i, n - i);
}

slang_string *slang_fs_dir(const slang_string *path) {
    const char *p = sdata(path);
    int64_t i = slen(path);
    while (i > 0 && p[i - 1] != '/') i--;
    return clean(p, i);
}

slang_string *slang_fs_ext(const slang_string *path) {
    const char *p = sdata(path);
    int64_t n = slen(path);
    for (int64_t i = n - 1; i >= 0 && p[i] != '/'; i--)
        if (p[i] == '.') return slang_string_new(p + i, n - i);
    return slang__string_alloc(0);
}

slang_string *slang_fs_abs(const slang_string *path, slang_error **err) {
    int64_t n = slen(path);
    if (n > 0 && sdata(path)[0] == '/') return clean(sdata(path), n);
    char *cwd = getcwd(NULL, 0);
    if (!cwd) { set_err(err, "getwd", sdata(path), errno); return NULL; }
    size_t cl = strlen(cwd);
    char *buf = slang_alloc((int64_t)cl + n + 2);
    memcpy(buf, cwd, cl);
    buf[cl] = '/';
    memcpy(buf + cl + 1, sdata(path), (size_t)n);
    free(cwd);
    slang_string *r = clean(buf, (int64_t)cl + 1 + n);
    slang_free(buf);
    return r;
}

slang_string *slang_fs_temp_dir(void) {
    const char *t = getenv("TMPDIR");
    return slang_string_from_cstr(t && *t ? t : "/tmp");
}
