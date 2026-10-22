/* map.c - open-addressing hash map (SwissTable-style) with generic key/value
 * sizes and specialised int and string key paths.
 *
 * Layout: `cap` slots (a power of two >= 8, or 0 before the first insert),
 * split into aligned groups of 8. Each slot has one control byte:
 * EMPTY (0x80), DELETED (0xFE) or FULL (0x00..0x7F, the low 7 bits of the
 * hash, "h2"). A lookup starts at group (h >> 7) & (ngroups - 1), matches h2
 * against the 8 control bytes of a group at once (SWAR on a uint64_t),
 * compares keys only for matching bytes, and stops at the first group that
 * holds an EMPTY byte. Groups are probed in triangular order, which visits
 * every group once. The table grows at 7/8 occupancy (FULL + DELETED).
 *
 * Deletion writes DELETED, or EMPTY when the group already holds an EMPTY
 * byte (no probe sequence can have passed through such a group). Entries
 * never move except on resize, so the iterator (a slot index) tolerates
 * deleting the entry it just returned, as ABI.md 6.3 requires.
 *
 * Keys of int maps are hashed with a 64-bit mixer, string keys with an
 * 8-bytes-at-a-time multiplicative hash (internal; zell_string_hash stays
 * FNV-1a as documented), other keys with desc.hash. */
#include "rt_internal.h"

#include <string.h>

enum { KIND_GENERIC, KIND_INT, KIND_STR };

#define CTRL_EMPTY ((uint8_t)0x80)
#define CTRL_DELETED ((uint8_t)0xFE)
#define GROUP 8
#define LSBS 0x0101010101010101ULL
#define MSBS 0x8080808080808080ULL

struct zell_map {
    zell_object hdr;        /* +0  */
    zell_map_desc desc;     /* +16 (48 bytes) */
    int64_t count;           /* live entries */
    int64_t cap;             /* slots: 0 or a power of two >= GROUP */
    int64_t growth_left;     /* insertions into EMPTY slots before a resize */
    int64_t slot_size;       /* ROUND8(key_size) + ROUND8(val_size) */
    int64_t val_off;         /* ROUND8(key_size) */
    int32_t kind;
    uint8_t *ctrl;           /* cap control bytes */
    unsigned char *slots;    /* cap * slot_size bytes */
};

#define SLOT(m, i) ((m)->slots + (size_t)(i) * (size_t)(m)->slot_size)
#define VAL(m, p) ((void *)((unsigned char *)(p) + (m)->val_off))

static inline int64_t max_load(int64_t cap) { return cap - cap / 8; }

static void release_slot(zell_map *m, unsigned char *p) {
    if (m->desc.key_ops && m->desc.key_ops->release) m->desc.key_ops->release(p);
    if (m->desc.val_ops && m->desc.val_ops->release) m->desc.val_ops->release(VAL(m, p));
}

static void map_drop(void *obj) {
    zell_map *m = obj;
    if (m->desc.key_ops || m->desc.val_ops)
        for (int64_t i = 0; i < m->cap; i++)
            if (!(m->ctrl[i] & 0x80)) release_slot(m, SLOT(m, i));
    zell_free(m->ctrl);
}

const zell_type_info zell__map_type = { "map", (int64_t)sizeof(struct zell_map), map_drop };

/* Cycle collector (ABI.md 3.8): the references of every entry. A resize
 * frees the old table, so another thread mutating the map could make the
 * walk read freed memory: once the process has more than one thread the
 * map's entries are not walked (they count as references from outside,
 * so cycles through maps are then not collected; ABI.md 3.8 lists it). */
void zell__map_visit(void *obj, zell_cycle_cb cb, void *ctx) {
    zell_map *m = obj;
    if (__atomic_load_n(&zell_threaded, __ATOMIC_RELAXED)) return;
    const zell_value_ops *ko = m->desc.key_ops, *vo = m->desc.val_ops;
    ko = ko && ko->visit ? ko : NULL;
    vo = vo && vo->visit ? vo : NULL;
    if (!ko && !vo) return;
    for (int64_t i = 0; i < m->cap; i++) {
        if (m->ctrl[i] & 0x80) continue;
        unsigned char *p = SLOT(m, i);
        if (ko) ko->visit(p, cb, ctx);
        if (vo) vo->visit(VAL(m, p), cb, ctx);
    }
}

/* ---- builtin hash / eq ---- */

static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

uint64_t zell_hash_int(const void *key) { return mix64(*(const uint64_t *)key); }
bool zell_eq_int(const void *a, const void *b) {
    return *(const int64_t *)a == *(const int64_t *)b;
}
uint64_t zell_hash_str(const void *key) {
    return zell_string_hash(*(const zell_string *const *)key);
}
bool zell_eq_str(const void *a, const void *b) {
    return zell_string_eq(*(const zell_string *const *)a, *(const zell_string *const *)b);
}

static inline uint64_t load64(const unsigned char *p) {
    uint64_t w;
    memcpy(&w, p, 8);
    return w;
}
static inline uint64_t load32(const unsigned char *p) {
    uint32_t w;
    memcpy(&w, p, 4);
    return w;
}

/* Fast string hash for string-keyed maps (not part of the ABI): 8 bytes per
 * multiply; the tail is read with fixed-size (overlapping) loads so no
 * variable-length copy is needed. */
static inline uint64_t str_hash(const zell_string *s) {
    int64_t n = s ? s->len : 0;
    const unsigned char *p = (const unsigned char *)(s ? s->data : "");
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ (uint64_t)n, w;
    if (n >= 8) {
        const unsigned char *end = p + n;
        while (end - p > 8) {
            h = (h ^ load64(p)) * 0xbf58476d1ce4e5b9ULL;
            h ^= h >> 31;
            p += 8;
        }
        w = load64(end - 8);  /* last 8 bytes, may overlap the previous word */
    } else if (n >= 4) {
        w = load32(p) | (load32(p + n - 4) << 32);
    } else if (n > 0) {
        w = (uint64_t)p[0] | ((uint64_t)p[n / 2] << 8) | ((uint64_t)p[n - 1] << 16);
    } else {
        w = 0;
    }
    h = (h ^ w) * 0x94d049bb133111ebULL;
    return mix64(h);
}

/* memcpy for value slots, with the common sizes inlined. */
static inline void copy_val(void *dst, const void *src, size_t n) {
    switch (n) {
    case 8: memcpy(dst, src, 8); break;
    case 16: memcpy(dst, src, 16); break;
    case 24: memcpy(dst, src, 24); break;
    case 32: memcpy(dst, src, 32); break;
    default: memcpy(dst, src, n);
    }
}

static inline bool str_eq(const zell_string *a, const zell_string *b) {
    if (a == b) return true;
    int64_t la = a ? a->len : 0;
    if (la != (b ? b->len : 0)) return false;
    return la == 0 || memcmp(a->data, b->data, (size_t)la) == 0;
}

static inline __attribute__((always_inline)) uint64_t hash_key(const zell_map *m, int kind,
                                                               const void *key) {
    if (kind == KIND_INT) return mix64(*(const uint64_t *)key);
    if (kind == KIND_STR) return str_hash(*(const zell_string *const *)key);
    return m->desc.hash(key);
}

static inline __attribute__((always_inline)) bool key_eq(const zell_map *m, int kind,
                                                         const void *a, const void *b) {
    if (kind == KIND_INT) return *(const int64_t *)a == *(const int64_t *)b;
    if (kind == KIND_STR)
        return str_eq(*(const zell_string *const *)a, *(const zell_string *const *)b);
    return m->desc.eq(a, b);
}

/* ---- control-byte groups (SWAR) ---- */

/* The 8 control bytes of a group, byte i in bits 8i..8i+7. */
static inline uint64_t group_load(const uint8_t *c) {
    uint64_t g;
    memcpy(&g, c, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    g = __builtin_bswap64(g);
#endif
    return g;
}
/* High bit set in each byte equal to h2 (may report a false positive right
 * after a true match; callers compare keys anyway). */
static inline uint64_t group_match(uint64_t g, uint8_t h2) {
    uint64_t x = g ^ (LSBS * h2);
    return (x - LSBS) & ~x & MSBS;
}
static inline uint64_t group_match_empty(uint64_t g) { return g & ~(g << 6) & MSBS; }
static inline uint64_t group_match_free(uint64_t g) { return g & MSBS; }  /* EMPTY or DELETED */
/* Byte index of the lowest set high bit, and the mask without it. */
static inline int group_first(uint64_t bits) { return __builtin_ctzll(bits) >> 3; }
static inline uint64_t group_next(uint64_t bits) { return bits & (bits - 1); }

static inline uint8_t h2_of(uint64_t h) { return (uint8_t)(h & 0x7F); }
static inline uint64_t h1_of(uint64_t h) { return h >> 7; }

/* Slot index of key, or -1. */
static inline __attribute__((always_inline)) int64_t find(const zell_map *m, int kind,
                                                          const void *key, uint64_t h) {
    if (m->cap == 0) return -1;
    uint64_t gmask = (uint64_t)(m->cap / GROUP) - 1;
    uint64_t g = h1_of(h) & gmask;
    uint8_t h2 = h2_of(h);
    for (uint64_t step = 1;; step++) {
        uint64_t ctrl = group_load(m->ctrl + g * GROUP);
        for (uint64_t bits = group_match(ctrl, h2); bits; bits = group_next(bits)) {
            int64_t i = (int64_t)(g * GROUP) + group_first(bits);
            if (key_eq(m, kind, SLOT(m, i), key)) return i;
        }
        if (group_match_empty(ctrl)) return -1;
        g = (g + step) & gmask;
    }
}

/* First EMPTY or DELETED slot on h's probe sequence. */
static int64_t find_free(const zell_map *m, uint64_t h) {
    uint64_t gmask = (uint64_t)(m->cap / GROUP) - 1;
    uint64_t g = h1_of(h) & gmask;
    for (uint64_t step = 1;; step++) {
        uint64_t f = group_match_free(group_load(m->ctrl + g * GROUP));
        if (f) return (int64_t)(g * GROUP) + group_first(f);
        g = (g + step) & gmask;
    }
}

static void alloc_table(zell_map *m, int64_t cap) {
    if (cap > ((int64_t)1 << 56) / (m->slot_size + 1)) zell__panicf("map too large");
    size_t cbytes = (size_t)cap;  /* multiple of 8 */
    unsigned char *mem = zell_realloc(NULL, (int64_t)(cbytes + (size_t)cap * (size_t)m->slot_size));
    m->ctrl = mem;
    m->slots = mem + cbytes;
    memset(m->ctrl, CTRL_EMPTY, cbytes);
    m->cap = cap;
    m->growth_left = max_load(cap) - m->count;
}

/* Rehash into a table of new_cap slots (entries are moved bytewise; their
 * references move with them). */
static void rehash(zell_map *m, int64_t new_cap) {
    uint8_t *old_ctrl = m->ctrl;
    unsigned char *old_slots = m->slots;
    int64_t old_cap = m->cap;
    alloc_table(m, new_cap);
    for (int64_t i = 0; i < old_cap; i++) {
        if (old_ctrl[i] & 0x80) continue;
        unsigned char *p = old_slots + (size_t)i * (size_t)m->slot_size;
        uint64_t h = hash_key(m, m->kind, p);
        int64_t j = find_free(m, h);
        m->ctrl[j] = h2_of(h);
        memcpy(SLOT(m, j), p, (size_t)m->slot_size);
    }
    zell_free(old_ctrl);
}

static int64_t cap_for(int64_t n) {
    int64_t cap = GROUP;
    while (max_load(cap) < n && cap < ((int64_t)1 << 56)) cap <<= 1;
    return cap;
}

/* Inserts a new entry for key (known absent) with a zeroed value; retains the key. */
static __attribute__((noinline)) int64_t insert_new(zell_map *m, const void *key, uint64_t h) {
    if (m->cap == 0) {
        alloc_table(m, GROUP);
    }
    int64_t i = find_free(m, h);
    if (m->growth_left == 0 && m->ctrl[i] == CTRL_EMPTY) {
        /* Grow, or just drop tombstones when most slots are deleted. */
        int64_t nc = m->count + 1 > m->cap / 2 ? m->cap * 2 : m->cap;
        rehash(m, nc);
        i = find_free(m, h);
    }
    if (m->ctrl[i] == CTRL_EMPTY) m->growth_left--;
    m->ctrl[i] = h2_of(h);
    unsigned char *p = SLOT(m, i);
    memcpy(p, key, (size_t)m->desc.key_size);
    if (m->desc.key_ops && m->desc.key_ops->retain) m->desc.key_ops->retain(p);
    memset(VAL(m, p), 0, (size_t)(m->slot_size - m->val_off));
    m->count++;
    return i;
}

/* ---- construction ---- */

zell_map *zell_map_new(const zell_map_desc *desc, int64_t hint) {
    if (!desc || !desc->hash || !desc->eq || desc->key_size <= 0 || desc->val_size < 0)
        zell__panicf("zell_map_new: invalid map descriptor");
    int cyclic = (desc->key_ops && desc->key_ops->visit) || (desc->val_ops && desc->val_ops->visit);
    int64_t bytes = (int64_t)sizeof(struct zell_map);
    cyclic = cyclic && zell__cycles_flag;   /* weak: the collector is linked */
    zell_map *m = zell_object_new(&zell__map_type, cyclic ? zell__cycles_extra(bytes) : bytes);
    m->desc = *desc;
    if (cyclic) zell__cycles_flag(&m->hdr);  /* entries may lie on cycles */
    m->val_off = ZELL_ROUND8(desc->key_size);
    m->slot_size = m->val_off + ZELL_ROUND8(desc->val_size);
    m->kind = KIND_GENERIC;
    if (desc->key_size == 8 && desc->hash == zell_hash_int && desc->eq == zell_eq_int)
        m->kind = KIND_INT;
    else if (desc->key_size == 8 && desc->hash == zell_hash_str && desc->eq == zell_eq_str)
        m->kind = KIND_STR;
    if (hint > 0) alloc_table(m, cap_for(hint));
    return m;
}

zell_map *zell_map_new_str(int64_t val_size, const zell_value_ops *val_ops) {
    zell_map_desc d = { 8, val_size, zell_hash_str, zell_eq_str, &zell_ops_ref, val_ops };
    return zell_map_new(&d, 0);
}

zell_map *zell_map_new_int(int64_t val_size, const zell_value_ops *val_ops) {
    zell_map_desc d = { 8, val_size, zell_hash_int, zell_eq_int, NULL, val_ops };
    return zell_map_new(&d, 0);
}

int64_t zell_map_len(const zell_map *m) { return m ? m->count : 0; }

/* ---- operations, specialised per key kind ---- */

static inline __attribute__((always_inline)) void *get_k(const zell_map *m, int kind,
                                                         const void *key) {
    if (!m || m->count == 0) return NULL;
    int64_t i = find(m, kind, key, hash_key(m, kind, key));
    return i < 0 ? NULL : VAL(m, SLOT(m, i));
}

static inline __attribute__((always_inline)) void *get_or_insert_k(zell_map *m, int kind,
                                                                   const void *key) {
    if (!m) zell_panic("assignment to entry in nil map");
    uint64_t h = hash_key(m, kind, key);
    int64_t i = find(m, kind, key, h);
    if (i < 0) i = insert_new(m, key, h);
    return VAL(m, SLOT(m, i));
}

static inline __attribute__((always_inline)) void set_k(zell_map *m, int kind, const void *key,
                                                        const void *val) {
    if (!m) zell_panic("assignment to entry in nil map");
    const zell_value_ops *vo = m->desc.val_ops;
    size_t vs = (size_t)m->desc.val_size;
    if (vo && vo->retain) {
        /* retain the new value before releasing the old one: they may be the
         * same object, and val may point into the map (moved by a resize) */
        unsigned char tmp[256];
        unsigned char *copy = vs <= sizeof tmp ? tmp : zell_alloc((int64_t)vs);
        copy_val(copy, val, vs);
        vo->retain(copy);
        void *slot = get_or_insert_k(m, kind, key);
        if (vo->release) vo->release(slot);
        copy_val(slot, copy, vs);
        if (copy != tmp) zell_free(copy);
    } else if (vs <= 64) {
        unsigned char tmp[64];  /* val may point into the map (moved by a resize) */
        copy_val(tmp, val, vs);
        void *slot = get_or_insert_k(m, kind, key);
        if (vo && vo->release) vo->release(slot);
        copy_val(slot, tmp, vs);
    } else {
        unsigned char *copy = zell_alloc((int64_t)vs);
        memcpy(copy, val, vs);
        void *slot = get_or_insert_k(m, kind, key);
        if (vo && vo->release) vo->release(slot);
        memcpy(slot, copy, vs);
        zell_free(copy);
    }
}

static inline __attribute__((always_inline)) bool delete_k(zell_map *m, int kind,
                                                           const void *key) {
    if (!m || m->count == 0) return false;
    int64_t i = find(m, kind, key, hash_key(m, kind, key));
    if (i < 0) return false;
    int64_t g = i & ~(int64_t)(GROUP - 1);
    if (group_match_empty(group_load(m->ctrl + g))) {
        m->ctrl[i] = CTRL_EMPTY;
        m->growth_left++;
    } else {
        m->ctrl[i] = CTRL_DELETED;
    }
    m->count--;
    release_slot(m, SLOT(m, i));
    return true;
}

void *zell_map_get(const zell_map *m, const void *key) {
    if (m && m->kind == KIND_INT) return get_k(m, KIND_INT, key);
    if (m && m->kind == KIND_STR) return get_k(m, KIND_STR, key);
    return get_k(m, KIND_GENERIC, key);
}

void *zell_map_get_or_insert(zell_map *m, const void *key) {
    if (m && m->kind == KIND_INT) return get_or_insert_k(m, KIND_INT, key);
    if (m && m->kind == KIND_STR) return get_or_insert_k(m, KIND_STR, key);
    return get_or_insert_k(m, KIND_GENERIC, key);
}

void zell_map_set(zell_map *m, const void *key, const void *val) {
    if (m && m->kind == KIND_INT) set_k(m, KIND_INT, key, val);
    else if (m && m->kind == KIND_STR) set_k(m, KIND_STR, key, val);
    else set_k(m, KIND_GENERIC, key, val);
}

bool zell_map_delete(zell_map *m, const void *key) {
    if (m && m->kind == KIND_INT) return delete_k(m, KIND_INT, key);
    if (m && m->kind == KIND_STR) return delete_k(m, KIND_STR, key);
    return delete_k(m, KIND_GENERIC, key);
}

/* ---- fast paths (the map must have been created for that key type) ---- */

void *zell_map_get_str(const zell_map *m, const zell_string *key) {
    return get_k(m, KIND_STR, &key);
}
void zell_map_set_str(zell_map *m, const zell_string *key, const void *val) {
    set_k(m, KIND_STR, &key, val);
}
void *zell_map_get_or_insert_str(zell_map *m, const zell_string *key) {
    return get_or_insert_k(m, KIND_STR, &key);
}
bool zell_map_delete_str(zell_map *m, const zell_string *key) {
    return delete_k(m, KIND_STR, &key);
}
void *zell_map_get_int(const zell_map *m, int64_t key) { return get_k(m, KIND_INT, &key); }
void zell_map_set_int(zell_map *m, int64_t key, const void *val) {
    set_k(m, KIND_INT, &key, val);
}
void *zell_map_get_or_insert_int(zell_map *m, int64_t key) {
    return get_or_insert_k(m, KIND_INT, &key);
}
bool zell_map_delete_int(zell_map *m, int64_t key) { return delete_k(m, KIND_INT, &key); }

/* ---- iteration ---- */

void zell_map_iter_init(zell_map_iter *it, zell_map *m) {
    it->map = m;
    it->bucket = 0;  /* next slot index to examine */
    it->next = NULL;
    it->reserved = 0;
}

bool zell_map_iter_next(zell_map_iter *it, void **key, void **val) {
    zell_map *m = it->map;
    if (!m) return false;
    for (int64_t i = it->bucket; i < m->cap; i++) {
        if (m->ctrl[i] & 0x80) continue;
        it->bucket = i + 1;
        unsigned char *p = SLOT(m, i);
        if (key) *key = p;
        if (val) *val = VAL(m, p);
        return true;
    }
    it->bucket = m->cap;
    return false;
}
