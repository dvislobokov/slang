/* diag_heap.c - live objects by type in production builds (std.diag
 * HeapByType, the /debug/heap handler; docs/FRAMEWORK.md section 5).
 *
 * With ZELL_DIAG_HEAP=1 in the environment (read once at startup) every
 * object allocation and free updates a per-thread counter of its type
 * info (an open-addressing table keyed by the type pointer, no atomics on
 * the shared path); a report sums the tables of all threads, and the
 * tables of exited threads are folded into a global one. Off (the
 * default), the only cost is one predictable branch in mem.c. Leak-check
 * builds with type tracking use the exact list of live objects instead. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t zell__diag_heap_on;

typedef struct { const zell_type_info *type; int64_t live, total; } tcount;

typedef struct ttable {
    tcount *v;                  /* the owner writes, reporters read (relaxed) */
    int64_t cap, n;
    struct ttable *next, *prev;
    int reg;
} ttable;

#if defined(__ELF__)
#define DH_TLS _Thread_local __attribute__((tls_model("initial-exec")))
#else
#define DH_TLS _Thread_local
#endif
static DH_TLS ttable t_tab;
static ttable g_head = { NULL, 0, 0, &g_head, &g_head, 1 };
static ttable g_dead;           /* exited threads (g_mu) */
static zell__plat_mutex g_mu = ZELL_PLAT_MUTEX_INIT;
static zell__plat_once g_once = ZELL_PLAT_ONCE_INIT;
static zell__plat_tls_key g_key;
static int g_keyed;

__attribute__((constructor)) static void diag_heap_init(void) {
    const char *e = getenv("ZELL_DIAG_HEAP");
    if (e && *e && *e != '0') zell__diag_heap_on = 1;
}

static inline uint64_t hash_ptr(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 29;
    return x;
}

/* The slot of `type` in t (inserted if absent); NULL on out of memory.
 * A resize swaps the array under g_mu unless the caller holds it. */
static tcount *slot(ttable *t, const zell_type_info *type, int locked) {
    if ((t->n + 1) * 2 > t->cap) {
        int64_t cap = t->cap ? t->cap * 2 : 256;
        tcount *v = calloc((size_t)cap, sizeof *v);
        if (!v) return NULL;
        for (int64_t i = 0; i < t->cap; i++) {
            if (!t->v[i].type) continue;
            uint64_t h = hash_ptr(t->v[i].type) & (uint64_t)(cap - 1);
            while (v[h].type) h = (h + 1) & (uint64_t)(cap - 1);
            v[h] = t->v[i];
        }
        /* Reporters hold g_mu while reading: swap under it. */
        if (!locked) zell__plat_mutex_lock(&g_mu);
        tcount *old = t->v;
        t->v = v;
        t->cap = cap;
        if (!locked) zell__plat_mutex_unlock(&g_mu);
        free(old);
    }
    uint64_t h = hash_ptr(type) & (uint64_t)(t->cap - 1);
    while (t->v[h].type && t->v[h].type != type) h = (h + 1) & (uint64_t)(t->cap - 1);
    if (!t->v[h].type) {
        __atomic_store_n(&t->v[h].type, type, __ATOMIC_RELEASE);
        t->n++;
    }
    return &t->v[h];
}

static void fold(ttable *into, ttable *from) {
    for (int64_t i = 0; i < from->cap; i++) {
        if (!from->v[i].type) continue;
        tcount *c = slot(into, from->v[i].type, 1);
        if (!c) return;
        c->live += from->v[i].live;
        c->total += from->v[i].total;
    }
}

static void thread_exit(void *arg) {
    ttable *t = arg;
    zell__plat_mutex_lock(&g_mu);
    t->prev->next = t->next;
    t->next->prev = t->prev;
    fold(&g_dead, t);   /* g_dead is written and read under g_mu only */
    zell__plat_mutex_unlock(&g_mu);
    free(t->v);
    t->v = NULL;
    t->cap = t->n = 0;
    t->reg = 0;
}

static void key_init(void) { g_keyed = zell__plat_tls_new(&g_key, thread_exit); }

__attribute__((noinline)) static void reg(void) {
    zell__plat_once_run(&g_once, key_init);
    if (g_keyed) zell__plat_tls_set(g_key, &t_tab);
    zell__plat_mutex_lock(&g_mu);
    t_tab.next = g_head.next;
    t_tab.prev = &g_head;
    g_head.next->prev = &t_tab;
    g_head.next = &t_tab;
    t_tab.reg = 1;
    zell__plat_mutex_unlock(&g_mu);
}

void zell__diag_heap_count(const zell_type_info *type, int64_t d) {
    if (__builtin_expect(!t_tab.reg, 0)) reg();
    tcount *c = slot(&t_tab, type, 0);
    if (!c) return;
    __atomic_store_n(&c->live, c->live + d, __ATOMIC_RELAXED);
    if (d > 0) __atomic_store_n(&c->total, c->total + d, __ATOMIC_RELAXED);
}

/* ---- report ---- */

typedef struct { const char *name; int64_t live, total, bytes; } row;
typedef struct { row *v; int64_t n, cap; } rows;

static void add_row(rows *r, const char *name, int64_t live, int64_t total, int64_t bytes) {
    for (int64_t i = 0; i < r->n; i++)
        if (strcmp(r->v[i].name, name) == 0) {
            r->v[i].live += live;
            r->v[i].total += total;
            r->v[i].bytes += bytes;
            return;
        }
    if (r->n == r->cap) {
        int64_t cap = r->cap ? r->cap * 2 : 64;
        row *v = realloc(r->v, (size_t)cap * sizeof *v);
        if (!v) return;
        r->v = v;
        r->cap = cap;
    }
    r->v[r->n++] = (row){ name, live, total, bytes };
}

static const char *tname(const zell_type_info *t) {
    return t && t->name ? t->name : "<untyped>";
}

static void add_table(rows *r, const ttable *t) {
    for (int64_t i = 0; i < t->cap; i++) {
        const zell_type_info *ty = __atomic_load_n(&t->v[i].type, __ATOMIC_ACQUIRE);
        if (!ty) continue;
        int64_t live = __atomic_load_n(&t->v[i].live, __ATOMIC_RELAXED);
        int64_t total = __atomic_load_n(&t->v[i].total, __ATOMIC_RELAXED);
        add_row(r, tname(ty), live, total, ty->size > 0 ? live * ty->size : 0);
    }
}

static void track_cb(const char *type, int64_t bytes, void *arg) { add_row(arg, type, 1, 0, bytes); }

static int cmp_row(const void *a, const void *b) {
    const row *x = a, *y = b;
    if (x->live != y->live) return x->live < y->live ? 1 : -1;
    return strcmp(x->name, y->name);
}

/* "live\ttotal\tbytes\ttype" lines, most live first; "" when neither the
 * counters (ZELL_DIAG_HEAP=1) nor leak tracking are on. `total` is -1
 * with leak tracking (it only sees live objects). */
zell_string *zell_diag_rt_heap_types(void) {
    rows r = { 0 };
    int tracked = zell__heap_each(track_cb, &r);
    if (!tracked && __atomic_load_n(&zell__diag_heap_on, __ATOMIC_RELAXED)) {
        zell__plat_mutex_lock(&g_mu);
        add_table(&r, &g_dead);
        for (ttable *t = g_head.next; t != &g_head; t = t->next) add_table(&r, t);
        zell__plat_mutex_unlock(&g_mu);
    }
    if (r.n) qsort(r.v, (size_t)r.n, sizeof *r.v, cmp_row);
    size_t cap = 64 + (size_t)r.n * 128, len = 0;
    char *s = malloc(cap);
    if (!s) {
        free(r.v);
        return NULL;
    }
    for (int64_t i = 0; i < r.n; i++) {
        if (r.v[i].live <= 0 && !tracked) continue;   /* freed by now (or before counting) */
        int w = snprintf(s + len, cap - len, "%lld\t%lld\t%lld\t%s\n", (long long)r.v[i].live,
                         (long long)(tracked ? -1 : r.v[i].total), (long long)r.v[i].bytes, r.v[i].name);
        if (w < 0 || (size_t)w >= cap - len) break;
        len += (size_t)w;
    }
    zell_string *out = len ? zell_string_new(s, (int64_t)len) : NULL;
    free(s);
    free(r.v);
    return out;
}

bool zell_diag_rt_heap_counting(void) {
    return __atomic_load_n(&zell__diag_heap_on, __ATOMIC_RELAXED) != 0;
}
