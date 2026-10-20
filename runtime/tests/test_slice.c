#include "test.h"

#define I64 ((int64_t)sizeof(int64_t))
#define PTR ((int64_t)sizeof(void *))

TEST(new_index) {
    slang_slice s;
    slang_slice_new(&s, I64, 3, 10);
    assert(s.len == 3 && s.cap == 10 && s.data && s.backing);
    for (int i = 0; i < 3; i++) assert(*(int64_t *)slang_slice_index(&s, i, I64) == 0);
    *(int64_t *)slang_slice_index(&s, 2, I64) = 7;
    assert(((int64_t *)s.data)[2] == 7);
    slang_slice_release(&s);
    assert(s.backing == NULL && s.len == 0);
    slang_slice z;
    slang_slice_new(&z, I64, 0, 0);
    assert(z.backing == NULL && z.data == NULL);
}

TEST(append_growth) {
    slang_slice s = {0};
    for (int64_t i = 0; i < 1000; i++) slang_slice_append(&s, I64, NULL, &i);
    assert(s.len == 1000 && s.cap >= 1000);
    for (int64_t i = 0; i < 1000; i++) assert(((int64_t *)s.data)[i] == i);
    slang_slice_release(&s);
}

TEST(shared_backing) {
    slang_slice a = {0};
    for (int64_t i = 0; i < 4; i++) slang_slice_append(&a, I64, NULL, &i);
    assert(a.cap == 4);
    slang_slice b;
    slang_slice_sub(&b, &a, 1, 3, I64);
    assert(b.len == 2 && b.cap == 3 && ((int64_t *)b.data)[0] == 1);
    assert(slang_refcount(a.backing) == 2);
    int64_t v = 99;
    slang_slice_append(&b, I64, NULL, &v);       /* fits: overwrites a[3] like Go */
    assert(((int64_t *)a.data)[3] == 99 && b.backing == a.backing);
    slang_slice_append(&b, I64, NULL, &v);       /* grows: new backing */
    assert(b.backing != a.backing && slang_refcount(a.backing) == 1);
    slang_slice_sub(&b, &b, 1, 2, I64);           /* in place */
    assert(b.len == 1 && ((int64_t *)b.data)[0] == 2);
    slang_slice_release(&a);
    slang_slice_release(&b);
}

TEST(copy) {
    int64_t src_vals[] = { 1, 2, 3, 4, 5 };
    slang_slice s, d;
    slang_slice_from(&s, I64, NULL, src_vals, 5);
    slang_slice_new(&d, I64, 3, 3);
    assert(slang_slice_copy(&d, &s, I64, NULL) == 3);
    assert(((int64_t *)d.data)[2] == 3);
    slang_slice tail;
    slang_slice_sub(&tail, &s, 1, 5, I64);
    assert(slang_slice_copy(&tail, &s, I64, NULL) == 4);    /* overlapping */
    int64_t want[] = { 1, 1, 2, 3, 4 };
    assert(memcmp(s.data, want, sizeof want) == 0);
    slang_slice_release(&tail);
    slang_slice_release(&s);
    slang_slice_release(&d);
}

TEST(refcounted_elements) {
    int64_t live = slang_live_objects();
    slang_string *x = S("x"), *y = S("y");
    slang_slice s = {0};
    slang_slice_append(&s, PTR, &slang_ops_ref, &x);
    slang_slice_append(&s, PTR, &slang_ops_ref, &y);
    assert(slang_refcount(x) == 2 && slang_refcount(y) == 2);
    slang_slice t;
    slang_slice c = {0};
    slang_slice_assign(&c, &s);
    assert(c.backing == s.backing && slang_refcount(s.backing) == 2);
    slang_slice_release(&c);
    slang_slice_sub(&t, &s, 0, 2, PTR);
    for (int i = 0; i < 10; i++) slang_slice_append(&t, PTR, &slang_ops_ref, &x); /* t grows */
    /* x: var + s[0] (2), t's 2 in-place appends into s's spare slots (+2),
     * growth copies 3 x's into t's new backing (+3), 8 more appends (+8). */
    assert(slang_refcount(x) == 15 && slang_refcount(y) == 3);
    slang_slice_append_slice(&s, PTR, &slang_ops_ref, &s);  /* self append */
    assert(s.len == 4);
    assert(*(slang_string **)slang_slice_index(&s, 3, PTR) == y);
    slang_slice_release(&t);
    slang_slice_release(&s);
    assert(slang_refcount(x) == 1 && slang_refcount(y) == 1);
    slang_release(x);
    slang_release(y);
    assert(slang_live_objects() == live);
}

TEST(append_slice_overlap) {
    int64_t v[] = { 1, 2, 3, 4 };
    slang_slice s, head, mid;
    slang_slice_from(&s, I64, NULL, v, 4);
    slang_slice_sub(&head, &s, 0, 1, I64);
    slang_slice_sub(&mid, &s, 0, 3, I64);
    slang_slice_append_slice(&head, I64, NULL, &mid);  /* Go: append(s[:1], s[:3]...) */
    int64_t want[] = { 1, 1, 2, 3 };
    assert(head.len == 4 && memcmp(head.data, want, sizeof want) == 0);
    slang_slice_release(&s); slang_slice_release(&head); slang_slice_release(&mid);
}

TEST(bytes) {
    slang_string *str = S("abc");
    slang_slice b;
    slang_string_to_bytes(&b, str);
    assert(b.len == 3 && ((char *)b.data)[1] == 'b');
    ((char *)b.data)[1] = 'X';
    slang_string *back = slang_string_from_bytes(&b);
    assert(STR_IS(back, "aXc") && STR_IS(str, "abc"));
    slang_release(back); slang_release(str); slang_slice_release(&b);
}

static void index_oob(void) {
    slang_slice s;
    slang_slice_new(&s, 8, 3, 3);
    slang_slice_index(&s, 3, 8);
}
static void sub_oob(void) {
    slang_slice s;
    slang_slice_new(&s, 8, 3, 4);
    slang_slice t;
    slang_slice_sub(&t, &s, 2, 5, 8);
}

TEST(bounds_panics) {
    char buf[4096];
    assert(run_child(index_oob, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: runtime error: index out of range [3] with length 3"));
    assert(run_child(sub_oob, buf, sizeof buf) == 2);
    assert(strstr(buf, "slice bounds out of range [2:5] with capacity 4"));
}

int main(void) {
    printf("test_slice\n");
    int64_t live = slang_live_objects();
    RUN(new_index);
    RUN(append_growth);
    RUN(shared_backing);
    RUN(copy);
    RUN(refcounted_elements);
    RUN(append_slice_overlap);
    RUN(bytes);
    RUN(bounds_panics);
    assert(slang_live_objects() == live);
    return 0;
}
