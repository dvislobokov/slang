/* fs_win32.c - Windows files and directories for the platform layer
 * (platform.h). Paths are UTF-8 and converted to UTF-16 here ('/' and '\\'
 * both separate); files are opened with CreateFileW and wrapped in C
 * runtime descriptors (_open_osfhandle) so 0/1/2 stay the standard streams
 * and descriptor numbers stay small. Errors are POSIX errno values
 * (zell__w32_errno). */
#include "platform.h"

#if defined(_WIN32)

#include "win32_util.h"

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK 0xA000000CL
#endif

static int last_errno(void) { return zell__w32_errno(GetLastError()); }

static HANDLE os_handle(int64_t fd) { return (HANDLE)_get_osfhandle((int)fd); }

int64_t zell__plat_open(const char *path, int flags, int perm) {
    wchar_t *w = zell__w32_wpath(path);
    if (!w) return -EINVAL;
    DWORD access = 0;
    int acc = flags & 3;
    if (acc == ZELL_PLAT_O_RDONLY || acc == ZELL_PLAT_O_RDWR) access |= GENERIC_READ;
    if (acc == ZELL_PLAT_O_WRONLY || acc == ZELL_PLAT_O_RDWR) {
        /* O_APPEND: append-only access makes every write go to the end atomically */
        access |= (flags & ZELL_PLAT_O_APPEND) ? (FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE) : GENERIC_WRITE;
    }
    bool create = flags & ZELL_PLAT_O_CREATE, excl = flags & ZELL_PLAT_O_EXCL, trunc = flags & ZELL_PLAT_O_TRUNC;
    DWORD disp = create ? (excl ? CREATE_NEW : trunc ? CREATE_ALWAYS : OPEN_ALWAYS) : (trunc ? TRUNCATE_EXISTING : OPEN_EXISTING);
    DWORD attrs = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS;  /* directories open too, like POSIX O_RDONLY */
    if (create && !(perm & 0200)) attrs |= FILE_ATTRIBUTE_READONLY;
    if (flags & ZELL_PLAT_O_SYNC) attrs |= FILE_FLAG_WRITE_THROUGH;
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp, attrs, NULL);
    DWORD err = GetLastError();
    if (h == INVALID_HANDLE_VALUE && err == ERROR_ACCESS_DENIED && (access & (GENERIC_WRITE | FILE_APPEND_DATA))) {
        DWORD a = GetFileAttributesW(w);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) err = ERROR_DIRECTORY_NOT_SUPPORTED;
    }
    free(w);
    if (h == INVALID_HANDLE_VALUE) return err == ERROR_DIRECTORY_NOT_SUPPORTED ? -EISDIR : -(int64_t)zell__w32_errno(err);
    int cf = _O_BINARY | (acc == ZELL_PLAT_O_RDONLY ? _O_RDONLY : acc == ZELL_PLAT_O_WRONLY ? _O_WRONLY : _O_RDWR);
    if (flags & ZELL_PLAT_O_APPEND) cf |= _O_APPEND;
    int fd = _open_osfhandle((intptr_t)h, cf);
    if (fd < 0) {
        CloseHandle(h);
        return -EMFILE;
    }
    return fd;
}

static unsigned clamp(int64_t n) { return n > 0x40000000 ? 0x40000000u : (unsigned)n; }

int64_t zell__plat_read(int64_t fd, void *buf, int64_t n) {
    int r = _read((int)fd, buf, clamp(n));
    return r < 0 ? -(int64_t)errno : r;
}

int64_t zell__plat_pread(int64_t fd, void *buf, int64_t n, int64_t off) {
    HANDLE h = os_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -EBADF;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)(uint64_t)off;
    ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    /* ReadFile with an offset moves the file pointer of a synchronous
     * handle; POSIX pread does not, so it is put back (not atomic with
     * respect to other threads using the same descriptor's position). */
    int64_t pos = _lseeki64((int)fd, 0, SEEK_CUR);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, clamp(n), &got, &ov);
    DWORD e = ok ? 0 : GetLastError();
    if (pos >= 0) _lseeki64((int)fd, pos, SEEK_SET);
    if (!ok) {
        if (e == ERROR_HANDLE_EOF) return 0;
        return -(int64_t)zell__w32_errno(e);
    }
    return got;
}

int64_t zell__plat_write(int64_t fd, const void *buf, int64_t n) {
    if (zell__w32_is_console_fd(fd)) {
        zell__w32_console_fd_write(fd, buf, (size_t)n);
        return n;
    }
    int r = _write((int)fd, buf, clamp(n));
    return r < 0 ? -(int64_t)errno : r;
}

int zell__plat_write_all(int64_t fd, const void *buf, int64_t n) {
    const char *p = buf;
    while (n > 0) {
        int64_t w = zell__plat_write(fd, p, n);
        if (w < 0) return (int)-w;
        if (w == 0) return EIO;
        p += w;
        n -= w;
    }
    return 0;
}

int64_t zell__plat_seek(int64_t fd, int64_t off, int whence) {
    int w = whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET;
    int64_t r = _lseeki64((int)fd, off, w);
    return r < 0 ? -(int64_t)errno : r;
}

int zell__plat_close(int64_t fd) { return _close((int)fd) == 0 ? 0 : errno; }

int zell__plat_fsync(int64_t fd) {
    HANDLE h = os_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return EBADF;
    if (!FlushFileBuffers(h)) {
        DWORD e = GetLastError();
        return e == ERROR_INVALID_HANDLE ? EINVAL : zell__w32_errno(e);  /* consoles, pipes */
    }
    return 0;
}

int zell__plat_ftruncate(int64_t fd, int64_t size) { return _chsize_s((int)fd, size) == 0 ? 0 : errno; }

static int64_t ft_ns(FILETIME ft) {
    int64_t t = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    return (t - 116444736000000000LL) * 100;
}

static void fill(zell__plat_stat *o, DWORD attrs, DWORD size_hi, DWORD size_lo, FILETIME mtime, bool link) {
    o->size = (int64_t)(((uint64_t)size_hi << 32) | size_lo);
    int64_t ns = ft_ns(mtime);
    o->mtime_sec = ns / 1000000000 - (ns % 1000000000 < 0);
    o->mtime_nsec = (int32_t)(ns - o->mtime_sec * 1000000000);
    /* Go's convention: 0666 / 0444 (read-only), directories 0777 */
    bool dir = attrs & FILE_ATTRIBUTE_DIRECTORY;
    o->perm = dir ? 0777 : (attrs & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666;
    o->kind = link ? ZELL_PLAT_KIND_LINK : dir ? ZELL_PLAT_KIND_DIR : ZELL_PLAT_KIND_FILE;
    if (link) o->size = 0;
}

int zell__plat_fstat(int64_t fd, zell__plat_stat *o) {
    HANDLE h = os_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return EBADF;
    memset(o, 0, sizeof *o);
    DWORD type = GetFileType(h);
    if (type != FILE_TYPE_DISK) {
        o->kind = ZELL_PLAT_KIND_OTHER;
        o->perm = 0666;
        return 0;
    }
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi)) return last_errno();
    fill(o, fi.dwFileAttributes, fi.nFileSizeHigh, fi.nFileSizeLow, fi.ftLastWriteTime, false);
    return 0;
}

static bool is_symlink(const wchar_t *w, DWORD attrs) {
    if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(w, &fd);
    if (f == INVALID_HANDLE_VALUE) return false;
    FindClose(f);
    return fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
}

int zell__plat_stat_path(const char *path, bool follow, zell__plat_stat *o) {
    if (!*path) return ENOENT;
    wchar_t *w = zell__w32_wpath(path);
    if (!w) return EINVAL;
    memset(o, 0, sizeof *o);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &a)) {
        int e = last_errno();
        free(w);
        return e;
    }
    if (!follow || !(a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        fill(o, a.dwFileAttributes, a.nFileSizeHigh, a.nFileSizeLow, a.ftLastWriteTime, !follow && is_symlink(w, a.dwFileAttributes));
        free(w);
        return 0;
    }
    /* follow a symlink: open the target */
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return last_errno();
    BY_HANDLE_FILE_INFORMATION fi;
    BOOL ok = GetFileInformationByHandle(h, &fi);
    int e = ok ? 0 : last_errno();
    CloseHandle(h);
    if (ok) fill(o, fi.dwFileAttributes, fi.nFileSizeHigh, fi.nFileSizeLow, fi.ftLastWriteTime, false);
    return e;
}

bool zell__plat_isatty(int64_t fd) {
    HANDLE h = os_handle(fd);
    DWORD mode;
    return h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}

#define WCALL(path, call) do { \
        wchar_t *w = zell__w32_wpath(path); \
        if (!w) return EINVAL; \
        BOOL ok_ = (call); \
        int e_ = ok_ ? 0 : last_errno(); \
        free(w); \
        return e_; \
    } while (0)

int zell__plat_mkdir(const char *path) { WCALL(path, CreateDirectoryW(w, NULL)); }
int zell__plat_rmdir(const char *path) { WCALL(path, RemoveDirectoryW(w)); }
int zell__plat_chdir(const char *path) { WCALL(path, SetCurrentDirectoryW(w)); }

int zell__plat_unlink_file(const char *path) {
    wchar_t *w = zell__w32_wpath(path);
    if (!w) return EINVAL;
    int e = 0;
    if (!DeleteFileW(w)) {
        DWORD le = GetLastError();
        DWORD a = GetFileAttributesW(w);
        if (le == ERROR_ACCESS_DENIED && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
            /* POSIX unlinks read-only files (the directory decides) */
            SetFileAttributesW(w, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
            if (!DeleteFileW(w)) e = last_errno();
        } else if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            e = (a & FILE_ATTRIBUTE_REPARSE_POINT) && RemoveDirectoryW(w) ? 0 : EISDIR;  /* directory symlink */
        } else {
            e = zell__w32_errno(le);
        }
    }
    free(w);
    return e;
}

int zell__plat_remove(const char *path) {
    int e = zell__plat_unlink_file(path);
    if (e == EISDIR) e = zell__plat_rmdir(path);
    return e;
}

int zell__plat_rename(const char *from, const char *to) {
    wchar_t *a = zell__w32_wpath(from), *b = zell__w32_wpath(to);
    int e = 0;
    if (!a || !b) e = EINVAL;
    else if (!MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) e = last_errno();
    free(a);
    free(b);
    return e;
}

int zell__plat_chmod(const char *path, int mode) {
    wchar_t *w = zell__w32_wpath(path);
    if (!w) return EINVAL;
    DWORD a = GetFileAttributesW(w);
    int e = 0;
    if (a == INVALID_FILE_ATTRIBUTES) e = last_errno();
    else {
        DWORD n = (mode & 0200) ? (a & ~(DWORD)FILE_ATTRIBUTE_READONLY) : (a | FILE_ATTRIBUTE_READONLY);
        if (n != a && !SetFileAttributesW(w, n)) e = last_errno();
    }
    free(w);
    return e;
}

char *zell__plat_getcwd(int *err) {
    DWORD n = GetCurrentDirectoryW(0, NULL);
    wchar_t *w = n ? malloc((size_t)n * sizeof *w) : NULL;
    if (!w) {
        *err = n ? ENOMEM : last_errno();
        return NULL;
    }
    DWORD k = GetCurrentDirectoryW(n, w);
    char *r = k && k < n ? zell__w32_utf8(w, (int)k) : NULL;
    free(w);
    *err = r ? 0 : EIO;
    return r;
}

bool zell__plat_is_executable(const char *path) {
    if (!*path) return false;
    wchar_t *w = zell__w32_wpath(path);
    if (!w) return false;
    DWORD a = GetFileAttributesW(w);
    free(w);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return false;
    /* executability is by extension: %PATHEXT% (default .COM;.EXE;.BAT;.CMD) */
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '\\'), *fslash = strrchr(path, '/');
    if (!dot || (slash && slash > dot) || (fslash && fslash > dot)) return false;
    char *pe = zell__plat_getenv("PATHEXT");
    const char *list = pe && *pe ? pe : ".COM;.EXE;.BAT;.CMD";
    bool ok = false;
    size_t el = strlen(dot);
    for (const char *p = list; *p && !ok;) {
        const char *s = strchr(p, ';');
        size_t n = s ? (size_t)(s - p) : strlen(p);
        if (n == el && _strnicmp(p, dot, n) == 0) ok = true;
        p += n + (s ? 1 : 0);
    }
    free(pe);
    return ok;
}

struct zell__plat_dir {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    bool first, done;
    char *name;
};

zell__plat_dir *zell__plat_dir_open(const char *path, int *err) {
    size_t n = strlen(path);
    char *pat = malloc(n + 3);
    if (!pat) { *err = ENOMEM; return NULL; }
    memcpy(pat, path, n);
    if (n == 0 || (path[n - 1] != '/' && path[n - 1] != '\\')) pat[n++] = '\\';
    pat[n++] = '*';
    pat[n] = 0;
    wchar_t *w = zell__w32_wpath(pat);
    wchar_t *wd = zell__w32_wpath(*path ? path : ".");
    free(pat);
    if (!w || !wd) { free(w); free(wd); *err = EINVAL; return NULL; }
    DWORD a = GetFileAttributesW(wd);
    free(wd);
    if (a == INVALID_FILE_ATTRIBUTES) { free(w); *err = last_errno(); return NULL; }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { free(w); *err = ENOTDIR; return NULL; }
    zell__plat_dir *d = calloc(1, sizeof *d);
    if (!d) { free(w); *err = ENOMEM; return NULL; }
    d->h = FindFirstFileW(w, &d->fd);
    free(w);
    if (d->h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) { d->done = true; *err = 0; return d; }  /* empty (root of a drive) */
        free(d);
        *err = zell__w32_errno(e);
        return NULL;
    }
    d->first = true;
    *err = 0;
    return d;
}

const char *zell__plat_dir_next(zell__plat_dir *d, int *kind, int *err) {
    *err = 0;
    for (;;) {
        if (d->done) return NULL;
        if (!d->first) {
            if (!FindNextFileW(d->h, &d->fd)) {
                DWORD e = GetLastError();
                d->done = true;
                if (e != ERROR_NO_MORE_FILES) *err = zell__w32_errno(e);
                return NULL;
            }
        }
        d->first = false;
        const wchar_t *n = d->fd.cFileName;
        if ((n[0] == L'.' && n[1] == 0) || (n[0] == L'.' && n[1] == L'.' && n[2] == 0)) continue;
        free(d->name);
        d->name = zell__w32_utf8(n, -1);
        if (!d->name) continue;
        DWORD a = d->fd.dwFileAttributes;
        if ((a & FILE_ATTRIBUTE_REPARSE_POINT) && d->fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK) *kind = ZELL_PLAT_KIND_LINK;
        else if (a & FILE_ATTRIBUTE_DIRECTORY) *kind = ZELL_PLAT_KIND_DIR;
        else if (a & FILE_ATTRIBUTE_DEVICE) *kind = ZELL_PLAT_KIND_OTHER;
        else *kind = ZELL_PLAT_KIND_FILE;
        return d->name;
    }
}

void zell__plat_dir_close(zell__plat_dir *d) {
    if (!d) return;
    if (d->h && d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d->name);
    free(d);
}

#else
typedef int zell__fs_win32_unused;
#endif
