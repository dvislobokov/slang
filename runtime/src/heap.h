/* heap.h - the object heap's types and allocation fast paths, inlined into
 * mem.c's object allocation (the design is described in heap.c). Only
 * mem.c and heap.c include this. */
#ifndef SLANG_HEAP_H
#define SLANG_HEAP_H

#include "rt_internal.h"

#include <string.h>

#define HP_SEG_SHIFT 22
#define HP_SEG ((size_t)1 << HP_SEG_SHIFT)       /* 4 MB segments */
#define HP_PAGE_SHIFT 16
#define HP_PAGE ((size_t)1 << HP_PAGE_SHIFT)     /* 64 KB pages */
#define HP_PAGES ((int)(HP_SEG / HP_PAGE))
#define HP_ADDR_BITS 48
#define HP_LEAF_BITS 16
#define HP_TOP (1u << (HP_ADDR_BITS - HP_SEG_SHIFT - HP_LEAF_BITS))

typedef struct hp_block { struct hp_block *next; } hp_block;

struct hp_heap;
typedef struct hp_page {
    hp_block *free;              /* owner: blocks to allocate */
    hp_block *local_free;        /* owner: blocks the owner freed */
    hp_block *xfree;             /* atomic: blocks other threads freed */
    struct hp_heap *heap;        /* atomic: owner, NULL when free or abandoned */
    struct hp_page *next, *prev; /* in a heap's queue / full list, or a pool list */
    char *start, *bump, *end;    /* blocks; [bump, end) not carved yet */
    uint32_t bsize;
    uint32_t used;               /* blocks handed out, minus those taken back */
    uint16_t cls;
    uint8_t state;
    uint8_t in_full;             /* atomic: in the owner's full list */
    int64_t freed_at;            /* monotonic ns when it entered the free list (g_mu) */
} hp_page;

typedef struct hp_list { hp_page *head, *tail; } hp_list;

typedef struct hp_heap {
    hp_page *cur[SLANG__HP_CLASSES];   /* page allocated from (head of queue, or the empty page) */
    hp_list queue[SLANG__HP_CLASSES];  /* pages that may have blocks */
    hp_list full[SLANG__HP_CLASSES];   /* pages that had none at the last look */
    uint32_t hint;                     /* atomic: remote frees into full pages */
    uint32_t seen;
    uint32_t epoch;                    /* last forced-collection epoch collected for */
    struct hp_heap *next_free;         /* pool of heaps of exited threads */
} hp_heap;

/* The calling thread's heap (a static empty heap until its first slow
 * path), the segment bitmap and the class sizes (heap.c). */
SLANG_HIDDEN extern _Thread_local hp_heap *slang__hp_tls;
SLANG_HIDDEN extern uint64_t *slang__hp_map[HP_TOP];
SLANG_HIDDEN extern const uint32_t slang__hp_sizes[SLANG__HP_CLASSES];
SLANG_HIDDEN extern uint8_t slang__heap_on;

SLANG_HIDDEN int slang__hp_start(void);
SLANG_HIDDEN void *slang__hp_alloc_slow(uint32_t c);
SLANG_HIDDEN void slang__hp_free_local_slow(hp_heap *h, hp_page *pg);
SLANG_HIDDEN void slang__hp_free_remote(hp_page *pg, hp_block *b);
SLANG_HIDDEN void *slang__hp_alloc_big(int64_t size, int zero);
SLANG_HIDDEN void slang__hp_free_big(void *p);
SLANG_HIDDEN int64_t slang__hp_big_capacity(const void *p);
SLANG_HIDDEN void *slang__hp_resize(void *p, int64_t size);

/* Size class of a small size (1 ..= SLANG__HP_SMALL_MAX): 16-byte steps to
 * 128, then four classes per power of two. */
static inline uint32_t slang__hp_cls(int64_t size) {
    uint64_t s = (uint64_t)size - 1;
    if (s < 128) return (uint32_t)(s >> 4);
    int b = 63 - __builtin_clzll(s);   /* 7..13 */
    return (uint32_t)(8 + (b - 7) * 4 + (int)((s - ((uint64_t)1 << b)) >> (b - 2)));
}

static inline hp_page *slang__hp_page_of(const void *p) {
    uintptr_t seg = (uintptr_t)p & ~(uintptr_t)(HP_SEG - 1);
    return (hp_page *)seg + (((uintptr_t)p >> HP_PAGE_SHIFT) & (HP_PAGES - 1));
}

/* Whether p lies in a small-object segment. */
static inline int slang__heap_is_small(const void *p) {
    uintptr_t a = (uintptr_t)p;
    if (a >> HP_ADDR_BITS) return 0;
    uintptr_t r = a >> HP_SEG_SHIFT;
    uint64_t *leaf = __atomic_load_n(&slang__hp_map[r >> HP_LEAF_BITS], __ATOMIC_ACQUIRE);
    if (!leaf) return 0;
    uintptr_t bit = r & ((1u << HP_LEAF_BITS) - 1);
    return (int)((__atomic_load_n(&leaf[bit >> 6], __ATOMIC_RELAXED) >> (bit & 63)) & 1);
}

/* `size` bytes, zeroed when `zero`; NULL when the heap is off. */
static inline void *slang__heap_alloc(int64_t size, int zero) {
    if (__builtin_expect(!slang__heap_on, 0) && !slang__hp_start()) return NULL;
    if (__builtin_expect(size > SLANG__HP_SMALL_MAX, 0)) return slang__hp_alloc_big(size, zero);
    uint32_t c = slang__hp_cls(size);
    hp_page *pg = slang__hp_tls->cur[c];
    hp_block *b = pg->free;
    if (__builtin_expect(b != NULL, 1)) {
        pg->free = b->next;
        pg->used++;
    } else {
        b = slang__hp_alloc_slow(c);
    }
    if (zero) memset(b, 0, (size_t)size);
    return b;
}

/* Frees memory from slang__heap_alloc. */
static inline void slang__heap_free(void *p) {
    if (__builtin_expect(!slang__heap_is_small(p), 0)) {
        slang__hp_free_big(p);
        return;
    }
    hp_page *pg = slang__hp_page_of(p);
    hp_block *b = p;
    hp_heap *h = slang__hp_tls;
    if (__builtin_expect(__atomic_load_n(&pg->heap, __ATOMIC_RELAXED) == h, 1)) {
        b->next = pg->local_free;
        pg->local_free = b;
        pg->used--;
        if (__builtin_expect(pg->used == 0 || __atomic_load_n(&pg->in_full, __ATOMIC_RELAXED), 0))
            slang__hp_free_local_slow(h, pg);
        return;
    }
    slang__hp_free_remote(pg, b);
}

/* Usable bytes of memory from slang__heap_alloc. */
static inline int64_t slang__heap_capacity(const void *p) {
    if (slang__heap_is_small(p)) return slang__hp_page_of(p)->bsize;
    return slang__hp_big_capacity(p);
}

#endif
