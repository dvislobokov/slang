/* std_os_user.c - std.os.user: the passwd/group databases (getpwuid_r &
 * co., so NSS sources work too). Results are [uid, gid, username, name,
 * home] / [gid, name] string slices; not found -> error. */
#include "rt_internal.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void slang_os_user_rt_lookup(const slang_string *name, int64_t uid, bool by_id, slang_slice *out, slang_error **err);
void slang_os_user_rt_group(const slang_string *name, int64_t gid, bool by_id, slang_slice *out, slang_error **err);
int64_t slang_os_user_rt_uid(void);

static void push(slang_slice *out, const char *s) {
    slang_string *x = slang_string_from_cstr(s ? s : "");
    slang_slice_append(out, 8, &slang_ops_ref, &x);
    slang_release(x);
}

static void push_int(slang_slice *out, int64_t v) {
    char b[32];
    snprintf(b, sizeof b, "%lld", (long long)v);
    push(out, b);
}

int64_t slang_os_user_rt_uid(void) { return (int64_t)getuid(); }

void slang_os_user_rt_lookup(const slang_string *name, int64_t uid, bool by_id, slang_slice *out, slang_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    struct passwd pw, *res = NULL;
    char buf[16384];
    int e = by_id ? getpwuid_r((uid_t)uid, &pw, buf, sizeof buf, &res)
                  : getpwnam_r(name ? name->data : "", &pw, buf, sizeof buf, &res);
    if (!res) {
        char m[512];
        if (e) snprintf(m, sizeof m, "user: lookup failed: %s", strerror(e));
        else if (by_id) snprintf(m, sizeof m, "user: unknown userid %lld", (long long)uid);
        else snprintf(m, sizeof m, "user: unknown user %.400s", name ? name->data : "");
        if (err) *err = slang_error_new_cstr(m);
        return;
    }
    slang_slice_new_ops(out, 8, 0, 5, &slang_ops_ref);
    push_int(out, pw.pw_uid);
    push_int(out, pw.pw_gid);
    push(out, pw.pw_name);
    /* GECOS: the full name is the first comma-separated field */
    char gecos[1024];
    snprintf(gecos, sizeof gecos, "%s", pw.pw_gecos ? pw.pw_gecos : "");
    char *comma = strchr(gecos, ',');
    if (comma) *comma = 0;
    push(out, gecos);
    push(out, pw.pw_dir);
}

void slang_os_user_rt_group(const slang_string *name, int64_t gid, bool by_id, slang_slice *out, slang_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    struct group gr, *res = NULL;
    char buf[16384];
    int e = by_id ? getgrgid_r((gid_t)gid, &gr, buf, sizeof buf, &res)
                  : getgrnam_r(name ? name->data : "", &gr, buf, sizeof buf, &res);
    if (!res) {
        char m[512];
        if (e) snprintf(m, sizeof m, "user: lookup failed: %s", strerror(e));
        else if (by_id) snprintf(m, sizeof m, "group: unknown groupid %lld", (long long)gid);
        else snprintf(m, sizeof m, "group: unknown group %.400s", name ? name->data : "");
        if (err) *err = slang_error_new_cstr(m);
        return;
    }
    slang_slice_new_ops(out, 8, 0, 2, &slang_ops_ref);
    push_int(out, gr.gr_gid);
    push(out, gr.gr_name);
}
