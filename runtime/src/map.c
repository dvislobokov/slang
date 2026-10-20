/* map.c - separately chained hash map with generic key/value sizes. */
#include "rt_internal.h"

#include <string.h>

typedef struct map_entry {
    struct map_entry *next;  /* +0  */
    uint64_t hash;           /* +8  */
    unsigned char kv[];      /* +16 key (rounded to 8), then value */
} map_entry;

struct slang_map {
    slang_object hdr;        /* +0  */
    slang_map_desc desc;     /* +16 (48 bytes) */
    int64_t count;           /* +64 */
    int64_t nbuckets;        /* +72 power of two */
    map_entry **buckets;     /* +80 */
};

#define KEY(m, e) ((void *)(e)->kv)
#define VAL(m, e) ((void *)((e)->kv + SLANG_ROUND8((m)->desc.key_size)))

static void entry_free(slang_map *m, map_entry *e) {
    if (m->desc.key_ops && m->desc.key_ops->release) m->desc.key_ops->release(KEY(m, e));
    if (m->desc.val_ops && m->desc.val_ops->release) m->desc.val_ops->release(VAL(m, e));
    slang_free(e);
}

static void map_drop(void *obj) {
    slang_map *m = obj;
    for (int64_t i = 0; i < m->nbuckets; i++) {
        map_entry *e = m->buckets[i];
        while (e) { map_entry *n = e->next; entry_free(m, e); e = n; }
    }
    slang_free(m->buckets);
}

static const slang_type_info map_type = { "map", (int64_t)sizeof(struct slang_map), map_drop };

/* ---- builtin hash / eq ---- */

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

uint64_t slang_hash_int(const void *key) { return mix64(*(const uint64_t *)key); }
bool slang_eq_int(const void *a, const void *b) {
    return *(const int64_t *)a == *(const int64_t *)b;
}
uint64_t slang_hash_str(const void *key) {
    return slang_string_hash(*(const slang_string *const *)key);
}
bool slang_eq_str(const void *a, const void *b) {
    return slang_string_eq(*(const slang_string *const *)a, *(const slang_string *const *)b);
}

/* ---- construction ---- */

slang_map *slang_map_new(const slang_map_desc *desc, int64_t hint) {
    if (!desc || !desc->hash || !desc->eq || desc->key_size <= 0 || desc->val_size < 0)
        slang__panicf("slang_map_new: invalid map descriptor");
    slang_map *m = slang_object_new(&map_type, (int64_t)sizeof(struct slang_map));
    m->desc = *desc;
    int64_t nb = 8;
    while (nb < hint && nb < ((int64_t)1 << 40)) nb <<= 1;
    m->nbuckets = nb;
    m->buckets = slang_alloc(nb * (int64_t)sizeof(map_entry *));
    return m;
}

slang_map *slang_map_new_str(int64_t val_size, const slang_value_ops *val_ops) {
    slang_map_desc d = { 8, val_size, slang_hash_str, slang_eq_str, &slang_ops_ref, val_ops };
    return slang_map_new(&d, 0);
}

slang_map *slang_map_new_int(int64_t val_size, const slang_value_ops *val_ops) {
    slang_map_desc d = { 8, val_size, slang_hash_int, slang_eq_int, NULL, val_ops };
    return slang_map_new(&d, 0);
}

int64_t slang_map_len(const slang_map *m) { return m ? m->count : 0; }

/* ---- lookup ---- */

static map_entry *find(const slang_map *m, const void *key, uint64_t h, map_entry ***link_out) {
    map_entry **link = &m->buckets[h & (uint64_t)(m->nbuckets - 1)];
    for (map_entry *e = *link; e; link = &e->next, e = e->next) {
        if (e->hash == h && m->desc.eq(KEY(m, e), key)) {
            if (link_out) *link_out = link;
            return e;
        }
    }
    return NULL;
}

void *slang_map_get(const slang_map *m, const void *key) {
    if (!m) return NULL;
    map_entry *e = find(m, key, m->desc.hash(key), NULL);
    return e ? VAL(m, e) : NULL;
}

static void resize(slang_map *m) {
    int64_t nb = m->nbuckets * 2;
    map_entry **nbk = slang_alloc(nb * (int64_t)sizeof(map_entry *));
    for (int64_t i = 0; i < m->nbuckets; i++) {
        map_entry *e = m->buckets[i];
        while (e) {
            map_entry *n = e->next;
            map_entry **b = &nbk[e->hash & (uint64_t)(nb - 1)];
            e->next = *b;
            *b = e;
            e = n;
        }
    }
    slang_free(m->buckets);
    m->buckets = nbk;
    m->nbuckets = nb;
}

/* Insert a new zeroed-value entry for key (key retained). Returns entry. */
static map_entry *insert_new(slang_map *m, const void *key, uint64_t h) {
    if (m->count >= m->nbuckets) resize(m);
    int64_t ks = SLANG_ROUND8(m->desc.key_size);
    map_entry *e = slang_alloc((int64_t)sizeof(map_entry) + ks + m->desc.val_size);
    e->hash = h;
    memcpy(KEY(m, e), key, (size_t)m->desc.key_size);
    if (m->desc.key_ops && m->desc.key_ops->retain) m->desc.key_ops->retain(KEY(m, e));
    map_entry **b = &m->buckets[h & (uint64_t)(m->nbuckets - 1)];
    e->next = *b;
    *b = e;
    m->count++;
    return e;
}

void *slang_map_get_or_insert(slang_map *m, const void *key) {
    if (!m) slang_panic("assignment to entry in nil map");
    uint64_t h = m->desc.hash(key);
    map_entry *e = find(m, key, h, NULL);
    if (!e) e = insert_new(m, key, h);
    return VAL(m, e);
}

void slang_map_set(slang_map *m, const void *key, const void *val) {
    if (!m) slang_panic("assignment to entry in nil map");
    const slang_value_ops *vo = m->desc.val_ops;
    /* retain the new value before releasing the old one: they may be the same object */
    if (vo && vo->retain) {
        unsigned char tmp[256];
        unsigned char *copy = (size_t)m->desc.val_size <= sizeof tmp
                                  ? tmp : slang_alloc(m->desc.val_size);
        memcpy(copy, val, (size_t)m->desc.val_size);
        vo->retain(copy);
        void *slot = slang_map_get_or_insert(m, key);
        if (vo->release) vo->release(slot);
        memcpy(slot, copy, (size_t)m->desc.val_size);
        if (copy != tmp) slang_free(copy);
    } else {
        void *slot = slang_map_get_or_insert(m, key);
        if (vo && vo->release) vo->release(slot);
        memmove(slot, val, (size_t)m->desc.val_size);
    }
}

bool slang_map_delete(slang_map *m, const void *key) {
    if (!m) return false;
    map_entry **link;
    map_entry *e = find(m, key, m->desc.hash(key), &link);
    if (!e) return false;
    *link = e->next;
    m->count--;
    entry_free(m, e);
    return true;
}

/* ---- fast paths ---- */

void *slang_map_get_str(const slang_map *m, const slang_string *key) { return slang_map_get(m, &key); }
void slang_map_set_str(slang_map *m, const slang_string *key, const void *val) {
    slang_map_set(m, &key, val);
}
bool slang_map_delete_str(slang_map *m, const slang_string *key) { return slang_map_delete(m, &key); }
void *slang_map_get_int(const slang_map *m, int64_t key) { return slang_map_get(m, &key); }
void slang_map_set_int(slang_map *m, int64_t key, const void *val) { slang_map_set(m, &key, val); }
bool slang_map_delete_int(slang_map *m, int64_t key) { return slang_map_delete(m, &key); }

/* ---- iteration ---- */

void slang_map_iter_init(slang_map_iter *it, slang_map *m) {
    it->map = m;
    it->bucket = 0;
    it->next = (m && m->nbuckets > 0) ? m->buckets[0] : NULL;
    it->reserved = 0;
}

bool slang_map_iter_next(slang_map_iter *it, void **key, void **val) {
    slang_map *m = it->map;
    if (!m) return false;
    map_entry *e = it->next;
    while (!e) {
        it->bucket++;
        if (it->bucket >= m->nbuckets) { it->next = NULL; it->bucket = m->nbuckets; return false; }
        e = m->buckets[it->bucket];
    }
    it->next = e->next;  /* computed before returning so deleting e is safe */
    if (key) *key = KEY(m, e);
    if (val) *val = VAL(m, e);
    return true;
}
