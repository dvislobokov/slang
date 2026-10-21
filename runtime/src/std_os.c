/* std_os.c - std.os (slang_os_*). Args is slang_os_args (main.c). Failing
 * calls report "<op> <arg>: <strerror>" errors through the `T!` out-param. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(slang_error **err, const char *op, const char *arg, int e) {
    if (!err) return;
    char buf[512];
    if (arg) snprintf(buf, sizeof buf, "%s %.400s: %s", op, arg, strerror(e));
    else snprintf(buf, sizeof buf, "%s: %s", op, strerror(e));
    *err = slang_error_new_cstr(buf);
}

/* Slang strings may contain NUL bytes; C APIs cannot take them. */
static bool has_nul(const slang_string *s) {
    return (int64_t)strlen(slang_string_data(s)) != slang_string_len(s);
}

slang_string *slang_os_getenv(const slang_string *name) {
    char *v = has_nul(name) ? NULL : slang__plat_getenv(slang_string_data(name));
    slang_string *r = slang_string_from_cstr(v ? v : "");
    free(v);
    return r;
}

void slang_os_setenv(const slang_string *name, const slang_string *value, slang_error **err) {
    const char *n = slang_string_data(name);
    if (slang_string_len(name) == 0 || strchr(n, '=') || has_nul(name) || has_nul(value)) {
        set_err(err, "setenv", n, EINVAL);
        return;
    }
    int e = slang__plat_setenv(n, slang_string_data(value));
    if (e) set_err(err, "setenv", n, e);
}

void slang_os_exit(int64_t code) { slang_exit((int32_t)code); }

slang_string *slang_os_hostname(slang_error **err) {
    char buf[300];
    int e = slang__plat_hostname(buf, sizeof buf);
    if (e) {
        set_err(err, "hostname", NULL, e);
        return NULL;
    }
    return slang_string_from_cstr(buf);
}

int64_t slang_os_pid(void) { return slang__plat_pid(); }

slang_string *slang_os_cwd(slang_error **err) {
    int e = 0;
    char *p = slang__plat_getcwd(&e);
    if (!p) {
        set_err(err, "getwd", NULL, e);
        return NULL;
    }
    slang_string *r = slang_string_from_cstr(p);
    free(p);
    return r;
}

void slang_os_chdir(const slang_string *path, slang_error **err) {
    if (has_nul(path)) { set_err(err, "chdir", slang_string_data(path), EINVAL); return; }
    int e = slang__plat_chdir(slang_string_data(path));
    if (e) set_err(err, "chdir", slang_string_data(path), e);
}
