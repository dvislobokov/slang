/* main.c - runtime entry points, process arguments, exit. */
#include "rt_internal.h"

#include <stdlib.h>

static slang_slice g_args;   /* []string, owned by the runtime */
static int g_initialized;

void slang_runtime_init(int argc, char **argv) {
    if (g_initialized) return;
    g_initialized = 1;
    slang_slice_new_ops(&g_args, (int64_t)sizeof(slang_string *), 0, argc > 0 ? argc : 0,
                        &slang_ops_ref);
    for (int i = 0; i < argc; i++) {
        slang_string *s = slang_string_from_cstr(argv[i]);
        slang_slice_append(&g_args, (int64_t)sizeof(slang_string *), &slang_ops_ref, &s);
        slang_release(s);
    }
    slang__sched_start();
}

void slang_runtime_drain(void) {
    slang__sched_stop();
}

void slang_runtime_shutdown(void) {
    slang__sched_stop();
    slang__io_flush_all();
    slang_slice_release(&g_args);
    g_initialized = 0;
    slang__leak_report();
}

void slang_os_args(slang_slice *out) {
    slang_slice_retain(&g_args);
    *out = g_args;
}

void slang_exit(int32_t code) {
    slang__io_flush_all();
    _Exit(code);
}
