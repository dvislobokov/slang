/* weak.c - weak references (`weak *T`).
 *
 * A weak reference is a small refcounted *weak box* {hdr, target} shared by
 * all weak references to the same object. Boxes are found through a global
 * side table (object header -> box), created lazily by zell_weak_new, and
 * the object's refcount word carries ZELL_RC_WEAK_FLAG while a box exists.
 * When the object's count reaches zero, zell_release clears box->target
 * (under the table lock) before running the destructor, so later reads of
 * the weak reference yield NULL. When the last weak reference goes away the
 * box removes itself from the table and clears the flag. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdint.h>

typedef struct weak_box {
    zell_object hdr;
    zell_object *target; /* NULL once the target was freed */
} weak_box;

typedef struct entry {
    zell_object *obj;
    weak_box *box;
    struct entry *next;
} entry;

static zell__plat_mutex g_mu = ZELL_PLAT_MUTEX_INIT;
static entry **g_buckets;
static int64_t g_nbuckets, g_count;

static uint64_t hash_ptr(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}

static entry **find(zell_object *o) {
    if (!g_nbuckets) return NULL;
    entry **pp = &g_buckets[hash_ptr(o) & (uint64_t)(g_nbuckets - 1)];
    while (*pp && (*pp)->obj != o) pp = &(*pp)->next;
    return pp;
}

static void insert(zell_object *o, weak_box *b) {
    if (g_count >= g_nbuckets) {
        int64_t nb = g_nbuckets ? g_nbuckets * 2 : 64;
        entry **nbk = zell_alloc(nb * (int64_t)sizeof(entry *));
        for (int64_t i = 0; i < g_nbuckets; i++) {
            entry *e = g_buckets[i];
            while (e) {
                entry *n = e->next;
                uint64_t h = hash_ptr(e->obj) & (uint64_t)(nb - 1);
                e->next = nbk[h];
                nbk[h] = e;
                e = n;
            }
        }
        zell_free(g_buckets);
        g_buckets = nbk;
        g_nbuckets = nb;
    }
    entry *e = zell_alloc((int64_t)sizeof(entry));
    e->obj = o;
    e->box = b;
    uint64_t h = hash_ptr(o) & (uint64_t)(g_nbuckets - 1);
    e->next = g_buckets[h];
    g_buckets[h] = e;
    g_count++;
}

static void remove_at(entry **pp) {
    entry *e = *pp;
    *pp = e->next;
    zell_free(e);
    g_count--;
    if (g_count == 0) {
        zell_free(g_buckets);
        g_buckets = NULL;
        g_nbuckets = 0;
    }
}

static void weak_box_drop(void *obj) {
    weak_box *b = obj;
    zell__plat_mutex_lock(&g_mu);
    if (b->target) {
        entry **pp = find(b->target);
        if (pp && *pp) remove_at(pp);
        __atomic_and_fetch(&b->target->refcount, ~ZELL_RC_WEAK_FLAG, __ATOMIC_ACQ_REL);
        b->target = NULL;
    }
    zell__plat_mutex_unlock(&g_mu);
}

static const zell_type_info weak_box_type = { "weak", (int64_t)sizeof(weak_box), weak_box_drop };

void *zell_weak_new(void *payload) {
    if (!payload) return NULL;
    zell_object *o = (zell_object *)((char *)payload - sizeof(zell_object));
    if (__atomic_load_n(&o->refcount, __ATOMIC_RELAXED) < 0)
        zell__panicf("weak reference to an immortal object");
    zell__plat_mutex_lock(&g_mu);
    weak_box *b = NULL;
    entry **pp = find(o);
    if (pp && *pp) {
        b = (*pp)->box;
        zell_retain(b);
    } else {
        b = zell_object_new(&weak_box_type, (int64_t)sizeof(weak_box));
        b->target = o;
        insert(o, b);
        __atomic_or_fetch(&o->refcount, ZELL_RC_WEAK_FLAG, __ATOMIC_ACQ_REL);
    }
    zell__plat_mutex_unlock(&g_mu);
    return b;
}

void *zell_weak_get(void *box) {
    weak_box *b = box;
    if (!b) return NULL;
    void *r = NULL;
    zell__plat_mutex_lock(&g_mu);
    zell_object *o = b->target;
    if (o) {
        /* Increment only while the object is still alive (count > 0). */
        int64_t old = __atomic_load_n(&o->refcount, __ATOMIC_RELAXED);
        if ((old & (ZELL_RC_BIASED | ZELL_RC_CYCLIC)) == ZELL_RC_BIASED) {
            /* A biased count (rc.c): B + S, retained through S. */
            if (zell__rc_try_retain_biased(o)) r = (char *)o + sizeof(zell_object);
            old = 0;
        }
        while ((old & ZELL_RC_COUNT_MASK) != 0) {
            if (__atomic_compare_exchange_n(&o->refcount, &old, old + 1, 1, __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED)) {
                /* A collection examining o must see that it was reached. */
                if (old & ZELL_RC_MARKED) zell__cycles_dirty(o);
                r = (char *)o + sizeof(zell_object);
                break;
            }
        }
    }
    zell__plat_mutex_unlock(&g_mu);
    return r;
}

void zell__weak_clear(zell_object *o) {
    zell__plat_mutex_lock(&g_mu);
    entry **pp = find(o);
    if (pp && *pp) {
        (*pp)->box->target = NULL;
        remove_at(pp);
    }
    zell__plat_mutex_unlock(&g_mu);
}

/* The cycle collector (cycles.c) validates a garbage set and clears its
 * boxes under the table lock, so zell_weak_get cannot resurrect a member
 * in between (ABI.md 3.8). */
void zell__weak_lock(void) { zell__plat_mutex_lock(&g_mu); }
void zell__weak_unlock(void) { zell__plat_mutex_unlock(&g_mu); }

void zell__weak_clear_locked(zell_object *o) {
    entry **pp = find(o);
    if (pp && *pp) {
        (*pp)->box->target = NULL;
        remove_at(pp);
    }
}
