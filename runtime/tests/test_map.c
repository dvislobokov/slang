#include "test.h"

typedef struct { int32_t a; int32_t b; } pair;
static uint64_t pair_hash(const void *k) { return zell_hash_bytes(k, sizeof(pair)); }
static bool pair_eq(const void *x, const void *y) { return memcmp(x, y, sizeof(pair)) == 0; }

TEST(int_map) {
    zell_map *m = zell_map_new_int(8, NULL);
    for (int64_t i = 0; i < 10000; i++) {
        int64_t v = i * i;
        zell_map_set_int(m, i, &v);
    }
    assert(zell_map_len(m) == 10000);
    for (int64_t i = 0; i < 10000; i++) assert(*(int64_t *)zell_map_get_int(m, i) == i * i);
    assert(zell_map_get_int(m, 10000) == NULL);
    int64_t v = -1;
    zell_map_set_int(m, 5, &v);
    assert(*(int64_t *)zell_map_get_int(m, 5) == -1 && zell_map_len(m) == 10000);
    for (int64_t i = 0; i < 10000; i += 2) assert(zell_map_delete_int(m, i));
    assert(!zell_map_delete_int(m, 0));
    assert(zell_map_len(m) == 5000);
    int64_t *slot = zell_map_get_or_insert(m, &(int64_t){ 0 });
    assert(*slot == 0);
    *slot += 3;
    assert(*(int64_t *)zell_map_get_int(m, 0) == 3);
    zell_release(m);
}

TEST(str_map) {
    int64_t live = zell_live_objects();
    zell_map *m = zell_map_new_str(sizeof(zell_string *), &zell_ops_ref);
    zell_string *k = S("key"), *v = S("value"), *k2 = S("key");
    zell_map_set_str(m, k, &v);
    assert(zell_refcount(k) == 2 && zell_refcount(v) == 2);
    zell_string **got = zell_map_get_str(m, k2);
    assert(got && *got == v);
    zell_map_set_str(m, k2, &v);              /* same value again: no leak/underflow */
    assert(zell_refcount(v) == 2 && zell_refcount(k2) == 1);
    assert(zell_map_get_str(m, NULL) == NULL);
    zell_map_set_str(m, NULL, &k);             /* "" key */
    assert(*(zell_string **)zell_map_get_str(m, NULL) == k);
    assert(zell_map_len(m) == 2);
    assert(zell_map_delete_str(m, k2));
    assert(zell_refcount(k) == 2);             /* held as the "" key's value */
    zell_release(m);
    assert(zell_refcount(k) == 1 && zell_refcount(v) == 1);
    zell_release(k); zell_release(v); zell_release(k2);
    assert(zell_live_objects() == live);
}

TEST(generic_map_iter) {
    zell_map_desc d = { sizeof(pair), sizeof(double), pair_hash, pair_eq, NULL, NULL };
    zell_map *m = zell_map_new(&d, 100);
    for (int i = 0; i < 100; i++) {
        pair p = { i, -i };
        double v = i / 2.0;
        zell_map_set(m, &p, &v);
    }
    zell_map_iter it;
    zell_map_iter_init(&it, m);
    void *k, *v;
    int count = 0;
    double sum = 0;
    while (zell_map_iter_next(&it, &k, &v)) {
        pair *p = k;
        assert(p->a == -p->b);
        sum += *(double *)v;
        if (p->a % 3 == 0) zell_map_delete(m, p);  /* deleting current entry is allowed */
        count++;
    }
    assert(count == 100 && sum == 99 * 100 / 4.0);
    assert(!zell_map_iter_next(&it, &k, &v));
    assert(zell_map_len(m) == 100 - 34);
    zell_release(m);
}

/* Insert/delete churn: tombstones must be recycled and lookups stay exact. */
TEST(churn) {
    zell_map *m = zell_map_new_int(8, NULL);
    for (int64_t round = 0; round < 50; round++) {
        for (int64_t i = 0; i < 1000; i++) {
            int64_t *slot = zell_map_get_or_insert_int(m, round * 1000 + i);
            assert(*slot == 0);
            *slot = i;
        }
        for (int64_t i = 0; i < 1000; i++) assert(zell_map_delete_int(m, round * 1000 + i));
        assert(zell_map_len(m) == 0);
    }
    for (int64_t i = 0; i < 100000; i++) *(int64_t *)zell_map_get_or_insert_int(m, i * 7919) += i;
    for (int64_t i = 0; i < 100000; i++) assert(*(int64_t *)zell_map_get_int(m, i * 7919) == i);
    assert(zell_map_get_int(m, 3) == NULL && zell_map_len(m) == 100000);
    zell_release(m);
}

/* String keys: get_or_insert_str retains a new key once; iteration sees
 * every entry exactly once; values are released on delete and drop. */
TEST(str_map_bulk) {
    int64_t live = zell_live_objects();
    zell_map *m = zell_map_new_str(sizeof(zell_string *), &zell_ops_ref);
    char buf[32];
    for (int i = 0; i < 5000; i++) {
        int n = snprintf(buf, sizeof buf, "key-%d", i);
        zell_string *k = zell_string_new(buf, n);
        zell_string *v = zell_string_from_int(i);
        zell_string **slot = zell_map_get_or_insert_str(m, k);
        assert(*slot == NULL);
        zell_retain(v);
        *slot = v;
        assert(zell_map_get_or_insert_str(m, k) == (void *)slot);
        zell_release(k);
        zell_release(v);
    }
    assert(zell_map_len(m) == 5000);
    zell_map_iter it;
    zell_map_iter_init(&it, m);
    void *k, *v;
    int count = 0;
    while (zell_map_iter_next(&it, &k, &v)) {
        zell_string *ks = *(zell_string **)k;
        int64_t idx = zell_string_to_int(*(zell_string **)v, NULL);
        int n = snprintf(buf, sizeof buf, "key-%lld", (long long)idx);
        assert(zell_string_len(ks) == n && memcmp(zell_string_data(ks), buf, (size_t)n) == 0);
        if (idx % 2) assert(zell_map_delete_str(m, ks));
        count++;
    }
    assert(count == 5000 && zell_map_len(m) == 2500);
    zell_string *probe = zell_string_new("key-42", 6);
    assert(zell_map_get_str(m, probe) != NULL);
    zell_release(probe);
    zell_release(m);
    assert(zell_live_objects() == live);
}

static void nil_map_write(void) {
    int64_t v = 1;
    zell_map_set_int(NULL, 1, &v);
}

TEST(nil_map) {
    assert(zell_map_len(NULL) == 0);
    assert(zell_map_get_int(NULL, 1) == NULL);
    assert(!zell_map_delete_int(NULL, 1));
    zell_map_iter it;
    zell_map_iter_init(&it, NULL);
    assert(!zell_map_iter_next(&it, NULL, NULL));
    char buf[4096];
    assert(run_child(nil_map_write, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: assignment to entry in nil map"));
}

int main(void) {
    printf("test_map\n");
    int64_t live = zell_live_objects();
    RUN(int_map);
    RUN(str_map);
    RUN(generic_map_iter);
    RUN(nil_map);
    RUN(churn);
    RUN(str_map_bulk);
    assert(zell_live_objects() == live);
    return 0;
}
