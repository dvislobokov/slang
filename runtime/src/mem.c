/* mem.c - raw allocation, refcounted object header, value ops. */
#include "rt_internal.h"

#include <stdlib.h>
#include <string.h>

static int64_t g_live_objects;

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

void *slang_object_new(const slang_type_info *type, int64_t size) {
    if (size < (int64_t)sizeof(slang_object))
        slang__panicf("slang_object_new: size %lld smaller than header", (long long)size);
    slang_object *o = slang_alloc(size);
    o->refcount = 1;
    o->type = type;
    __atomic_add_fetch(&g_live_objects, 1, __ATOMIC_RELAXED);
    return o;
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
    if (__atomic_sub_fetch(&o->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        if (o->type && o->type->drop) o->type->drop(o);
        __atomic_sub_fetch(&g_live_objects, 1, __ATOMIC_RELAXED);
        free(o);
    }
}

int64_t slang_refcount(const void *obj) {
    if (!obj) return 0;
    return __atomic_load_n(&((const slang_object *)obj)->refcount, __ATOMIC_RELAXED);
}

int64_t slang_live_objects(void) {
    return __atomic_load_n(&g_live_objects, __ATOMIC_RELAXED);
}

/* ---- value ops ---- */

static void ops_ref_retain(void *slot) { slang_retain(*(void **)slot); }
static void ops_ref_release(void *slot) { slang_release(*(void **)slot); }
const slang_value_ops slang_ops_ref = { ops_ref_retain, ops_ref_release };

static void ops_slice_retain(void *slot) { slang_slice_retain((slang_slice *)slot); }
static void ops_slice_release(void *slot) { slang_slice_release((slang_slice *)slot); }
const slang_value_ops slang_ops_slice = { ops_slice_retain, ops_slice_release };
