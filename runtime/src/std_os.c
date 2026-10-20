/* std_os.c - std.os (slang_os_*). Args is slang_os_args (main.c). Failing
 * calls report "<op> <arg>: <strerror>" errors through the `T!` out-param. */
#include "rt_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    const char *v = has_nul(name) ? NULL : getenv(slang_string_data(name));
    return slang_string_from_cstr(v ? v : "");
}

void slang_os_setenv(const slang_string *name, const slang_string *value, slang_error **err) {
    const char *n = slang_string_data(name);
    if (slang_string_len(name) == 0 || strchr(n, '=') || has_nul(name) || has_nul(value)) {
        set_err(err, "setenv", n, EINVAL);
        return;
    }
    if (setenv(n, slang_string_data(value), 1) != 0) set_err(err, "setenv", n, errno);
}

void slang_os_exit(int64_t code) { slang_exit((int32_t)code); }

slang_string *slang_os_hostname(slang_error **err) {
    char buf[HOST_NAME_MAX + 2];
    if (gethostname(buf, sizeof buf) != 0) {
        set_err(err, "hostname", NULL, errno);
        return NULL;
    }
    buf[sizeof buf - 1] = 0;
    return slang_string_from_cstr(buf);
}

int64_t slang_os_pid(void) { return (int64_t)getpid(); }

slang_string *slang_os_cwd(slang_error **err) {
    char *p = getcwd(NULL, 0);
    if (!p) {
        set_err(err, "getwd", NULL, errno);
        return NULL;
    }
    slang_string *r = slang_string_from_cstr(p);
    free(p);
    return r;
}

void slang_os_chdir(const slang_string *path, slang_error **err) {
    if (has_nul(path)) { set_err(err, "chdir", slang_string_data(path), EINVAL); return; }
    if (chdir(slang_string_data(path)) != 0) set_err(err, "chdir", slang_string_data(path), errno);
}
