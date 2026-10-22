/* std_os_user.c - os.user: the passwd/group databases through the
 * platform layer (getpwuid_r & co. on POSIX, so NSS sources work too; the
 * current user only on Windows). Results are [uid, gid, username, name,
 * home] / [gid, name] string slices; not found -> error. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zell_os_user_rt_lookup(const zell_string *name, int64_t uid, bool by_id, zell_slice *out, zell_error **err);
void zell_os_user_rt_group(const zell_string *name, int64_t gid, bool by_id, zell_slice *out, zell_error **err);
static void push(zell_slice *out, const char *s) {
    zell_string *x = zell_string_from_cstr(s ? s : "");
    zell_slice_append(out, 8, &zell_ops_ref, &x);
    zell_release(x);
}

static void push_int(zell_slice *out, int64_t v) {
    char b[32];
    snprintf(b, sizeof b, "%lld", (long long)v);
    push(out, b);
}

int64_t zell_os_user_rt_uid(void) { return zell__plat_uid(); }

void zell_os_user_rt_lookup(const zell_string *name, int64_t uid, bool by_id, zell_slice *out, zell_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    zell__plat_user u;
    int e = zell__plat_user_lookup(zell_string_cstr(name), uid, by_id, &u);
    if (e) {
        char m[512];
        if (e > 0) snprintf(m, sizeof m, "user: lookup failed: %s", strerror(e));
        else if (by_id) snprintf(m, sizeof m, "user: unknown userid %lld", (long long)uid);
        else snprintf(m, sizeof m, "user: unknown user %.400s", zell_string_cstr(name));
        if (err) *err = zell_error_new_cstr(m);
        return;
    }
    zell_slice_new_ops(out, 8, 0, 5, &zell_ops_ref);
    push_int(out, u.uid);
    push_int(out, u.gid);
    push(out, u.name);
    push(out, u.full);
    push(out, u.home);
}

void zell_os_user_rt_group(const zell_string *name, int64_t gid, bool by_id, zell_slice *out, zell_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    int64_t g = 0;
    char gname[256];
    int e = zell__plat_group_lookup(zell_string_cstr(name), gid, by_id, &g, gname, sizeof gname);
    if (e) {
        char m[512];
        if (e > 0) snprintf(m, sizeof m, "user: lookup failed: %s", strerror(e));
        else if (by_id) snprintf(m, sizeof m, "group: unknown groupid %lld", (long long)gid);
        else snprintf(m, sizeof m, "group: unknown group %.400s", zell_string_cstr(name));
        if (err) *err = zell_error_new_cstr(m);
        return;
    }
    zell_slice_new_ops(out, 8, 0, 2, &zell_ops_ref);
    push_int(out, g);
    push(out, gname);
}
