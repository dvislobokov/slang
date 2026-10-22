/* slice.c - Go-like slices over a refcounted backing array. */
#include "rt_internal.h"

#include <string.h>

/* Backing array object (internal, but layout documented in ABI.md). */
typedef struct zell_backing {
    zell_object hdr;             /* +0  */
    int64_t elem_size;            /* +16 */
    int64_t cap;                  /* +24 elements */
    const zell_value_ops *ops;   /* +32 */
    int64_t _pad;                 /* +40 keeps data 16-byte aligned */
    unsigned char data[];         /* +48 */
} zell_backing;

static void backing_drop(void *obj) {
    zell_backing *b = obj;
    if (!b->ops || !b->ops->release) return;
    if (b->ops == &zell_ops_ref) {  /* common case: strings, maps, errors... */
        zell_object **el = (zell_object **)b->data;
        for (int64_t i = 0; i < b->cap; i++) zell_release(el[i]);
        return;
    }
    for (int64_t i = 0; i < b->cap; i++) b->ops->release(b->data + i * b->elem_size);
}

const zell_type_info zell__backing_type = { "[]backing", 0, backing_drop };

/* Cycle collector (ABI.md 3.8): the references of every element slot. A
 * flagged backing never moves (its count is never exactly 1, so append
 * does not resize it in place), and its elements are read word by word. */
void zell__backing_visit(void *obj, zell_cycle_cb cb, void *ctx) {
    zell_backing *b = obj;
    const zell_value_ops *ops = b->ops;
    if (!ops || !ops->visit) return;
    for (int64_t i = 0; i < b->cap; i++) ops->visit(b->data + i * b->elem_size, cb, ctx);
}

static void check_size(int64_t elem_size, int64_t n) {
    if (elem_size < 0 || n < 0) zell__panicf("invalid slice size (elem %lld, n %lld)",
                                              (long long)elem_size, (long long)n);
    if (elem_size > 0 && n > (INT64_MAX - 64) / elem_size)
        zell__panicf("slice too large (%lld elements of %lld bytes)", (long long)n,
                      (long long)elem_size);
}

static zell_backing *backing_new(int64_t elem_size, int64_t cap, const zell_value_ops *ops) {
    check_size(elem_size, cap);
    int64_t bytes = (int64_t)sizeof(zell_backing) + elem_size * cap;
    /* Elements that may lie on cycles: room for the collector's word. */
    int cyclic = ops && ops->visit && zell__cycles_flag;   /* weak: the collector is linked */
    if (cyclic) bytes = zell__cycles_extra(bytes);
    zell_backing *b = zell_object_new(&zell__backing_type, bytes);
    b->elem_size = elem_size;
    b->cap = cap;
    b->ops = ops;
    if (cyclic) zell__cycles_flag(&b->hdr);  /* elements may lie on cycles */
    return b;
}

void zell_slice_new_ops(zell_slice *out, int64_t elem_size, int64_t len, int64_t cap,
                         const zell_value_ops *ops) {
    if (len < 0 || cap < len)
        zell__panicf("makeslice: len %lld out of range (cap %lld)", (long long)len,
                      (long long)cap);
    if (cap == 0) { memset(out, 0, sizeof *out); return; }
    zell_backing *b = backing_new(elem_size, cap, ops);
    out->data = b->data;
    out->len = len;
    out->cap = cap;
    out->backing = &b->hdr;
}

void zell_slice_new(zell_slice *out, int64_t elem_size, int64_t len, int64_t cap) {
    zell_slice_new_ops(out, elem_size, len, cap, NULL);
}

void zell_slice_from(zell_slice *out, int64_t elem_size, const zell_value_ops *ops,
                      const void *elems, int64_t n) {
    zell_slice_new_ops(out, elem_size, n, n, ops);
    if (n == 0) return;
    memcpy(out->data, elems, (size_t)(elem_size * n));
    if (ops && ops->retain)
        for (int64_t i = 0; i < n; i++) ops->retain((char *)out->data + i * elem_size);
}

void zell_slice_retain(const zell_slice *s) { zell_retain(s->backing); }

void zell_slice_release(zell_slice *s) {
    zell_object *b = s->backing;
    memset(s, 0, sizeof *s);
    zell_release(b);
}

void zell_slice_assign(zell_slice *dst, const zell_slice *src) {
    if (dst == src) return;
    zell_object *old = dst->backing;
    zell_retain(src->backing);
    *dst = *src;
    zell_release(old);
}

void *zell_slice_index(const zell_slice *s, int64_t i, int64_t elem_size) {
    if (i < 0 || i >= s->len) zell_panic_bounds(i, s->len);
    return (char *)s->data + i * elem_size;
}

/* Ensure room for `extra` more elements, reallocating (copy + retain) if needed. */
static void grow(zell_slice *s, int64_t elem_size, const zell_value_ops *ops, int64_t extra) {
    int64_t need = s->len + extra;
    if (need < s->len) zell__panicf("slice length overflow");
    if (need <= s->cap) return;
    int64_t ncap = s->cap < 4 ? 4 : s->cap;
    while (ncap < need) {
        if (ncap > INT64_MAX / 2) { ncap = need; break; }
        ncap = ncap < 1024 ? ncap * 2 : ncap + ncap / 2;
    }
    zell_backing *ob = (zell_backing *)s->backing;
    if (ob && s->data == (void *)ob->data && ob->elem_size == elem_size && ob->ops == ops &&
        zell__rc_unique(&ob->hdr)) {
        /* Only this slice references the backing: grow it in place; the
         * elements keep their references (no retain/release per element). */
        check_size(elem_size, ncap);
        int64_t old_cap = ob->cap;
        zell_backing *nb = (zell_backing *)zell__object_resize(
            &ob->hdr, (int64_t)sizeof(zell_backing) + elem_size * ncap);
        memset(nb->data + old_cap * elem_size, 0, (size_t)((ncap - old_cap) * elem_size));
        nb->cap = ncap;
        s->data = nb->data;
        s->cap = ncap;
        s->backing = &nb->hdr;
        return;
    }
    zell_backing *b = backing_new(elem_size, ncap, ops);
    if (s->len > 0) {
        memcpy(b->data, s->data, (size_t)(s->len * elem_size));
        if (ops && ops->retain)
            for (int64_t i = 0; i < s->len; i++) ops->retain(b->data + i * elem_size);
    }
    zell_object *old = s->backing;
    s->data = b->data;
    s->cap = ncap;
    s->backing = &b->hdr;
    zell_release(old);
}

/* Store a borrowed value into a slot owned by the backing array. */
static void store(void *slot, const void *val, int64_t elem_size, const zell_value_ops *ops) {
    if (ops && ops->retain) ops->retain((void *)val);  /* retain first: val may alias slot */
    if (ops && ops->release) ops->release(slot);
    memmove(slot, val, (size_t)elem_size);
}

void zell_slice_append(zell_slice *s, int64_t elem_size, const zell_value_ops *ops,
                        const void *elem) {
    if (s->len < s->cap) {
        /* Fast path: room in the (possibly shared) backing, as in Go. */
        void *slot = (char *)s->data + s->len * elem_size;
        if (!ops) {
            memmove(slot, elem, (size_t)elem_size);
        } else {
            store(slot, elem, elem_size, ops);
        }
        s->len++;
        return;
    }
    /* elem may point into s's own backing: keep it alive across grow(). */
    unsigned char tmp[256];
    unsigned char *copy = (size_t)elem_size <= sizeof tmp ? tmp : zell_alloc(elem_size);
    memcpy(copy, elem, (size_t)elem_size);
    if (ops && ops->retain) ops->retain(copy);
    grow(s, elem_size, ops, 1);
    store((char *)s->data + s->len * elem_size, copy, elem_size, ops);
    if (ops && ops->release) ops->release(copy);
    if (copy != tmp) zell_free(copy);
    s->len++;
}

void zell_slice_append_slice(zell_slice *s, int64_t elem_size, const zell_value_ops *ops,
                              const zell_slice *other) {
    int64_t n = other->len;
    if (n == 0) return;
    /* Snapshot the source (it may alias s's backing), taking a reference per element. */
    unsigned char *tmp = zell_alloc(n * elem_size);
    memcpy(tmp, other->data, (size_t)(n * elem_size));
    if (ops && ops->retain)
        for (int64_t i = 0; i < n; i++) ops->retain(tmp + i * elem_size);
    grow(s, elem_size, ops, n);
    for (int64_t i = 0; i < n; i++) {
        void *slot = (char *)s->data + (s->len + i) * elem_size;
        if (ops && ops->release) ops->release(slot);
        memcpy(slot, tmp + i * elem_size, (size_t)elem_size); /* transfers tmp's reference */
    }
    zell_free(tmp);
    s->len += n;
}

void zell_slice_sub(zell_slice *out, const zell_slice *s, int64_t lo, int64_t hi,
                     int64_t elem_size) {
    if (lo < 0 || hi < lo || hi > s->cap) zell_panic_slice_bounds(lo, hi, s->cap);
    zell_slice r;
    r.backing = s->backing;
    r.len = hi - lo;
    r.cap = s->cap - lo;
    r.data = s->data ? (char *)s->data + lo * elem_size : NULL;
    zell_retain(r.backing);
    /* out == s: in-place reslice, the old reference held by *s is given up.
     * Otherwise *out is overwritten without being released. */
    zell_object *old = (out == s) ? s->backing : NULL;
    *out = r;
    zell_release(old);
}

int64_t zell_slice_copy(zell_slice *dst, const zell_slice *src, int64_t elem_size,
                         const zell_value_ops *ops) {
    int64_t n = dst->len < src->len ? dst->len : src->len;
    if (n == 0) return 0;
    if (ops && ops->retain)
        for (int64_t i = 0; i < n; i++) ops->retain((char *)src->data + i * elem_size);
    if (ops && ops->release)
        for (int64_t i = 0; i < n; i++) ops->release((char *)dst->data + i * elem_size);
    memmove(dst->data, src->data, (size_t)(n * elem_size));
    return n;
}

zell_string *zell_string_from_bytes(const zell_slice *bytes) {
    return zell_string_new(bytes->data ? bytes->data : "", bytes->len);
}

void zell_string_to_bytes(zell_slice *out, const zell_string *s) {
    int64_t n = zell_string_len(s);
    if (n == 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    /* Every byte is written: no need to zero the backing first. */
    check_size(1, n);
    zell_backing *b = (zell_backing *)zell__object_new_uninit(&zell__backing_type, (int64_t)sizeof(zell_backing) + n);
    b->elem_size = 1;
    b->cap = n;
    b->ops = NULL;
    b->_pad = 0;
    memcpy(b->data, zell_string_data(s), (size_t)n);
    out->data = b->data;
    out->len = n;
    out->cap = n;
    out->backing = &b->hdr;
}
