/* Tests for std_json.c (tokenizer, quoting) and std_reflect.c (table
 * lookups on a hand-made table laid out as in ABI.md section 15). */
#include "test.h"

static int64_t n_tokens(const zell_slice *s) { return s->len; }
static const zell_string *tok(const zell_slice *s, int64_t i) { return ((zell_string *const *)s->data)[i]; }

TEST(scan_tokens) {
    zell_string *t = S(" {\"a\\u00e9\\n\": [1, -2.5e3, true, false, null], \"\\ud83d\\ude00\":\"\"}\n");
    zell_slice out;
    zell_error *err = (zell_error *)1;
    zell_encoding_json_scan(t, &out, &err);
    assert(err == NULL);
    const char *want[] = { "{", "sa\xc3\xa9\n", ":", "[", "n1", ",", "n-2.5e3", ",", "t", ",", "f", ",", "z", "]", ",", "s\xf0\x9f\x98\x80", ":", "s", "}" };
    assert(n_tokens(&out) == (int64_t)(sizeof want / sizeof want[0]));
    for (int64_t i = 0; i < n_tokens(&out); i++) assert(STR_IS(tok(&out, i), want[i]));
    zell_slice_release(&out);
    zell_release(t);
}

static void expect_error(const char *text, const char *msg) {
    zell_string *t = S(text);
    zell_slice out;
    zell_error *err = NULL;
    zell_encoding_json_scan(t, &out, &err);
    assert(err != NULL);
    assert(out.len == 0 && out.backing == NULL);
    zell_string *m = zell_error_message(err);
    if (!STR_IS(m, msg)) {
        fprintf(stderr, "got [%s] want [%s]\n", zell_string_cstr(m), msg);
        assert(0);
    }
    zell_release(m);
    zell_release(err);
    zell_release(t);
}

TEST(scan_errors) {
    expect_error("{bad", "json: invalid character 'b' at offset 1");
    expect_error("\"abc", "json: unterminated string at offset 4");
    expect_error("-", "json: invalid number at offset 0");
    expect_error("1.", "json: invalid number at offset 0");
    expect_error("\"\\x\"", "json: invalid escape in string at offset 1");
    expect_error("\"a\tb\"", "json: control character in string at offset 2");
    expect_error("\"\\u12\"", "json: invalid \\u escape at offset 3");
    expect_error("nul", "json: invalid character 'n' at offset 0");
}

TEST(quote) {
    zell_string *s = zell_string_new("a\"b\\c\n\x01\xc3\xa9", 9);
    zell_string *q = zell_encoding_json_quote(s);
    assert(STR_IS(q, "\"a\\\"b\\\\c\\n\\u0001\xc3\xa9\""));
    zell_release(q);
    zell_release(s);
    q = zell_encoding_json_quote(NULL);
    assert(STR_IS(q, "\"\""));
    zell_release(q);
}

/* A table for `struct P { X int }` the way the compiler lays it out. */
ZELL_STATIC_STRING(name_int, "int");
ZELL_STATIC_STRING(name_p, "P");
ZELL_STATIC_STRING(name_x, "X");
static zell_reflect_type ti_int = { { "int", 24, NULL }, ZELL_KIND_INT, NULL, NULL, 8, 8, { 0 }, { 0 }, { 0 }, NULL, NULL, 0, { 0 }, NULL, NULL };
static zell_reflect_field field_x = { NULL, &ti_int, 0, 0, false, true, { 0 } };
static zell_reflect_field *fields_p[] = { &field_x };
static zell_reflect_type ti_p = { { "t.P", 24, NULL }, ZELL_KIND_STRUCT, NULL, NULL, 8, 8, { fields_p, 1, 1, NULL }, { 0 }, { 0 }, NULL, NULL, 0, { 0 }, NULL, NULL };
static zell_reflect_type ti_int64 = { { "int", 24, NULL }, ZELL_KIND_INT, NULL, NULL, 8, 8, { 0 }, { 0 }, { 0 }, NULL, NULL, 0, { 0 }, NULL, &ti_int };

TEST(reflect_lookups) {
    ti_int.name = name_int;
    ti_p.name = name_p;
    field_x.name = name_x;
    assert(strcmp(zell_reflect_kind_name(ti_p.kind), "Struct") == 0);
    assert(strcmp(zell_reflect_kind_name(ZELL_KIND_TASK), "Task") == 0);
    assert(strcmp(zell_reflect_kind_name(99), "Invalid") == 0);
    assert(zell_reflect_field_by_name(&ti_p, "X") == &field_x);
    assert(zell_reflect_field_by_name(&ti_p, "Y") == NULL);
    assert(zell_reflect_method_by_name(&ti_p, "X") == NULL);
    assert(zell_reflect_canon(&ti_int64) == &ti_int);
    assert(zell_reflect_canon(&ti_int) == &ti_int);
    /* A box made with a table as its type info. */
    void *obj = zell_object_new(&ti_p.rt, 24);
    assert(zell_reflect_type_of(obj) == &ti_p);
    zell_release(obj);
    assert(zell_reflect_type_of(NULL) == NULL);
}

int main(int argc, char **argv) {
    (void)argc;
    zell_runtime_init(1, argv);
    printf("test_json\n");
    int64_t live = zell_live_objects();
    RUN(scan_tokens);
    RUN(scan_errors);
    RUN(quote);
    RUN(reflect_lookups);
    assert(zell_live_objects() == live);
    return 0;
}
