/* std_plugin.c - plugin: loading shared objects (runtime/ABI.md
 * section 22.7); the debug agent loads expressions compiled by
 * `zell serve` with it. Gated like unsafe (`plugin` capability). */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>

static zell_error *dl_err(const char *what, const zell_string *arg) {
    char buf[600];
    snprintf(buf, sizeof buf, "plugin: %s %s: %s", what, zell_string_cstr(arg), zell__plat_dl_error());
    return zell_error_new_cstr(buf);
}

int64_t zell_plugin_rt_open(const zell_string *path, zell_error **err) {
    void *h = zell__plat_dl_open(zell_string_cstr(path));
    if (!h) {
        *err = dl_err("open", path);
        return 0;
    }
    return (int64_t)(intptr_t)h;
}

int64_t zell_plugin_rt_sym(int64_t h, const zell_string *name, zell_error **err) {
    void *p = zell__plat_dl_sym((void *)(intptr_t)h, zell_string_cstr(name));
    if (!p) {
        *err = dl_err("symbol", name);
        return 0;
    }
    return (int64_t)(intptr_t)p;
}

void zell_plugin_rt_close(int64_t h) { zell__plat_dl_close((void *)(intptr_t)h); }
