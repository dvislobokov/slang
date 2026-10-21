/* main.c - runtime entry points, process arguments, exit. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdlib.h>

static slang_slice g_args;   /* []string, owned by the runtime, built on first use */
static int g_argc;
static char **g_argv;
static int g_initialized;
static slang__plat_mutex g_args_mu = SLANG_PLAT_MUTEX_INIT;
static void (*g_args_free)(void);   /* set once g_args exists (shutdown releases it) */

void slang_runtime_init(int argc, char **argv) {
    if (g_initialized) return;
    g_initialized = 1;
    slang__plat_runtime_init(&argc, &argv);  /* Windows: UTF-8 argv, console */
    g_argc = argc;
    g_argv = argv;
    slang__process_init();   /* the scheduler starts with the first task (task.c) */
}

void slang_runtime_drain(void) {
    slang__sched_stop();
}

void slang_runtime_shutdown(void) {
    slang__sched_stop();
    slang__io_flush_all();
    if (g_args_free) g_args_free();
    slang__diag_shutdown();
    g_initialized = 0;
    slang__leak_report();
}

static void args_free(void) { slang_slice_release(&g_args); }

void slang_os_args(slang_slice *out) {
    slang__plat_mutex_lock(&g_args_mu);
    if (!g_args_free) {
        /* Built lazily: a program that never reads its arguments does not
         * allocate them (nor link the slice code for it). */
        int argc = g_argc;
        slang_slice_new_ops(&g_args, (int64_t)sizeof(slang_string *), 0, argc > 0 ? argc : 0, &slang_ops_ref);
        for (int i = 0; i < argc; i++) {
            slang_string *s = slang_string_from_cstr(g_argv[i]);
            slang_slice_append(&g_args, (int64_t)sizeof(slang_string *), &slang_ops_ref, &s);
            slang_release(s);
        }
        g_args_free = args_free;
    }
    slang_slice_retain(&g_args);
    *out = g_args;
    slang__plat_mutex_unlock(&g_args_mu);
}

void slang_exit(int32_t code) {
    slang_dbg_exit(code);
    slang__io_flush_all();
    _Exit(code);
}
