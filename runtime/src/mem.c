/* mem.c - raw allocation, refcounted object header, value ops, leak check. */
#include "rt_internal.h"

#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t g_live_objects;
static int64_t g_total_objects; /* objects ever allocated (std.diag) */

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

/* ---- small-object allocator ----
 * Objects of at most SLAB_MAX bytes (most strings, error and weak boxes,
 * small backing arrays) come from per-thread free lists instead of malloc.
 * Memory is carved from 4 MB regions, each dedicated to one size class
 * (multiples of 16 bytes); a two-level bitmap over the address space tells
 * whether a pointer lies in a region, so freeing needs no per-object header.
 * Freed slots go to the freeing thread's list; lists longer than SLAB_BATCH
 * slots and the lists of exiting threads are handed to a global per-class
 * pool so memory moves between producer and consumer threads. Regions are
 * never returned to the OS. Disabled under AddressSanitizer (so it still sees
 * every object), in leak-check mode 2 (objects carry a tracking prefix) and
 * with SLANG_NO_SLAB=1 in the environment (e.g. for Valgrind memcheck). */

#if defined(__SANITIZE_ADDRESS__)
#define SLAB_ENABLED 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SLAB_ENABLED 0
#endif
#endif
#ifndef SLAB_ENABLED
#define SLAB_ENABLED 1
#endif

#define SLAB_MAX 128
#define SLAB_CLASSES (SLAB_MAX / 16)
#define SLAB_REGION_SHIFT 22                 /* 4 MB regions */
#define SLAB_REGION ((size_t)1 << SLAB_REGION_SHIFT)
#define SLAB_SPAN ((size_t)64 << 10)         /* bump chunk handed to a thread */
#define SLAB_BATCH 4096                      /* local list length before spilling */
#define SLAB_ADDR_BITS 48
#define SLAB_LEAF_BITS 16                    /* regions per leaf (bits) */
#define SLAB_TOP (1u << (SLAB_ADDR_BITS - SLAB_REGION_SHIFT - SLAB_LEAF_BITS))

typedef struct slab_node { struct slab_node *next, *next_batch; } slab_node;

_Static_assert(sizeof(struct { int64_t a; void *b; }) <= 16, "region header fits a slot");
typedef struct slab_region {                 /* at the region base (first slot) */
    int64_t cls;
    struct slab_region *next;                /* all regions, so leak checkers see them as reachable */
} slab_region;

static uint64_t *g_slab_map[SLAB_TOP];       /* leaf bitmaps, set once */
static pthread_mutex_t g_slab_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char *cur, *end;                         /* uncarved part of the current region */
    slab_node *batches;                      /* spilled free lists (linked by next_batch) */
} g_slab[SLAB_CLASSES];
static slab_region *g_slab_regions;
static int g_slab_off;                       /* SLANG_NO_SLAB set, or no TLS key */
static pthread_once_t g_slab_once = PTHREAD_ONCE_INIT;
static pthread_key_t g_slab_key;

typedef struct {
    slab_node *free;                         /* free slots */
    int64_t nfree;
    char *cur, *end;                         /* thread's bump span */
    int registered;
} slab_local;
static _Thread_local slab_local t_slab[SLAB_CLASSES];

static inline int slab_is(const void *p) {
    uintptr_t a = (uintptr_t)p;
    if (a >> SLAB_ADDR_BITS) return 0;
    uintptr_t r = a >> SLAB_REGION_SHIFT;
    uint64_t *leaf = __atomic_load_n(&g_slab_map[r >> SLAB_LEAF_BITS], __ATOMIC_ACQUIRE);
    if (!leaf) return 0;
    uintptr_t bit = r & ((1u << SLAB_LEAF_BITS) - 1);
    return (int)((__atomic_load_n(&leaf[bit >> 6], __ATOMIC_RELAXED) >> (bit & 63)) & 1);
}

static inline int64_t slab_cls_of(const void *p) {
    return ((const slab_region *)((uintptr_t)p & ~(uintptr_t)(SLAB_REGION - 1)))->cls;
}

static inline int64_t slab_size(int64_t cls) { return (cls + 1) * 16; }

/* Flush a thread's free lists into the global pools (thread exit). */
static void slab_thread_exit(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_slab_mu);
    for (int c = 0; c < SLAB_CLASSES; c++) {
        slab_local *l = &t_slab[c];
        if (l->free) {
            l->free->next_batch = g_slab[c].batches;
            g_slab[c].batches = l->free;
        }
        l->free = NULL;
        l->nfree = 0;
        l->cur = l->end = NULL;  /* the rest of the span is abandoned */
    }
    pthread_mutex_unlock(&g_slab_mu);
}

static void slab_init_key(void) {
    const char *e = getenv("SLANG_NO_SLAB");
    if ((e && *e && *e != '0') || pthread_key_create(&g_slab_key, slab_thread_exit) != 0)
        __atomic_store_n(&g_slab_off, 1, __ATOMIC_RELAXED);
}

/* Marks [base, base + SLAB_REGION) as a slab region (g_slab_mu held). */
static int slab_mark(char *base) {
    uintptr_t r = (uintptr_t)base >> SLAB_REGION_SHIFT;
    if ((uintptr_t)base >> SLAB_ADDR_BITS) return 0;
    uint64_t **slot = &g_slab_map[r >> SLAB_LEAF_BITS];
    uint64_t *leaf = *slot;
    if (!leaf) {
        leaf = calloc((1u << SLAB_LEAF_BITS) / 64, sizeof(uint64_t));
        if (!leaf) return 0;
        __atomic_store_n(slot, leaf, __ATOMIC_RELEASE);
    }
    uintptr_t bit = r & ((1u << SLAB_LEAF_BITS) - 1);
    __atomic_or_fetch(&leaf[bit >> 6], (uint64_t)1 << (bit & 63), __ATOMIC_RELEASE);
    return 1;
}

/* Slow path: refill the thread's list or span for class c. NULL = use malloc. */
__attribute__((noinline)) static void *slab_refill(int64_t c) {
    slab_local *l = &t_slab[c];
    if (!l->registered) {
        pthread_once(&g_slab_once, slab_init_key);
        if (__atomic_load_n(&g_slab_off, __ATOMIC_RELAXED)) return NULL;
        pthread_setspecific(g_slab_key, (void *)1);
        l->registered = 1;
    }
    int64_t sz = slab_size(c);
    pthread_mutex_lock(&g_slab_mu);
    slab_node *batch = g_slab[c].batches;
    if (batch) {
        g_slab[c].batches = batch->next_batch;
        pthread_mutex_unlock(&g_slab_mu);
        l->free = batch->next;
        l->nfree = 0;  /* unknown length; only bounds spilling */
        return batch;
    }
    if (g_slab[c].end - g_slab[c].cur < (ptrdiff_t)SLAB_SPAN) {
        char *base = aligned_alloc(SLAB_REGION, SLAB_REGION);
        if (!base || !slab_mark(base)) {
            pthread_mutex_unlock(&g_slab_mu);
            free(base);
            return NULL;
        }
        slab_region *rg = (slab_region *)base;
        rg->cls = c;
        rg->next = g_slab_regions;
        g_slab_regions = rg;
        /* The first slot holds the region header (16 bytes, the smallest class). */
        g_slab[c].cur = base + sz;
        g_slab[c].end = base + SLAB_REGION;
    }
    char *span = g_slab[c].cur;
    size_t n = SLAB_SPAN - SLAB_SPAN % (size_t)sz;
    if ((size_t)(g_slab[c].end - span) < n) n = (size_t)(g_slab[c].end - span) / (size_t)sz * (size_t)sz;
    g_slab[c].cur = span + n;
    pthread_mutex_unlock(&g_slab_mu);
    l->cur = span + sz;
    l->end = span + n;
    return span;
}

/* A slot of the class for `size` bytes, contents undefined; NULL = use malloc. */
static inline void *slab_alloc(int64_t size) {
    int64_t c = (size - 1) >> 4;
    slab_local *l = &t_slab[c];
    slab_node *n = l->free;
    if (n) {
        l->free = n->next;
        l->nfree--;
        return n;
    }
    if (l->cur < l->end) {
        void *p = l->cur;
        l->cur += slab_size(c);
        return p;
    }
    return slab_refill(c);
}

static inline void slab_free(void *p) {
    int64_t c = slab_cls_of(p);
    slab_local *l = &t_slab[c];
    slab_node *n = p;
    if (l->nfree >= SLAB_BATCH) {
        /* Hand the whole list to the global pool and start a new one. */
        pthread_mutex_lock(&g_slab_mu);
        l->free->next_batch = g_slab[c].batches;
        g_slab[c].batches = l->free;
        pthread_mutex_unlock(&g_slab_mu);
        l->free = NULL;
        l->nfree = 0;
    }
    n->next = l->free;
    l->free = n;
    l->nfree++;
}

/* ---- objects ---- */

/* Allocates an object of `size` bytes (header included) with the header set
 * and the payload NOT zeroed. */
static inline slang_object *object_alloc(const slang_type_info *type, int64_t size) {
    if (size < (int64_t)sizeof(slang_object))
        slang__panicf("slang_object_new: size %lld smaller than header", (long long)size);
    slang_object *o = NULL;
    if (SLAB_ENABLED && size <= SLAB_MAX && !g_track &&
        !__atomic_load_n(&g_slab_off, __ATOMIC_RELAXED)) o = slab_alloc(size);
    if (!o) {
        if (g_track) {
            o = (slang_object *)((track *)slang_alloc(size + (int64_t)sizeof(track)) + 1);
            track_add(o);
        } else {
            o = malloc((size_t)size);
            if (!o) slang__panicf("out of memory (requested %lld bytes)", (long long)size);
        }
    }
    o->refcount = 1;
    o->type = type;
    __atomic_add_fetch(&g_live_objects, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_total_objects, 1, __ATOMIC_RELAXED);
    return o;
}

void *slang_object_new(const slang_type_info *type, int64_t size) {
    slang_object *o = object_alloc(type, size);
    memset(o + 1, 0, (size_t)size - sizeof(slang_object));
    return o;
}

slang_object *slang__object_new_uninit(const slang_type_info *type, int64_t size) {
    return object_alloc(type, size);
}

static void object_free(slang_object *o) {
    __atomic_sub_fetch(&g_live_objects, 1, __ATOMIC_RELAXED);
    if (SLAB_ENABLED && slab_is(o)) {
        slab_free(o);
    } else if (g_track) {
        track_remove(o);
        free((track *)o - 1);
    } else {
        free(o);
    }
}

int64_t slang__object_capacity(slang_object *o) {
    if (SLAB_ENABLED && slab_is(o)) return slab_size(slab_cls_of(o));
    if (g_track) return (int64_t)malloc_usable_size((track *)o - 1) - (int64_t)sizeof(track);
    return (int64_t)malloc_usable_size(o);
}

slang_object *slang__object_resize(slang_object *o, int64_t size) {
    if (SLAB_ENABLED && slab_is(o)) {
        int64_t have = slab_size(slab_cls_of(o));
        if (size <= have) return o;
        /* Move out of the slab (slab objects are never tracked). */
        slang_object *n = malloc((size_t)size);
        if (!n) slang__panicf("out of memory (requested %lld bytes)", (long long)size);
        memcpy(n, o, (size_t)have);
        slab_free(o);
        return n;
    }
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
    int64_t n = __atomic_load_n(&o->refcount, __ATOMIC_ACQUIRE);
    if (n < 0) return;
    if (n != 1) {
        /* Shared (or has a weak box): the decrement must be atomic. A count
         * of exactly 1 with no weak flag means the caller holds the only
         * reference and nobody else can retain it, so the RMW is skipped. */
        n = __atomic_sub_fetch(&o->refcount, 1, __ATOMIC_ACQ_REL);
        if ((n & SLANG_RC_COUNT_MASK) != 0) return;
        /* Weak references are cleared before the destructor runs. */
        if (n & SLANG_RC_WEAK_FLAG) slang__weak_clear(o);
    } else {
        o->refcount = 0;
    }
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

int64_t slang__total_objects(void) {
    return __atomic_load_n(&g_total_objects, __ATOMIC_RELAXED);
}

bool slang__heap_each(void (*cb)(const char *type, int64_t bytes, void *arg), void *arg) {
    if (!g_track) return false;
    pthread_mutex_lock(&g_track_mu);
    for (track *t = g_track_head.next; t != &g_track_head; t = t->next) {
        slang_object *o = (slang_object *)(t + 1);
        const char *name = (o->type && o->type->name) ? o->type->name : "<untyped>";
        int64_t bytes = (int64_t)malloc_usable_size(t) - (int64_t)sizeof(track);
        cb(name, bytes, arg);
    }
    pthread_mutex_unlock(&g_track_mu);
    return true;
}

void slang__live_each(void (*cb)(slang_object *obj, void *arg), void *arg) {
    if (!g_track) return;
    pthread_mutex_lock(&g_track_mu);
    for (track *t = g_track_head.next; t != &g_track_head; t = t->next) cb((slang_object *)(t + 1), arg);
    pthread_mutex_unlock(&g_track_mu);
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
        slang__leak_cycles();
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
