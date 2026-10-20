#include "test.h"

typedef struct { int32_t a; int32_t b; } pair;
static uint64_t pair_hash(const void *k) { return slang_hash_bytes(k, sizeof(pair)); }
static bool pair_eq(const void *x, const void *y) { return memcmp(x, y, sizeof(pair)) == 0; }

TEST(int_map) {
    slang_map *m = slang_map_new_int(8, NULL);
    for (int64_t i = 0; i < 10000; i++) {
        int64_t v = i * i;
        slang_map_set_int(m, i, &v);
    }
    assert(slang_map_len(m) == 10000);
    for (int64_t i = 0; i < 10000; i++) assert(*(int64_t *)slang_map_get_int(m, i) == i * i);
    assert(slang_map_get_int(m, 10000) == NULL);
    int64_t v = -1;
    slang_map_set_int(m, 5, &v);
    assert(*(int64_t *)slang_map_get_int(m, 5) == -1 && slang_map_len(m) == 10000);
    for (int64_t i = 0; i < 10000; i += 2) assert(slang_map_delete_int(m, i));
    assert(!slang_map_delete_int(m, 0));
    assert(slang_map_len(m) == 5000);
    int64_t *slot = slang_map_get_or_insert(m, &(int64_t){ 0 });
    assert(*slot == 0);
    *slot += 3;
    assert(*(int64_t *)slang_map_get_int(m, 0) == 3);
    slang_release(m);
}

TEST(str_map) {
    int64_t live = slang_live_objects();
    slang_map *m = slang_map_new_str(sizeof(slang_string *), &slang_ops_ref);
    slang_string *k = S("key"), *v = S("value"), *k2 = S("key");
    slang_map_set_str(m, k, &v);
    assert(slang_refcount(k) == 2 && slang_refcount(v) == 2);
    slang_string **got = slang_map_get_str(m, k2);
    assert(got && *got == v);
    slang_map_set_str(m, k2, &v);              /* same value again: no leak/underflow */
    assert(slang_refcount(v) == 2 && slang_refcount(k2) == 1);
    assert(slang_map_get_str(m, NULL) == NULL);
    slang_map_set_str(m, NULL, &k);             /* "" key */
    assert(*(slang_string **)slang_map_get_str(m, NULL) == k);
    assert(slang_map_len(m) == 2);
    assert(slang_map_delete_str(m, k2));
    assert(slang_refcount(k) == 2);             /* held as the "" key's value */
    slang_release(m);
    assert(slang_refcount(k) == 1 && slang_refcount(v) == 1);
    slang_release(k); slang_release(v); slang_release(k2);
    assert(slang_live_objects() == live);
}

TEST(generic_map_iter) {
    slang_map_desc d = { sizeof(pair), sizeof(double), pair_hash, pair_eq, NULL, NULL };
    slang_map *m = slang_map_new(&d, 100);
    for (int i = 0; i < 100; i++) {
        pair p = { i, -i };
        double v = i / 2.0;
        slang_map_set(m, &p, &v);
    }
    slang_map_iter it;
    slang_map_iter_init(&it, m);
    void *k, *v;
    int count = 0;
    double sum = 0;
    while (slang_map_iter_next(&it, &k, &v)) {
        pair *p = k;
        assert(p->a == -p->b);
        sum += *(double *)v;
        if (p->a % 3 == 0) slang_map_delete(m, p);  /* deleting current entry is allowed */
        count++;
    }
    assert(count == 100 && sum == 99 * 100 / 4.0);
    assert(!slang_map_iter_next(&it, &k, &v));
    assert(slang_map_len(m) == 100 - 34);
    slang_release(m);
}

static void nil_map_write(void) {
    int64_t v = 1;
    slang_map_set_int(NULL, 1, &v);
}

TEST(nil_map) {
    assert(slang_map_len(NULL) == 0);
    assert(slang_map_get_int(NULL, 1) == NULL);
    assert(!slang_map_delete_int(NULL, 1));
    slang_map_iter it;
    slang_map_iter_init(&it, NULL);
    assert(!slang_map_iter_next(&it, NULL, NULL));
    char buf[4096];
    assert(run_child(nil_map_write, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: assignment to entry in nil map"));
}

int main(void) {
    printf("test_map\n");
    int64_t live = slang_live_objects();
    RUN(int_map);
    RUN(str_map);
    RUN(generic_map_iter);
    RUN(nil_map);
    assert(slang_live_objects() == live);
    return 0;
}
