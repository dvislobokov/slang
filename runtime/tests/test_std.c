#include "test.h"
#include <math.h>

/* []string from C strings (owned slice, each element owned by the backing). */
static void strs(slang_slice *out, int n, const char *const *v) {
    slang_slice_new_ops(out, 8, n, n, &slang_ops_ref);
    for (int i = 0; i < n; i++) ((slang_string **)out->data)[i] = S(v[i]);
}

static bool slice_is(const slang_slice *s, int n, const char *const *v) {
    if (s->len != n) return false;
    for (int i = 0; i < n; i++)
        if (!STR_IS(((slang_string **)s->data)[i], v[i])) return false;
    return true;
}

#define SPLIT_IS(s, sep, ...) do {                                               \
        static const char *const exp_[] = { __VA_ARGS__ };                       \
        slang_string *a_ = S(s), *b_ = S(sep);                                   \
        slang_slice r_;                                                          \
        slang_strings_split(a_, b_, &r_);                                        \
        assert(slice_is(&r_, (int)(sizeof exp_ / sizeof *exp_), exp_));          \
        slang_slice_release(&r_); slang_release(a_); slang_release(b_);          \
    } while (0)

/* Check a string-returning call and release its result. */
#define EXPECT_STR(call, lit) do { slang_string *r_ = (call); assert(r_ != NULL); \
        assert(STR_IS(r_, lit)); slang_release(r_); } while (0)

TEST(split) {
    SPLIT_IS("red,green,blue", ",", "red", "green", "blue");
    SPLIT_IS("a,b,", ",", "a", "b", "");
    SPLIT_IS(",a", ",", "", "a");
    SPLIT_IS("abc", "x", "abc");                 /* no match */
    SPLIT_IS("", ",", "");                       /* Go: [""] */
    SPLIT_IS("a--b----c", "--", "a", "b", "", "c");
    SPLIT_IS("aaa", "aa", "", "a");              /* non-overlapping */
    SPLIT_IS("abc", "abc", "", "");
    SPLIT_IS("héllo", "", "h", "é", "l", "l", "o"); /* empty sep: per UTF-8 sequence */
    /* empty s with empty sep -> empty slice */
    slang_slice r;
    slang_strings_split(NULL, NULL, &r);
    assert(r.len == 0);
    slang_slice_release(&r);
    /* invalid UTF-8 byte becomes a 1-byte element */
    slang_string *bad = slang_string_new("a\xff", 2);
    slang_strings_split(bad, NULL, &r);
    assert(r.len == 2 && slang_string_len(((slang_string **)r.data)[1]) == 2 - 1);
    assert((unsigned char)slang_string_data(((slang_string **)r.data)[1])[0] == 0xff);
    slang_slice_release(&r);
    slang_release(bad);
    /* result is a proper []string: appendable, elements owned */
    slang_string *s = S("x y"), *sp = S(" ");
    slang_strings_split(s, sp, &r);
    slang_string *z = S("z");
    slang_slice_append(&r, 8, &slang_ops_ref, &z);
    slang_release(z);
    static const char *const e3[] = { "x", "y", "z" };
    assert(slice_is(&r, 3, e3));
    slang_slice_release(&r);
    slang_release(s); slang_release(sp);
}

TEST(join) {
    static const char *const v[] = { "red", "green", "blue" };
    slang_slice parts;
    strs(&parts, 3, v);
    slang_string *sep = S(" | ");
    EXPECT_STR(slang_strings_join(&parts, sep), "red | green | blue");
    EXPECT_STR(slang_strings_join(&parts, NULL), "redgreenblue");
    slang_slice one; static const char *const v1[] = { "solo" };
    strs(&one, 1, v1);
    EXPECT_STR(slang_strings_join(&one, sep), "solo");
    slang_slice empty = { 0 };
    EXPECT_STR(slang_strings_join(&empty, sep), "");
    /* NULL elements (zero-value strings) are "" */
    slang_slice holes;
    slang_slice_new_ops(&holes, 8, 3, 3, &slang_ops_ref);
    ((slang_string **)holes.data)[1] = S("m");
    EXPECT_STR(slang_strings_join(&holes, sep), " | m | ");
    /* sub-slice view */
    slang_slice sub;
    slang_slice_sub(&sub, &parts, 1, 3, 8);
    slang_string *comma = S(",");
    EXPECT_STR(slang_strings_join(&sub, comma), "green,blue");
    slang_slice_release(&sub);
    /* round trip with Split */
    slang_string *j = slang_strings_join(&parts, comma);
    slang_slice back;
    slang_strings_split(j, comma, &back);
    assert(slice_is(&back, 3, v));
    slang_slice_release(&back);
    slang_release(j); slang_release(comma);
    slang_slice_release(&holes); slang_slice_release(&one); slang_slice_release(&parts);
    slang_release(sep);
}

TEST(trim) {
    slang_string *a = S("   padded   ");
    EXPECT_STR(slang_strings_trim(a), "padded");
    slang_string *b = S(" \t\n\v\f\r x y \r\n");
    EXPECT_STR(slang_strings_trim(b), "x y");
    slang_string *c = S("none");
    slang_string *r = slang_strings_trim(c);
    assert(STR_IS(r, "none"));
    slang_release(r);
    slang_string *d = S("   ");
    EXPECT_STR(slang_strings_trim(d), "");
    EXPECT_STR(slang_strings_trim(NULL), "");
    slang_string *u = S("\xC2\xA0\xE3\x80\x80héllo\xE2\x80\x83\xC2\x85");  /* NBSP, U+3000 ... U+2003, NEL */
    EXPECT_STR(slang_strings_trim(u), "héllo");
    slang_string *inner = S("a  b");
    EXPECT_STR(slang_strings_trim(inner), "a  b");
    slang_string *bad = slang_string_new(" \x80 ", 3);   /* invalid byte is not space */
    r = slang_strings_trim(bad);
    assert(slang_string_len(r) == 1 && (unsigned char)slang_string_data(r)[0] == 0x80);
    slang_release(r);
    slang_string *tail = S("x\xA0");                     /* lone continuation byte at end */
    EXPECT_STR(slang_strings_trim(tail), "x\xA0");
    slang_release(a); slang_release(b); slang_release(c); slang_release(d);
    slang_release(u); slang_release(inner); slang_release(bad); slang_release(tail);
}

TEST(predicates) {
    slang_string *msg = S("Hello, Slang!"), *sl = S("Slang"), *go = S("Go");
    slang_string *he = S("Hello"), *bang = S("!"), *e = S("");
    assert(slang_strings_contains(msg, sl));
    assert(!slang_strings_contains(msg, go));
    assert(slang_strings_contains(msg, e) && slang_strings_contains(NULL, NULL));
    assert(!slang_strings_contains(NULL, go));
    assert(slang_strings_has_prefix(msg, he) && !slang_strings_has_prefix(msg, sl));
    assert(slang_strings_has_prefix(msg, NULL) && !slang_strings_has_prefix(NULL, he));
    assert(slang_strings_has_suffix(msg, bang) && !slang_strings_has_suffix(msg, he));
    assert(slang_strings_has_suffix(NULL, e) && !slang_strings_has_suffix(bang, msg));
    assert(slang_strings_index(msg, sl) == 7);
    assert(slang_strings_index(msg, go) == -1);
    assert(slang_strings_index(msg, e) == 0 && slang_strings_index(NULL, NULL) == 0);
    assert(slang_strings_index(sl, msg) == -1);
    slang_string *h = S("héllo"), *l = S("l");
    assert(slang_strings_index(h, l) == 3);   /* byte offset */
    slang_release(h); slang_release(l);
    slang_release(msg); slang_release(sl); slang_release(go);
    slang_release(he); slang_release(bang); slang_release(e);
}

TEST(replace) {
    slang_string *s = S("a-b-c"), *dash = S("-"), *plus = S("+"), *e = S("");
    EXPECT_STR(slang_strings_replace(s, dash, plus), "a+b+c");
    EXPECT_STR(slang_strings_replace(s, dash, e), "abc");
    EXPECT_STR(slang_strings_replace(s, plus, dash), "a-b-c");     /* no match */
    EXPECT_STR(slang_strings_replace(s, dash, dash), "a-b-c");
    slang_string *aaa = S("aaaa"), *aa = S("aa"), *b = S("b"), *xyz = S("xyz");
    EXPECT_STR(slang_strings_replace(aaa, aa, b), "bb");
    EXPECT_STR(slang_strings_replace(aaa, aa, xyz), "xyzxyz");   /* grows */
    EXPECT_STR(slang_strings_replace(aaa, aaa, e), "");
    /* empty old: Go inserts at start and after each rune */
    slang_string *abc = S("héc");
    EXPECT_STR(slang_strings_replace(abc, e, dash), "-h-é-c-");
    EXPECT_STR(slang_strings_replace(NULL, NULL, dash), "-");
    EXPECT_STR(slang_strings_replace(NULL, dash, plus), "");
    slang_release(s); slang_release(dash); slang_release(plus); slang_release(e);
    slang_release(aaa); slang_release(aa); slang_release(b); slang_release(xyz); slang_release(abc);
}

TEST(case_mapping) {
    slang_string *msg = S("Hello, Slang! 123");
    EXPECT_STR(slang_strings_to_upper(msg), "HELLO, SLANG! 123");
    EXPECT_STR(slang_strings_to_lower(msg), "hello, slang! 123");
    EXPECT_STR(slang_strings_to_upper(NULL), "");
    EXPECT_STR(slang_strings_to_lower(NULL), "");
    slang_string *u = S("héllo ÿ straße ı ǆ мир αβγ ς ά ő 😀");
    EXPECT_STR(slang_strings_to_upper(u), "HÉLLO Ÿ STRAßE I ǆ МИР ΑΒΓ Σ Ά Ő 😀");
    slang_string *l = S("HÉLLO Ÿ İ ЁЖ ΑΒΓ Ά Ő × ÷");
    EXPECT_STR(slang_strings_to_lower(l), "héllo ÿ i ёж αβγ ά ő × ÷");
    slang_string *bad = slang_string_new("a\xff" "b", 3);
    slang_string *r = slang_strings_to_upper(bad);
    assert(slang_string_len(r) == 3 && memcmp(slang_string_data(r), "A\xff" "B", 3) == 0);
    slang_release(r);
    slang_release(msg); slang_release(u); slang_release(l); slang_release(bad);
}

static void repeat_negative(void) { slang_strings_repeat(NULL, -1); }
static void repeat_overflow(void) {
    slang_string *s = S("ab");
    slang_strings_repeat(s, INT64_MAX / 2);
}

TEST(repeat) {
    slang_string *ab = S("ab");
    EXPECT_STR(slang_strings_repeat(ab, 3), "ababab");
    EXPECT_STR(slang_strings_repeat(ab, 1), "ab");
    EXPECT_STR(slang_strings_repeat(ab, 0), "");
    EXPECT_STR(slang_strings_repeat(NULL, 1000000), "");
    slang_release(ab);
    char buf[4096];
    setenv("SLANG_NO_BACKTRACE", "1", 1);
    assert(run_child(repeat_negative, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: strings: negative Repeat count"));
    assert(run_child(repeat_overflow, buf, sizeof buf) == 2);
    assert(strstr(buf, "overflow"));
}

static int64_t parse_int(const char *lit, bool *ok) {
    slang_string *s = S(lit);
    slang_error *err = (slang_error *)1;
    int64_t v = slang_strings_to_int(s, &err);
    *ok = err == NULL;
    if (err) { assert(v == 0); slang_release(err); }
    slang_release(s);
    return v;
}

TEST(to_int) {
    bool ok;
    assert(parse_int("42", &ok) == 42 && ok);
    assert(parse_int("-17", &ok) == -17 && ok);
    assert(parse_int("0", &ok) == 0 && ok);
    assert(parse_int("+5", &ok) == 5 && ok);   /* as Go strconv.Atoi */
    assert(parse_int("9223372036854775807", &ok) == INT64_MAX && ok);
    assert(parse_int("-9223372036854775808", &ok) == INT64_MIN && ok);
    const char *bad[] = { "4x2", "", " 1", "1 ", "\r1", "\v1", "-", "+", "0x10", "1.5",
                          "9223372036854775808", "-9223372036854775809",
                          "99999999999999999999999", "--1", "abc" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        parse_int(bad[i], &ok);
        assert(!ok);
    }
    slang_error *err = NULL;
    assert(slang_strings_to_int(NULL, &err) == 0 && err != NULL);
    slang_string *m = slang_error_message(err);
    assert(strstr(slang_string_data(m), "invalid integer"));
    slang_release(m); slang_release(err);
    slang_string *big = S("99999999999999999999");
    assert(slang_strings_to_int(big, &err) == 0 && err != NULL);
    m = slang_error_message(err);
    assert(strstr(slang_string_data(m), "out of range"));
    slang_release(m); slang_release(err); slang_release(big);
    /* embedded NUL */
    slang_string *nul = slang_string_new("1\0" "2", 3);
    assert(slang_strings_to_int(nul, &err) == 0 && err != NULL);
    slang_release(err); slang_release(nul);
    assert(slang_strings_to_int(NULL, NULL) == 0);   /* err may be NULL */
}

TEST(to_float) {
    slang_error *err = (slang_error *)1;
    slang_string *s = S("3.14");
    assert(slang_strings_to_float(s, &err) == 3.14 && err == NULL);
    slang_release(s);
    s = S("-2e3");
    assert(slang_strings_to_float(s, &err) == -2000.0 && err == NULL);
    slang_release(s);
    s = S("7");
    assert(slang_strings_to_float(s, &err) == 7.0 && err == NULL);
    slang_release(s);
    const char *bad[] = { "", "x", "1.5x", " 1", "1 ", "\f1", "1e400", "-1e400", "." };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        s = S(bad[i]);
        err = NULL;
        assert(slang_strings_to_float(s, &err) == 0 && err != NULL);
        slang_release(err); slang_release(s);
    }
}

TEST(math) {
    assert(slang_math_sqrt(4.0) == 2.0);
    assert(fabs(slang_math_sqrt(2.0) - 1.4142135623730951) < 1e-15);
    assert(isnan(slang_math_sqrt(-1.0)));
    assert(slang_math_abs(-2.5) == 2.5 && slang_math_abs(3.0) == 3.0);
    assert(!signbit(slang_math_abs(-0.0)));
    assert(slang_math_pow(2.0, 10.0) == 1024.0 && slang_math_pow(5.0, 0.0) == 1.0);
    assert(slang_math_pow(2.0, -1.0) == 0.5);
    assert(slang_math_floor(3.7) == 3.0 && slang_math_floor(-3.2) == -4.0);
    assert(slang_math_ceil(3.2) == 4.0 && slang_math_ceil(-3.7) == -3.0);
    assert(slang_math_min(1.0, 2.0) == 1.0 && slang_math_min(2.0, -1.0) == -1.0);
    assert(slang_math_max(1.0, 2.0) == 2.0 && slang_math_max(2.0, -1.0) == 2.0);
    assert(isnan(slang_math_min(NAN, 1.0)) && isnan(slang_math_max(1.0, NAN)));
    assert(slang_math_min(NAN, -INFINITY) == -INFINITY);
    assert(slang_math_max(NAN, INFINITY) == INFINITY);
    assert(signbit(slang_math_min(0.0, -0.0)) && signbit(slang_math_min(-0.0, 0.0)));
    assert(!signbit(slang_math_max(0.0, -0.0)) && !signbit(slang_math_max(-0.0, 0.0)));
    assert(slang_math_sin(0.0) == 0.0 && fabs(slang_math_sin(M_PI / 2) - 1.0) < 1e-15);
    assert(slang_math_cos(0.0) == 1.0 && fabs(slang_math_cos(M_PI) + 1.0) < 1e-15);
    assert(slang_math_tan(0.0) == 0.0 && fabs(slang_math_tan(M_PI / 4) - 1.0) < 1e-15);
    assert(slang_math_log(1.0) == 0.0 && fabs(slang_math_log(M_E) - 1.0) < 1e-15);
    assert(slang_math_log(0.0) == -INFINITY && isnan(slang_math_log(-1.0)));
    assert(slang_math_exp(0.0) == 1.0 && fabs(slang_math_exp(1.0) - M_E) < 1e-15);
}

static slang_string *fmt(const char *tmpl, int n, const char *const *v) {
    slang_slice args;
    strs(&args, n, v);
    slang_string *t = S(tmpl);
    slang_string *r = slang_fmt_format(t, &args);
    slang_release(t);
    slang_slice_release(&args);
    return r;
}

TEST(format) {
    static const char *const a2[] = { "Alice", "31" };
    EXPECT_STR(fmt("{0} is {1} years old", 2, a2), "Alice is 31 years old");
    static const char *const xy[] = { "x", "y" };
    EXPECT_STR(fmt("{1} before {0}, {0} again", 2, xy), "y before x, x again");
    EXPECT_STR(fmt("no placeholders", 2, xy), "no placeholders");          /* extra ignored */
    EXPECT_STR(fmt("{0} {2} {1}", 2, xy), "x {2} y");                    /* missing kept */
    EXPECT_STR(fmt("{0}", 0, NULL), "{0}");
    EXPECT_STR(fmt("", 2, xy), "");
    EXPECT_STR(fmt("{} {x} {-1} { 0} {0 } {0", 2, xy), "{} {x} {-1} { 0} {0 } {0");
    EXPECT_STR(fmt("{{0}} {01} {99999999999999999999999}", 2, xy), "{x} y {99999999999999999999999}");
    EXPECT_STR(fmt("${0}.0{1}", 2, xy), "$x.0y");                         /* $ not special */
    EXPECT_STR(fmt("{0}{0}{0}", 1, xy), "xxx");
    static const char *const empt[] = { "" };
    EXPECT_STR(fmt("[{0}]", 1, empt), "[]");
    /* NULL template, NULL args pointer, NULL elements */
    EXPECT_STR(slang_fmt_format(NULL, NULL), "");
    slang_string *t = S("a{0}b");
    EXPECT_STR(slang_fmt_format(t, NULL), "a{0}b");
    slang_slice holes;
    slang_slice_new_ops(&holes, 8, 1, 1, &slang_ops_ref);
    EXPECT_STR(slang_fmt_format(t, &holes), "ab");
    slang_slice_release(&holes);
    slang_release(t);
}

TEST(io) {
    char dir[] = "/tmp/slang_std_test_XXXXXX";
    assert(mkdtemp(dir));
    char path[256];
    snprintf(path, sizeof path, "%s/f.txt", dir);
    slang_string *p = S(path), *data = S("hello\nworld\n");
    slang_error *err = (slang_error *)1;
    slang_io_write_file(p, data, &err);
    assert(err == NULL);
    err = (slang_error *)1;
    slang_string *back = slang_io_read_file(p, &err);
    assert(err == NULL && slang_string_eq(back, data));
    slang_release(back);
    slang_io_write_file(p, NULL, &err);            /* empty contents */
    assert(err == NULL);
    back = slang_io_read_file(p, &err);
    assert(err == NULL && slang_string_len(back) == 0);
    slang_release(back);
    slang_io_write_file(p, data, NULL);            /* err may be NULL */
    /* failures */
    char missing[300];
    snprintf(missing, sizeof missing, "%s/nope/x", dir);
    slang_string *mp = S(missing);
    assert(slang_io_read_file(mp, &err) == NULL && err != NULL);
    slang_string *m = slang_error_message(err);
    assert(strstr(slang_string_data(m), "open ") && strstr(slang_string_data(m), "No such file"));
    slang_release(m); slang_release(err);
    err = NULL;
    slang_io_write_file(mp, data, &err);
    assert(err != NULL);
    slang_release(err);
    slang_io_write_file(mp, data, NULL);           /* error dropped without leaking */
    unlink(path);
    rmdir(dir);
    slang_release(p); slang_release(data); slang_release(mp);
}

static void read_lines(void) {
    slang_error *err = (slang_error *)1;
    slang_string *a = slang_io_read_line(&err);
    assert(err == NULL && STR_IS(a, "first"));
    slang_string *b = slang_io_read_line(&err);
    assert(err == NULL && STR_IS(b, ""));
    slang_string *c = slang_io_read_line(&err);
    assert(err == NULL && STR_IS(c, "last"));
    slang_string *d = slang_io_read_line(&err);
    assert(d == NULL && err == slang_err_eof);
    slang_release(err);
    slang_release(a); slang_release(b); slang_release(c);
    printf("lines ok");
    fflush(stdout);
}

TEST(read_line) {
    int fds[2];
    assert(pipe(fds) == 0);
    const char *in = "first\r\n\nlast";
    assert(write(fds[1], in, strlen(in)) == (ssize_t)strlen(in));
    close(fds[1]);
    int saved = dup(0);
    dup2(fds[0], 0);
    close(fds[0]);
    char buf[256];
    int st = run_child_fd(read_lines, 1, buf, sizeof buf);
    dup2(saved, 0);
    close(saved);
    assert(st == 0 && strcmp(buf, "lines ok") == 0);
}

TEST(errors) {
    SLANG_STATIC_STRING(nf_msg, "not found");
    slang_error *sentinel = slang_error_sentinel(nf_msg);
    slang_error *other = slang_error_new_cstr("empty");
    slang_string *ctx = S("user 3");
    slang_error *w = slang_errors_wrap(sentinel, ctx);
    slang_string *m = slang_error_message(w);
    assert(STR_IS(m, "user 3: not found"));
    slang_release(m);
    assert(slang_errors_is(w, sentinel) && !slang_errors_is(w, other));
    assert(slang_errors_is(sentinel, sentinel) && !slang_errors_is(sentinel, w));
    assert(slang_errors_is(NULL, NULL) && !slang_errors_is(w, NULL) && !slang_errors_is(NULL, sentinel));
    slang_string *req = S("request");
    slang_error *w2 = slang_errors_wrap(w, req);
    m = slang_error_message(w2);
    assert(STR_IS(m, "request: user 3: not found"));
    slang_release(m);
    assert(slang_errors_is(w2, sentinel) && slang_errors_is(w2, w));
    slang_error *u1 = slang_errors_unwrap(w2);
    assert(u1 == w);
    slang_error *u2 = slang_errors_unwrap(u1);
    assert(u2 == sentinel);
    assert(slang_errors_unwrap(u2) == NULL);        /* not a wrapper */
    assert(slang_errors_unwrap(other) == NULL);
    assert(slang_errors_unwrap(NULL) == NULL);
    assert(slang_errors_wrap(NULL, ctx) == NULL);   /* wrapping nil is nil */
    slang_error *we = slang_errors_wrap(other, NULL);  /* empty context */
    m = slang_error_message(we);
    assert(STR_IS(m, ": empty"));
    slang_release(m);
    /* the wrapper keeps its cause alive */
    slang_release(other);
    slang_error *c = slang_errors_unwrap(we);
    m = slang_error_message(c);
    assert(STR_IS(m, "empty"));
    slang_release(m); slang_release(c); slang_release(we);
    slang_release(u1); slang_release(u2);
    slang_release(w2); slang_release(w);
    slang_release(ctx); slang_release(req);
}

TEST(delay) {
    slang_task *t = slang_task_delay(1);
    slang_task_await(t);
    assert(slang_task_is_done(t));
    slang_release(t);
    t = slang_task_delay(0);
    assert(slang_task_is_done(t));
    slang_release(t);
}

int main(void) {
    printf("test_std\n");
    int64_t live = slang_live_objects();
    RUN(split);
    RUN(join);
    RUN(trim);
    RUN(predicates);
    RUN(replace);
    RUN(case_mapping);
    RUN(repeat);
    RUN(to_int);
    RUN(to_float);
    RUN(math);
    RUN(format);
    RUN(io);
    RUN(read_line);
    assert(slang_live_objects() == live);
    RUN(errors);
    assert(slang_live_objects() == live);  /* immortal sentinels do not count */
    RUN(delay);
    slang_runtime_shutdown();
    assert(slang_live_objects() == live);
    return 0;
}
