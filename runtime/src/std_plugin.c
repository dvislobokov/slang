/* std_plugin.c - std.plugin: loading shared objects (runtime/ABI.md
 * section 22.7); the debug agent loads expressions compiled by
 * `slang serve` with it. Gated like std.unsafe (`plugin` capability). */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>

static slang_error *dl_err(const char *what, const slang_string *arg) {
    char buf[600];
    snprintf(buf, sizeof buf, "plugin: %s %s: %s", what, slang_string_cstr(arg), slang__plat_dl_error());
    return slang_error_new_cstr(buf);
}

int64_t slang_plugin_rt_open(const slang_string *path, slang_error **err) {
    void *h = slang__plat_dl_open(slang_string_cstr(path));
    if (!h) {
        *err = dl_err("open", path);
        return 0;
    }
    return (int64_t)(intptr_t)h;
}

int64_t slang_plugin_rt_sym(int64_t h, const slang_string *name, slang_error **err) {
    void *p = slang__plat_dl_sym((void *)(intptr_t)h, slang_string_cstr(name));
    if (!p) {
        *err = dl_err("symbol", name);
        return 0;
    }
    return (int64_t)(intptr_t)p;
}

void slang_plugin_rt_close(int64_t h) { slang__plat_dl_close((void *)(intptr_t)h); }
