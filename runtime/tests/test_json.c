/* Tests for std_json.c (tokenizer, quoting) and std_reflect.c (table
 * lookups on a hand-made table laid out as in ABI.md section 15). */
#include "test.h"

static int64_t n_tokens(const slang_slice *s) { return s->len; }
static const slang_string *tok(const slang_slice *s, int64_t i) { return ((slang_string *const *)s->data)[i]; }

TEST(scan_tokens) {
    slang_string *t = S(" {\"a\\u00e9\\n\": [1, -2.5e3, true, false, null], \"\\ud83d\\ude00\":\"\"}\n");
    slang_slice out;
    slang_error *err = (slang_error *)1;
    slang_encoding_json_scan(t, &out, &err);
    assert(err == NULL);
    const char *want[] = { "{", "sa\xc3\xa9\n", ":", "[", "n1", ",", "n-2.5e3", ",", "t", ",", "f", ",", "z", "]", ",", "s\xf0\x9f\x98\x80", ":", "s", "}" };
    assert(n_tokens(&out) == (int64_t)(sizeof want / sizeof want[0]));
    for (int64_t i = 0; i < n_tokens(&out); i++) assert(STR_IS(tok(&out, i), want[i]));
    slang_slice_release(&out);
    slang_release(t);
}

static void expect_error(const char *text, const char *msg) {
    slang_string *t = S(text);
    slang_slice out;
    slang_error *err = NULL;
    slang_encoding_json_scan(t, &out, &err);
    assert(err != NULL);
    assert(out.len == 0 && out.backing == NULL);
    slang_string *m = slang_error_message(err);
    if (!STR_IS(m, msg)) {
        fprintf(stderr, "got [%s] want [%s]\n", slang_string_cstr(m), msg);
        assert(0);
    }
    slang_release(m);
    slang_release(err);
    slang_release(t);
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
    slang_string *s = slang_string_new("a\"b\\c\n\x01\xc3\xa9", 9);
    slang_string *q = slang_encoding_json_quote(s);
    assert(STR_IS(q, "\"a\\\"b\\\\c\\n\\u0001\xc3\xa9\""));
    slang_release(q);
    slang_release(s);
    q = slang_encoding_json_quote(NULL);
    assert(STR_IS(q, "\"\""));
    slang_release(q);
}

/* A table for `struct P { X int }` the way the compiler lays it out. */
SLANG_STATIC_STRING(name_int, "int");
SLANG_STATIC_STRING(name_p, "P");
SLANG_STATIC_STRING(name_x, "X");
static slang_reflect_type ti_int = { { "int", 24, NULL }, SLANG_KIND_INT, NULL, NULL, 8, 8, { 0 }, { 0 }, { 0 }, NULL, NULL, 0, { 0 }, NULL, NULL };
static slang_reflect_field field_x = { NULL, &ti_int, 0, 0, false, true, { 0 } };
static slang_reflect_field *fields_p[] = { &field_x };
static slang_reflect_type ti_p = { { "t.P", 24, NULL }, SLANG_KIND_STRUCT, NULL, NULL, 8, 8, { fields_p, 1, 1, NULL }, { 0 }, { 0 }, NULL, NULL, 0, { 0 }, NULL, NULL };
static slang_reflect_type ti_int64 = { { "int", 24, NULL }, SLANG_KIND_INT, NULL, NULL, 8, 8, { 0 }, { 0 }, { 0 }, NULL, NULL, 0, { 0 }, NULL, &ti_int };

TEST(reflect_lookups) {
    ti_int.name = name_int;
    ti_p.name = name_p;
    field_x.name = name_x;
    assert(strcmp(slang_reflect_kind_name(ti_p.kind), "Struct") == 0);
    assert(strcmp(slang_reflect_kind_name(SLANG_KIND_TASK), "Task") == 0);
    assert(strcmp(slang_reflect_kind_name(99), "Invalid") == 0);
    assert(slang_reflect_field_by_name(&ti_p, "X") == &field_x);
    assert(slang_reflect_field_by_name(&ti_p, "Y") == NULL);
    assert(slang_reflect_method_by_name(&ti_p, "X") == NULL);
    assert(slang_reflect_canon(&ti_int64) == &ti_int);
    assert(slang_reflect_canon(&ti_int) == &ti_int);
    /* A box made with a table as its type info. */
    void *obj = slang_object_new(&ti_p.rt, 24);
    assert(slang_reflect_type_of(obj) == &ti_p);
    slang_release(obj);
    assert(slang_reflect_type_of(NULL) == NULL);
}

int main(int argc, char **argv) {
    (void)argc;
    slang_runtime_init(1, argv);
    printf("test_json\n");
    int64_t live = slang_live_objects();
    RUN(scan_tokens);
    RUN(scan_errors);
    RUN(quote);
    RUN(reflect_lookups);
    assert(slang_live_objects() == live);
    return 0;
}
