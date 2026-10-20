/* arena.c - std.arena: bump allocation of objects freed all at once (ABI.md 3.6).
 *
 * An arena is a list of chunks. Every allocation is a record: a 16-byte
 * prefix (record size, unused word) followed by an ordinary object header
 * whose refcount is SLANG_RC_ARENA, so slang_retain/slang_release ignore it.
 * slang_arena_free runs the type's drop of every record (releasing what the
 * objects reference outside the arena) in allocation order, then frees the
 * chunks. Arena objects are not counted by slang_live_objects. Arenas are
 * not thread-safe. */
#include "rt_internal.h"

#include <stdlib.h>
#include <string.h>

#define CHUNK_SIZE ((int64_t)64 << 10)

typedef struct chunk {
    struct chunk *next;
    int64_t used, cap;     /* bytes of data[] */
    int64_t _pad;
    unsigned char data[];  /* 16-byte aligned records */
} chunk;

typedef struct arena {
    chunk *first, *last;
    int64_t objects;
} arena;

typedef struct record {
    int64_t size;          /* whole record, prefix included */
    int64_t _pad;
    slang_object obj;      /* payload follows */
} record;

/* Array backing of a.Alloc<T>(n): same layout as slice.c's backing. */
typedef struct backing {
    slang_object hdr;
    int64_t elem_size, cap;
    const slang_value_ops *ops;
    int64_t _pad;
    unsigned char data[];
} backing;

static void backing_drop(void *obj) {
    backing *b = obj;
    if (!b->ops || !b->ops->release) return;
    for (int64_t i = 0; i < b->cap; i++) b->ops->release(b->data + i * b->elem_size);
}

static const slang_type_info arena_backing_type = { "[]backing(arena)", 0, backing_drop };

static arena *get(int64_t h) {
    if (!h) slang__panicf("use of an arena that was not created with arena.New()");
    return (arena *)(intptr_t)h;
}

int64_t slang_arena_create(void) { return (int64_t)(intptr_t)slang_alloc(sizeof(arena)); }

void *slang_arena_object(int64_t h, const slang_type_info *type, int64_t size) {
    arena *a = get(h);
    if (size < (int64_t)sizeof(slang_object) || size > INT64_MAX / 2)
        slang__panicf("arena: invalid object size %lld", (long long)size);
    int64_t need = ((int64_t)offsetof(record, obj) + size + 15) & ~(int64_t)15;
    chunk *c = a->last;
    if (!c || c->cap - c->used < need) {
        int64_t cap = need > CHUNK_SIZE / 4 ? need : CHUNK_SIZE;
        c = aligned_alloc(16, (sizeof(chunk) + (size_t)cap + 15) & ~(size_t)15);
        if (!c) slang__panicf("out of memory (arena chunk of %lld bytes)", (long long)cap);
        c->next = NULL;
        c->used = 0;
        c->cap = cap;
        if (a->last) a->last->next = c; else a->first = c;
        a->last = c;
    }
    record *r = (record *)(c->data + c->used);
    c->used += need;
    memset(r, 0, (size_t)need);
    r->size = need;
    r->obj.refcount = SLANG_RC_ARENA;
    r->obj.type = type;
    a->objects++;
    return &r->obj;
}

void slang_arena_slice(slang_slice *out, int64_t h, int64_t elem_size, int64_t n, const slang_value_ops *ops) {
    if (n < 0 || elem_size < 0 || (elem_size > 0 && n > (INT64_MAX / 4) / elem_size))
        slang__panicf("arena: invalid slice size (elem %lld, n %lld)", (long long)elem_size, (long long)n);
    if (n == 0) { memset(out, 0, sizeof *out); return; }
    backing *b = slang_arena_object(h, &arena_backing_type, (int64_t)sizeof(backing) + elem_size * n);
    b->elem_size = elem_size;
    b->cap = n;
    b->ops = ops;
    out->data = b->data;
    out->len = n;
    out->cap = n;
    out->backing = &b->hdr;
}

slang_string *slang_arena_string(int64_t h, const slang_string *s) {
    int64_t len = s ? s->len : 0;
    if (len == 0) return NULL;
    slang_string *r = slang_arena_object(h, &slang_type_string, (int64_t)sizeof(slang_string) + len + 1);
    r->len = len;
    memcpy(r->data, s->data, (size_t)len);
    r->data[len] = 0;
    return r;
}

int64_t slang_arena_objects(int64_t h) { return h ? get(h)->objects : 0; }

void slang_arena_free(int64_t h) {
    if (!h) return;
    arena *a = get(h);
    /* A drop may allocate in this arena again: repeat until it is empty. */
    while (a->first) {
        chunk *c = a->first;
        a->first = a->last = NULL;
        a->objects = 0;
        while (c) {
            for (int64_t off = 0; off < c->used;) {
                record *r = (record *)(c->data + off);
                off += r->size;
                if (r->obj.type && r->obj.type->drop) r->obj.type->drop(&r->obj);
            }
            chunk *next = c->next;
            free(c);
            c = next;
        }
    }
}

void slang_arena_destroy(int64_t h) {
    if (!h) return;
    slang_arena_free(h);
    slang_free(get(h));
}
