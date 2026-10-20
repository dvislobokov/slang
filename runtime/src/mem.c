/* mem.c - raw allocation, refcounted object header, value ops, leak check. */
#include "rt_internal.h"

#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t g_live_objects;

/* ---- leak tracking ----
 * Mode 0: off. Mode 1: count live objects and report the count at shutdown.
 * Mode 2: additionally keep every live object in a doubly linked list (a
 * 16-byte prefix in front of the header) so the report can name the types. */
typedef struct track {
    struct track *prev, *next;
} track;

static int g_leak_mode;
static int g_track; /* prefix in use (fixed before the first allocation) */
static track g_track_head = { &g_track_head, &g_track_head };
static pthread_mutex_t g_track_mu = PTHREAD_MUTEX_INITIALIZER;

void slang_leak_check_enable(int32_t mode) {
    g_leak_mode = mode;
    /* The prefix can only be used if no object exists yet. */
    if (mode >= 2 && __atomic_load_n(&g_live_objects, __ATOMIC_RELAXED) == 0) g_track = 1;
}

static void track_add(slang_object *o) {
    track *t = (track *)o - 1;
    pthread_mutex_lock(&g_track_mu);
    t->next = g_track_head.next;
    t->prev = &g_track_head;
    g_track_head.next->prev = t;
    g_track_head.next = t;
    pthread_mutex_unlock(&g_track_mu);
}

static void track_remove(slang_object *o) {
    track *t = (track *)o - 1;
    pthread_mutex_lock(&g_track_mu);
    t->prev->next = t->next;
    t->next->prev = t->prev;
    t->prev = t->next = t;
    pthread_mutex_unlock(&g_track_mu);
}

/* ---- raw memory ---- */

void *slang_alloc(int64_t size) {
    if (size < 0) slang__panicf("slang_alloc: negative size %lld", (long long)size);
    void *p = calloc(1, size == 0 ? 1 : (size_t)size);
    if (!p) slang__panicf("out of memory (requested %lld bytes)", (long long)size);
    return p;
}

void *slang_realloc(void *p, int64_t size) {
    if (size < 0) slang__panicf("slang_realloc: negative size %lld", (long long)size);
    void *q = realloc(p, size == 0 ? 1 : (size_t)size);
    if (!q) slang__panicf("out of memory (requested %lld bytes)", (long long)size);
    return q;
}

void slang_free(void *p) { free(p); }

/* ---- objects ---- */

void *slang_object_new(const slang_type_info *type, int64_t size) {
    if (size < (int64_t)sizeof(slang_object))
        slang__panicf("slang_object_new: size %lld smaller than header", (long long)size);
    slang_object *o;
    if (g_track) {
        o = (slang_object *)((track *)slang_alloc(size + (int64_t)sizeof(track)) + 1);
        track_add(o);
    } else {
        o = slang_alloc(size);
    }
    o->refcount = 1;
    o->type = type;
    __atomic_add_fetch(&g_live_objects, 1, __ATOMIC_RELAXED);
    return o;
}

static void object_free(slang_object *o) {
    __atomic_sub_fetch(&g_live_objects, 1, __ATOMIC_RELAXED);
    if (g_track) {
        track_remove(o);
        free((track *)o - 1);
    } else {
        free(o);
    }
}

int64_t slang__object_capacity(slang_object *o) {
    if (g_track) return (int64_t)malloc_usable_size((track *)o - 1) - (int64_t)sizeof(track);
    return (int64_t)malloc_usable_size(o);
}

slang_object *slang__object_resize(slang_object *o, int64_t size) {
    if (!g_track) return slang_realloc(o, size);
    pthread_mutex_lock(&g_track_mu);
    track *t = (track *)o - 1;
    track *n = realloc(t, (size_t)size + sizeof(track));
    if (!n) {
        pthread_mutex_unlock(&g_track_mu);
        slang__panicf("out of memory (requested %lld bytes)", (long long)size);
    }
    n->prev->next = n;
    n->next->prev = n;
    pthread_mutex_unlock(&g_track_mu);
    return (slang_object *)(n + 1);
}

void slang__object_immortalize(slang_object *o) {
    if (o->refcount < 0) return;
    o->refcount = SLANG_RC_IMMORTAL;
    __atomic_sub_fetch(&g_live_objects, 1, __ATOMIC_RELAXED);
    if (g_track) track_remove(o);
}

void slang_retain(void *obj) {
    if (!obj) return;
    slang_object *o = obj;
    if (__atomic_load_n(&o->refcount, __ATOMIC_RELAXED) < 0) return;
    __atomic_add_fetch(&o->refcount, 1, __ATOMIC_RELAXED);
}

void slang_release(void *obj) {
    if (!obj) return;
    slang_object *o = obj;
    if (__atomic_load_n(&o->refcount, __ATOMIC_RELAXED) < 0) return;
    int64_t n = __atomic_sub_fetch(&o->refcount, 1, __ATOMIC_ACQ_REL);
    if ((n & SLANG_RC_COUNT_MASK) != 0) return;
    /* Weak references are cleared before the destructor runs. */
    if (n & SLANG_RC_WEAK_FLAG) slang__weak_clear(o);
    if (o->type && o->type->drop) o->type->drop(o);
    object_free(o);
}

int64_t slang_refcount(const void *obj) {
    if (!obj) return 0;
    int64_t rc = __atomic_load_n(&((const slang_object *)obj)->refcount, __ATOMIC_RELAXED);
    return rc < 0 ? rc : (rc & SLANG_RC_COUNT_MASK);
}

int64_t slang_live_objects(void) {
    return __atomic_load_n(&g_live_objects, __ATOMIC_RELAXED);
}

/* ---- pointers to payloads (`*T` values) ---- */

void slang_ptr_retain(void *payload) {
    if (payload) slang_retain((char *)payload - sizeof(slang_object));
}

void slang_ptr_release(void *payload) {
    if (payload) slang_release((char *)payload - sizeof(slang_object));
}

/* ---- leak report ---- */

typedef struct { const char *name; int64_t n; } type_count;

static int cmp_count(const void *a, const void *b) {
    const type_count *x = a, *y = b;
    if (x->n != y->n) return x->n < y->n ? 1 : -1;
    return strcmp(x->name, y->name);
}

void slang__leak_report(void) {
    if (!g_leak_mode) return;
    int64_t live = slang_live_objects();
    if (live <= 0) return;
    fflush(stdout);
    fprintf(stderr, "leak: %lld objects\n", (long long)live);
    if (g_track) {
        pthread_mutex_lock(&g_track_mu);
        int64_t cap = 16, len = 0;
        type_count *counts = calloc((size_t)cap, sizeof *counts);
        for (track *t = g_track_head.next; t != &g_track_head && counts; t = t->next) {
            slang_object *o = (slang_object *)(t + 1);
            const char *name = (o->type && o->type->name) ? o->type->name : "<untyped>";
            int64_t i = 0;
            while (i < len && strcmp(counts[i].name, name) != 0) i++;
            if (i == len) {
                if (len == cap) {
                    type_count *nc = realloc(counts, (size_t)(cap * 2) * sizeof *counts);
                    if (!nc) break;
                    counts = nc;
                    cap *= 2;
                }
                counts[len].name = name;
                counts[len].n = 0;
                len++;
            }
            counts[i].n++;
        }
        pthread_mutex_unlock(&g_track_mu);
        if (counts) {
            qsort(counts, (size_t)len, sizeof *counts, cmp_count);
            for (int64_t i = 0; i < len; i++)
                fprintf(stderr, "  %lld x %s\n", (long long)counts[i].n, counts[i].name);
            free(counts);
        }
    }
    fflush(stderr);
    _Exit(SLANG_LEAK_EXIT_CODE);
}

/* ---- value ops ---- */

static void ops_ref_retain(void *slot) { slang_retain(*(void **)slot); }
static void ops_ref_release(void *slot) { slang_release(*(void **)slot); }
const slang_value_ops slang_ops_ref = { ops_ref_retain, ops_ref_release };

static void ops_ptr_retain(void *slot) { slang_ptr_retain(*(void **)slot); }
static void ops_ptr_release(void *slot) { slang_ptr_release(*(void **)slot); }
const slang_value_ops slang_ops_ptr = { ops_ptr_retain, ops_ptr_release };

static void ops_slice_retain(void *slot) { slang_slice_retain((slang_slice *)slot); }
static void ops_slice_release(void *slot) { slang_slice_release((slang_slice *)slot); }
const slang_value_ops slang_ops_slice = { ops_slice_retain, ops_slice_release };
