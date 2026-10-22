#include "test.h"
#include <math.h>

ZELL_STATIC_STRING(lit_hello, "hello");
ZELL_STATIC_STRING(lit_comma, ",");
ZELL_STATIC_STRING(lit_a_b, "a,,b");
#define S_LIT(x) (strcmp(x, ",") == 0 ? lit_comma : lit_a_b)

TEST(literals) {
    assert(zell_string_len(lit_hello) == 5);
    assert(STR_IS(lit_hello, "hello"));
    zell_retain(lit_hello);
    zell_release(lit_hello);
    zell_release(lit_hello);
    assert(lit_hello->hdr.refcount == ZELL_RC_IMMORTAL);
    zell_string *l = zell_string_literal("abc", 3);
    zell_release(l);
    assert(STR_IS(l, "abc"));
}

TEST(null_is_empty) {
    assert(zell_string_len(NULL) == 0);
    assert(strcmp(zell_string_data(NULL), "") == 0);
    zell_string *e = S("");
    assert(zell_string_eq(NULL, e));
    assert(zell_string_compare(NULL, e) == 0);
    assert(zell_string_hash(NULL) == zell_string_hash(e));
    zell_string *c = zell_string_concat(NULL, NULL);
    assert(zell_string_len(c) == 0);
    zell_release(c);
    zell_release(e);
}

TEST(concat_compare) {
    int64_t live = zell_live_objects();
    zell_string *a = S("foo"), *b = S("bar");
    zell_string *ab = zell_string_concat(a, b);
    assert(STR_IS(ab, "foobar"));
#ifndef ZELL_STRESS_NONUL
    assert(ab->data[6] == 0);
#endif
    assert(zell_string_compare(a, b) == 1);
    assert(zell_string_compare(b, a) == -1);
    zell_string *fo = S("fo");
    assert(zell_string_compare(fo, a) == -1);
    assert(zell_string_compare(a, fo) == 1);
    assert(!zell_string_eq(a, b));
    zell_string *a2 = S("foo");
    assert(zell_string_eq(a, a2));
    assert(zell_string_hash(a) == zell_string_hash(a2));
    assert(zell_string_hash(a) != zell_string_hash(b));
    zell_release(a); zell_release(b); zell_release(ab); zell_release(fo); zell_release(a2);
    assert(zell_live_objects() == live);
}

TEST(runes) {
    zell_string *s = S("héllo, мир! 😀");
    assert(zell_string_len(s) == 20);
    assert(zell_string_rune_count(s) == 13);
    int64_t w;
    assert(zell_string_decode_rune(s, 1, &w) == 0xE9 && w == 2);
    assert(zell_string_decode_rune(s, 16, &w) == 0x1F600 && w == 4);
    zell_string *bad = zell_string_new("\xff\xfe" "a", 3);
    assert(zell_string_rune_count(bad) == 3);
    assert(zell_string_decode_rune(bad, 0, &w) == 0xFFFD && w == 1);
    zell_string *r = zell_string_from_rune(0x43F);
    assert(STR_IS(r, "п"));
    zell_release(r); zell_release(bad); zell_release(s);
}

TEST(slicing_search) {
    zell_string *s = S("hello world");
    zell_string *w = zell_string_slice(s, 6, 11);
    assert(STR_IS(w, "world"));
    zell_string *e = zell_string_slice(s, 3, 3);
    assert(zell_string_len(e) == 0);
    assert(zell_string_index(s, w) == 6);
    zell_string *x = S("xyz"), *o = S("o");
    assert(zell_string_index(s, x) == -1);
    assert(zell_string_index(s, o) == 4);
    assert(zell_string_index(s, NULL) == 0);
    assert(zell_string_contains(s, w));
    assert(!zell_string_contains(s, x));
    zell_string *h = S("hello");
    assert(zell_string_has_prefix(s, h) && !zell_string_has_prefix(h, s));
    assert(zell_string_has_suffix(s, w) && !zell_string_has_suffix(s, h));
    assert(zell_string_byte_at(s, 4) == 'o');
    zell_release(s); zell_release(w); zell_release(e); zell_release(x);
    zell_release(o); zell_release(h);
}

TEST(int_conv) {
    zell_string *s = zell_string_from_int(-1234567890123LL);
    assert(STR_IS(s, "-1234567890123"));
    zell_error *err = (zell_error *)1;
    assert(zell_string_to_int(s, &err) == -1234567890123LL && err == NULL);
    zell_release(s);
    s = zell_string_from_uint(18446744073709551615ULL);
    assert(STR_IS(s, "18446744073709551615"));
    assert(zell_string_to_int(s, &err) == 0 && err != NULL);
    zell_string *m = zell_error_message(err);
    assert(STR_IS(m, "integer out of range: \"18446744073709551615\""));
    zell_release(m); zell_release(err); zell_release(s);
    const char *bad[] = { "", "12a", " 1", "1 ", "--1", "0x10" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        s = S(bad[i]);
        zell_string_to_int(s, &err);
        assert(err != NULL);
        zell_release(err); zell_release(s);
    }
    s = S("+42");
    assert(zell_string_to_int(s, &err) == 42 && !err);
    zell_release(s);
    s = zell_string_from_bool(true);
    assert(STR_IS(s, "true"));
    zell_release(s);
}

TEST(float_conv) {
    struct { double v; const char *s; } cases[] = {
        { 0.5, "0.5" }, { 3.0, "3" }, { -2.25, "-2.25" }, { 0.1, "0.1" },
        { 1e21, "1e+21" }, { 1.0 / 3.0, "0.3333333333333333" }, { 0.0, "0" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        zell_string *s = zell_string_from_float(cases[i].v);
        assert(STR_IS(s, cases[i].s));
        zell_error *err;
        assert(zell_string_to_float(s, &err) == cases[i].v && !err);
        zell_release(s);
    }
    zell_string *s = zell_string_from_float(NAN);
    assert(STR_IS(s, "NaN"));
    zell_release(s);
    s = zell_string_from_float(-INFINITY);
    assert(STR_IS(s, "-Inf"));
    zell_release(s);
    zell_error *err;
    s = S("1.5x");
    zell_string_to_float(s, &err);
    assert(err);
    zell_release(err); zell_release(s);
}

static void slice_oob(void) {
    zell_string *s = S("abc");
    zell_string_slice(s, 2, 5);
}

TEST(bounds_panic) {
    char buf[4096];
    assert(run_child(slice_oob, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: runtime error: slice bounds out of range [2:5] with capacity 3"));
}

TEST(append_in_place) {
    zell_string *s = S("ab");
    zell_string *x = S("cd");
    s = zell_string_append(s, x);                 /* unique: extended */
    assert(STR_IS(s, "abcd") && zell_refcount(s) == 1);
    for (int i = 0; i < 1000; i++) s = zell_string_append(s, x);
    assert(zell_string_len(s) == 2004);
    s = zell_string_append(s, s);                 /* self-append */
    assert(zell_string_len(s) == 4008 && memcmp(zell_string_data(s) + 2004, "abcd", 4) == 0);
    zell_retain(s);                               /* shared: copied */
    zell_string *t = zell_string_append(s, x);
    assert(t != s && zell_refcount(s) == 1 && zell_string_len(t) == 4010);
    assert(zell_string_append(t, NULL) == t);
    zell_string *lit = zell_string_literal("lit", 3);
    zell_string *u = zell_string_append(lit, x);  /* immortal: copied */
    assert(STR_IS(u, "litcd") && STR_IS(lit, "lit"));
    zell_string *e = zell_string_append(NULL, x);
    assert(STR_IS(e, "cd"));
    zell_release(s); zell_release(t); zell_release(u); zell_release(e); zell_release(x);
}

/* Views (ABI.md 4.1): Split/Fields pieces in a view block, long slices as
 * standalone views, short ones copied; all release their parent. */
TEST(views) {
    int64_t live = zell_live_objects();
    char big[200];
    for (int i = 0; i < 199; i++) big[i] = (char)('a' + i % 26);
    big[199] = 0;
    big[10] = ',';
    big[150] = ',';
    zell_string *p = zell_string_new(big, 199);
    zell_slice parts;
    zell_strings_split(p, S_LIT(","), &parts);
    assert(parts.len == 3);
    zell_string **el = parts.data;
    assert(zell_string_len(el[0]) == 10 && memcmp(el[0]->data, big, 10) == 0);
    assert(el[1]->data == p->data + 11 && zell_rc_is_view(el[1]->hdr.refcount));
    assert(el[1]->data[el[1]->len] == ',');              /* not terminated */
    assert(strlen(zell_string_cstr(el[1])) == 139);      /* terminated copy */
#ifndef ZELL_STRESS_NONUL
    assert(zell_string_cstr(el[2]) == el[2]->data);      /* ends at the parent's NUL */
#endif
    zell_string *keep = el[1];
    zell_retain(keep);                                    /* forwards to the block */
    assert(zell_refcount(keep) == 4);
    zell_release(p);                                      /* the block owns p's bytes */
    zell_slice_release(&parts);
    assert(zell_refcount(keep) == 1 && keep->data[0] == big[11]);
    zell_string *k2 = zell_string_slice(keep, 1, 100);  /* view of a view: owner flattened */
    assert(k2->hdr.type == &zell_type_string_view && k2->data == keep->data + 1);
    zell_string *k3 = zell_string_slice(keep, 1, 5);    /* short: a copy */
    assert(k3->hdr.type == &zell_type_string && k3->data == (char *)(k3 + 1) && STR_IS(k3, "mnop"));
    zell_release(keep);                                   /* block gone, k2 keeps p */
    zell_string *k4 = zell_string_append(k2, k3);        /* a view is never extended in place */
    assert(zell_string_len(k4) == 103 && k4->data == (char *)(k4 + 1));  /* k2 consumed */
    zell_error *e = zell_error_sentinel(k4);             /* copies a non-immortal message */
    assert(e->message != k4);
    zell_release(k3); zell_release(k4);
    zell_slice f;
    zell_string *ws = S("  alpha \t beta\ngamma  ");
    zell_strings_fields(ws, &f);
    zell_string **fe = f.data;
    assert(f.len == 3 && STR_IS(fe[0], "alpha") && STR_IS(fe[1], "beta") && STR_IS(fe[2], "gamma"));
    zell_release(ws);
    zell_slice_release(&f);
    zell_strings_split(S_LIT("a,,b"), S_LIT(","), &f);   /* immortal parent, empty piece NULL */
    fe = f.data;
    assert(f.len == 3 && STR_IS(fe[0], "a") && fe[1] == NULL && STR_IS(fe[2], "b"));
    zell_slice_release(&f);
    assert(zell_live_objects() == live);
}

int main(void) {
    printf("test_string\n");
    int64_t live = zell_live_objects();
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
    /* zell_string_literal makes an immortal object, which does not count */
    assert(zell_live_objects() == live);
    return 0;
}
