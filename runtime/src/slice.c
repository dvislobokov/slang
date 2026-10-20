/* slice.c - Go-like slices over a refcounted backing array. */
#include "rt_internal.h"

#include <string.h>

/* Backing array object (internal, but layout documented in ABI.md). */
typedef struct slang_backing {
    slang_object hdr;             /* +0  */
    int64_t elem_size;            /* +16 */
    int64_t cap;                  /* +24 elements */
    const slang_value_ops *ops;   /* +32 */
    int64_t _pad;                 /* +40 keeps data 16-byte aligned */
    unsigned char data[];         /* +48 */
} slang_backing;

static void backing_drop(void *obj) {
    slang_backing *b = obj;
    if (!b->ops || !b->ops->release) return;
    for (int64_t i = 0; i < b->cap; i++) b->ops->release(b->data + i * b->elem_size);
}

static const slang_type_info backing_type = { "[]backing", 0, backing_drop };

static void check_size(int64_t elem_size, int64_t n) {
    if (elem_size < 0 || n < 0) slang__panicf("invalid slice size (elem %lld, n %lld)",
                                              (long long)elem_size, (long long)n);
    if (elem_size > 0 && n > (INT64_MAX - 64) / elem_size)
        slang__panicf("slice too large (%lld elements of %lld bytes)", (long long)n,
                      (long long)elem_size);
}

static slang_backing *backing_new(int64_t elem_size, int64_t cap, const slang_value_ops *ops) {
    check_size(elem_size, cap);
    slang_backing *b = slang_object_new(&backing_type,
                                        (int64_t)sizeof(slang_backing) + elem_size * cap);
    b->elem_size = elem_size;
    b->cap = cap;
    b->ops = ops;
    return b;
}

void slang_slice_new_ops(slang_slice *out, int64_t elem_size, int64_t len, int64_t cap,
                         const slang_value_ops *ops) {
    if (len < 0 || cap < len)
        slang__panicf("makeslice: len %lld out of range (cap %lld)", (long long)len,
                      (long long)cap);
    if (cap == 0) { memset(out, 0, sizeof *out); return; }
    slang_backing *b = backing_new(elem_size, cap, ops);
    out->data = b->data;
    out->len = len;
    out->cap = cap;
    out->backing = &b->hdr;
}

void slang_slice_new(slang_slice *out, int64_t elem_size, int64_t len, int64_t cap) {
    slang_slice_new_ops(out, elem_size, len, cap, NULL);
}

void slang_slice_from(slang_slice *out, int64_t elem_size, const slang_value_ops *ops,
                      const void *elems, int64_t n) {
    slang_slice_new_ops(out, elem_size, n, n, ops);
    if (n == 0) return;
    memcpy(out->data, elems, (size_t)(elem_size * n));
    if (ops && ops->retain)
        for (int64_t i = 0; i < n; i++) ops->retain((char *)out->data + i * elem_size);
}

void slang_slice_retain(const slang_slice *s) { slang_retain(s->backing); }

void slang_slice_release(slang_slice *s) {
    slang_object *b = s->backing;
    memset(s, 0, sizeof *s);
    slang_release(b);
}

void slang_slice_assign(slang_slice *dst, const slang_slice *src) {
    if (dst == src) return;
    slang_object *old = dst->backing;
    slang_retain(src->backing);
    *dst = *src;
    slang_release(old);
}

void *slang_slice_index(const slang_slice *s, int64_t i, int64_t elem_size) {
    if (i < 0 || i >= s->len) slang_panic_bounds(i, s->len);
    return (char *)s->data + i * elem_size;
}

/* Ensure room for `extra` more elements, reallocating (copy + retain) if needed. */
static void grow(slang_slice *s, int64_t elem_size, const slang_value_ops *ops, int64_t extra) {
    int64_t need = s->len + extra;
    if (need < s->len) slang__panicf("slice length overflow");
    if (need <= s->cap) return;
    int64_t ncap = s->cap < 4 ? 4 : s->cap;
    while (ncap < need) {
        if (ncap > INT64_MAX / 2) { ncap = need; break; }
        ncap = ncap < 1024 ? ncap * 2 : ncap + ncap / 2;
    }
    slang_backing *b = backing_new(elem_size, ncap, ops);
    if (s->len > 0) {
        memcpy(b->data, s->data, (size_t)(s->len * elem_size));
        if (ops && ops->retain)
            for (int64_t i = 0; i < s->len; i++) ops->retain(b->data + i * elem_size);
    }
    slang_object *old = s->backing;
    s->data = b->data;
    s->cap = ncap;
    s->backing = &b->hdr;
    slang_release(old);
}

/* Store a borrowed value into a slot owned by the backing array. */
static void store(void *slot, const void *val, int64_t elem_size, const slang_value_ops *ops) {
    if (ops && ops->retain) ops->retain((void *)val);  /* retain first: val may alias slot */
    if (ops && ops->release) ops->release(slot);
    memmove(slot, val, (size_t)elem_size);
}

void slang_slice_append(slang_slice *s, int64_t elem_size, const slang_value_ops *ops,
                        const void *elem) {
    /* elem may point into s's own backing: keep it alive across grow(). */
    unsigned char tmp[256];
    unsigned char *copy = (size_t)elem_size <= sizeof tmp ? tmp : slang_alloc(elem_size);
    memcpy(copy, elem, (size_t)elem_size);
    if (ops && ops->retain) ops->retain(copy);
    grow(s, elem_size, ops, 1);
    store((char *)s->data + s->len * elem_size, copy, elem_size, ops);
    if (ops && ops->release) ops->release(copy);
    if (copy != tmp) slang_free(copy);
    s->len++;
}

void slang_slice_append_slice(slang_slice *s, int64_t elem_size, const slang_value_ops *ops,
                              const slang_slice *other) {
    int64_t n = other->len;
    if (n == 0) return;
    /* Snapshot the source (it may alias s's backing), taking a reference per element. */
    unsigned char *tmp = slang_alloc(n * elem_size);
    memcpy(tmp, other->data, (size_t)(n * elem_size));
    if (ops && ops->retain)
        for (int64_t i = 0; i < n; i++) ops->retain(tmp + i * elem_size);
    grow(s, elem_size, ops, n);
    for (int64_t i = 0; i < n; i++) {
        void *slot = (char *)s->data + (s->len + i) * elem_size;
        if (ops && ops->release) ops->release(slot);
        memcpy(slot, tmp + i * elem_size, (size_t)elem_size); /* transfers tmp's reference */
    }
    slang_free(tmp);
    s->len += n;
}

void slang_slice_sub(slang_slice *out, const slang_slice *s, int64_t lo, int64_t hi,
                     int64_t elem_size) {
    if (lo < 0 || hi < lo || hi > s->cap) slang_panic_slice_bounds(lo, hi, s->cap);
    slang_slice r;
    r.backing = s->backing;
    r.len = hi - lo;
    r.cap = s->cap - lo;
    r.data = s->data ? (char *)s->data + lo * elem_size : NULL;
    slang_retain(r.backing);
    /* out == s: in-place reslice, the old reference held by *s is given up.
     * Otherwise *out is overwritten without being released. */
    slang_object *old = (out == s) ? s->backing : NULL;
    *out = r;
    slang_release(old);
}

int64_t slang_slice_copy(slang_slice *dst, const slang_slice *src, int64_t elem_size,
                         const slang_value_ops *ops) {
    int64_t n = dst->len < src->len ? dst->len : src->len;
    if (n == 0) return 0;
    if (ops && ops->retain)
        for (int64_t i = 0; i < n; i++) ops->retain((char *)src->data + i * elem_size);
    if (ops && ops->release)
        for (int64_t i = 0; i < n; i++) ops->release((char *)dst->data + i * elem_size);
    memmove(dst->data, src->data, (size_t)(n * elem_size));
    return n;
}

slang_string *slang_string_from_bytes(const slang_slice *bytes) {
    return slang_string_new(bytes->data ? bytes->data : "", bytes->len);
}

void slang_string_to_bytes(slang_slice *out, const slang_string *s) {
    int64_t n = slang_string_len(s);
    slang_slice_new(out, 1, n, n);
    if (n) memcpy(out->data, slang_string_data(s), (size_t)n);
}
