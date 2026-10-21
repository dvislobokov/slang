#include "test.h"
#include <math.h>

SLANG_STATIC_STRING(lit_hello, "hello");
SLANG_STATIC_STRING(lit_comma, ",");
SLANG_STATIC_STRING(lit_a_b, "a,,b");
#define S_LIT(x) (strcmp(x, ",") == 0 ? lit_comma : lit_a_b)

TEST(literals) {
    assert(slang_string_len(lit_hello) == 5);
    assert(STR_IS(lit_hello, "hello"));
    slang_retain(lit_hello);
    slang_release(lit_hello);
    slang_release(lit_hello);
    assert(lit_hello->hdr.refcount == SLANG_RC_IMMORTAL);
    slang_string *l = slang_string_literal("abc", 3);
    slang_release(l);
    assert(STR_IS(l, "abc"));
}

TEST(null_is_empty) {
    assert(slang_string_len(NULL) == 0);
    assert(strcmp(slang_string_data(NULL), "") == 0);
    slang_string *e = S("");
    assert(slang_string_eq(NULL, e));
    assert(slang_string_compare(NULL, e) == 0);
    assert(slang_string_hash(NULL) == slang_string_hash(e));
    slang_string *c = slang_string_concat(NULL, NULL);
    assert(slang_string_len(c) == 0);
    slang_release(c);
    slang_release(e);
}

TEST(concat_compare) {
    int64_t live = slang_live_objects();
    slang_string *a = S("foo"), *b = S("bar");
    slang_string *ab = slang_string_concat(a, b);
    assert(STR_IS(ab, "foobar"));
#ifndef SLANG_STRESS_NONUL
    assert(ab->data[6] == 0);
#endif
    assert(slang_string_compare(a, b) == 1);
    assert(slang_string_compare(b, a) == -1);
    slang_string *fo = S("fo");
    assert(slang_string_compare(fo, a) == -1);
    assert(slang_string_compare(a, fo) == 1);
    assert(!slang_string_eq(a, b));
    slang_string *a2 = S("foo");
    assert(slang_string_eq(a, a2));
    assert(slang_string_hash(a) == slang_string_hash(a2));
    assert(slang_string_hash(a) != slang_string_hash(b));
    slang_release(a); slang_release(b); slang_release(ab); slang_release(fo); slang_release(a2);
    assert(slang_live_objects() == live);
}

TEST(runes) {
    slang_string *s = S("héllo, мир! 😀");
    assert(slang_string_len(s) == 20);
    assert(slang_string_rune_count(s) == 13);
    int64_t w;
    assert(slang_string_decode_rune(s, 1, &w) == 0xE9 && w == 2);
    assert(slang_string_decode_rune(s, 16, &w) == 0x1F600 && w == 4);
    slang_string *bad = slang_string_new("\xff\xfe" "a", 3);
    assert(slang_string_rune_count(bad) == 3);
    assert(slang_string_decode_rune(bad, 0, &w) == 0xFFFD && w == 1);
    slang_string *r = slang_string_from_rune(0x43F);
    assert(STR_IS(r, "п"));
    slang_release(r); slang_release(bad); slang_release(s);
}

TEST(slicing_search) {
    slang_string *s = S("hello world");
    slang_string *w = slang_string_slice(s, 6, 11);
    assert(STR_IS(w, "world"));
    slang_string *e = slang_string_slice(s, 3, 3);
    assert(slang_string_len(e) == 0);
    assert(slang_string_index(s, w) == 6);
    slang_string *x = S("xyz"), *o = S("o");
    assert(slang_string_index(s, x) == -1);
    assert(slang_string_index(s, o) == 4);
    assert(slang_string_index(s, NULL) == 0);
    assert(slang_string_contains(s, w));
    assert(!slang_string_contains(s, x));
    slang_string *h = S("hello");
    assert(slang_string_has_prefix(s, h) && !slang_string_has_prefix(h, s));
    assert(slang_string_has_suffix(s, w) && !slang_string_has_suffix(s, h));
    assert(slang_string_byte_at(s, 4) == 'o');
    slang_release(s); slang_release(w); slang_release(e); slang_release(x);
    slang_release(o); slang_release(h);
}

TEST(int_conv) {
    slang_string *s = slang_string_from_int(-1234567890123LL);
    assert(STR_IS(s, "-1234567890123"));
    slang_error *err = (slang_error *)1;
    assert(slang_string_to_int(s, &err) == -1234567890123LL && err == NULL);
    slang_release(s);
    s = slang_string_from_uint(18446744073709551615ULL);
    assert(STR_IS(s, "18446744073709551615"));
    assert(slang_string_to_int(s, &err) == 0 && err != NULL);
    slang_string *m = slang_error_message(err);
    assert(STR_IS(m, "integer out of range: \"18446744073709551615\""));
    slang_release(m); slang_release(err); slang_release(s);
    const char *bad[] = { "", "12a", " 1", "1 ", "--1", "0x10" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        s = S(bad[i]);
        slang_string_to_int(s, &err);
        assert(err != NULL);
        slang_release(err); slang_release(s);
    }
    s = S("+42");
    assert(slang_string_to_int(s, &err) == 42 && !err);
    slang_release(s);
    s = slang_string_from_bool(true);
    assert(STR_IS(s, "true"));
    slang_release(s);
}

TEST(float_conv) {
    struct { double v; const char *s; } cases[] = {
        { 0.5, "0.5" }, { 3.0, "3" }, { -2.25, "-2.25" }, { 0.1, "0.1" },
        { 1e21, "1e+21" }, { 1.0 / 3.0, "0.3333333333333333" }, { 0.0, "0" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        slang_string *s = slang_string_from_float(cases[i].v);
        assert(STR_IS(s, cases[i].s));
        slang_error *err;
        assert(slang_string_to_float(s, &err) == cases[i].v && !err);
        slang_release(s);
    }
    slang_string *s = slang_string_from_float(NAN);
    assert(STR_IS(s, "NaN"));
    slang_release(s);
    s = slang_string_from_float(-INFINITY);
    assert(STR_IS(s, "-Inf"));
    slang_release(s);
    slang_error *err;
    s = S("1.5x");
    slang_string_to_float(s, &err);
    assert(err);
    slang_release(err); slang_release(s);
}

static void slice_oob(void) {
    slang_string *s = S("abc");
    slang_string_slice(s, 2, 5);
}

TEST(bounds_panic) {
    char buf[4096];
    assert(run_child(slice_oob, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: runtime error: slice bounds out of range [2:5] with capacity 3"));
}

TEST(append_in_place) {
    slang_string *s = S("ab");
    slang_string *x = S("cd");
    s = slang_string_append(s, x);                 /* unique: extended */
    assert(STR_IS(s, "abcd") && slang_refcount(s) == 1);
    for (int i = 0; i < 1000; i++) s = slang_string_append(s, x);
    assert(slang_string_len(s) == 2004);
    s = slang_string_append(s, s);                 /* self-append */
    assert(slang_string_len(s) == 4008 && memcmp(slang_string_data(s) + 2004, "abcd", 4) == 0);
    slang_retain(s);                               /* shared: copied */
    slang_string *t = slang_string_append(s, x);
    assert(t != s && slang_refcount(s) == 1 && slang_string_len(t) == 4010);
    assert(slang_string_append(t, NULL) == t);
    slang_string *lit = slang_string_literal("lit", 3);
    slang_string *u = slang_string_append(lit, x);  /* immortal: copied */
    assert(STR_IS(u, "litcd") && STR_IS(lit, "lit"));
    slang_string *e = slang_string_append(NULL, x);
    assert(STR_IS(e, "cd"));
    slang_release(s); slang_release(t); slang_release(u); slang_release(e); slang_release(x);
}

/* Views (ABI.md 4.1): Split/Fields pieces in a view block, long slices as
 * standalone views, short ones copied; all release their parent. */
TEST(views) {
    int64_t live = slang_live_objects();
    char big[200];
    for (int i = 0; i < 199; i++) big[i] = (char)('a' + i % 26);
    big[199] = 0;
    big[10] = ',';
    big[150] = ',';
    slang_string *p = slang_string_new(big, 199);
    slang_slice parts;
    slang_strings_split(p, S_LIT(","), &parts);
    assert(parts.len == 3);
    slang_string **el = parts.data;
    assert(slang_string_len(el[0]) == 10 && memcmp(el[0]->data, big, 10) == 0);
    assert(el[1]->data == p->data + 11 && slang_rc_is_view(el[1]->hdr.refcount));
    assert(el[1]->data[el[1]->len] == ',');              /* not terminated */
    assert(strlen(slang_string_cstr(el[1])) == 139);      /* terminated copy */
#ifndef SLANG_STRESS_NONUL
    assert(slang_string_cstr(el[2]) == el[2]->data);      /* ends at the parent's NUL */
#endif
    slang_string *keep = el[1];
    slang_retain(keep);                                    /* forwards to the block */
    assert(slang_refcount(keep) == 4);
    slang_release(p);                                      /* the block owns p's bytes */
    slang_slice_release(&parts);
    assert(slang_refcount(keep) == 1 && keep->data[0] == big[11]);
    slang_string *k2 = slang_string_slice(keep, 1, 100);  /* view of a view: owner flattened */
    assert(k2->hdr.type == &slang_type_string_view && k2->data == keep->data + 1);
    slang_string *k3 = slang_string_slice(keep, 1, 5);    /* short: a copy */
    assert(k3->hdr.type == &slang_type_string && k3->data == (char *)(k3 + 1) && STR_IS(k3, "mnop"));
    slang_release(keep);                                   /* block gone, k2 keeps p */
    slang_string *k4 = slang_string_append(k2, k3);        /* a view is never extended in place */
    assert(slang_string_len(k4) == 103 && k4->data == (char *)(k4 + 1));  /* k2 consumed */
    slang_error *e = slang_error_sentinel(k4);             /* copies a non-immortal message */
    assert(e->message != k4);
    slang_release(k3); slang_release(k4);
    slang_slice f;
    slang_string *ws = S("  alpha \t beta\ngamma  ");
    slang_strings_fields(ws, &f);
    slang_string **fe = f.data;
    assert(f.len == 3 && STR_IS(fe[0], "alpha") && STR_IS(fe[1], "beta") && STR_IS(fe[2], "gamma"));
    slang_release(ws);
    slang_slice_release(&f);
    slang_strings_split(S_LIT("a,,b"), S_LIT(","), &f);   /* immortal parent, empty piece NULL */
    fe = f.data;
    assert(f.len == 3 && STR_IS(fe[0], "a") && fe[1] == NULL && STR_IS(fe[2], "b"));
    slang_slice_release(&f);
    assert(slang_live_objects() == live);
}

int main(void) {
    printf("test_string\n");
    int64_t live = slang_live_objects();
    RUN(literals);
    RUN(append_in_place);
    RUN(null_is_empty);
    RUN(concat_compare);
    RUN(runes);
    RUN(slicing_search);
    RUN(int_conv);
    RUN(float_conv);
    RUN(bounds_panic);
    RUN(views);
    /* slang_string_literal makes an immortal object, which does not count */
    assert(slang_live_objects() == live);
    return 0;
}
