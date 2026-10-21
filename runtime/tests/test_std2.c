/* Tests for the second batch of std.* runtime functions: time, os, fs, io
 * additions, strings additions, math additions, random, crypto, encoding,
 * uuid. Hash/HMAC/base64 vectors are from FIPS 180-4 examples, RFC 1321,
 * RFC 4231 and RFC 4648. */
#include "test.h"
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

#define EXPECT_STR(call, lit) do { slang_string *r_ = (call); assert(r_ != NULL); \
        if (!STR_IS(r_, lit)) { fprintf(stderr, "got '%s' want '%s' (%s:%d)\n",  \
            slang_string_data(r_), lit, __FILE__, __LINE__); assert(0); }         \
        slang_release(r_); } while (0)

/* call taking one string literal argument */
#define STR1(fn, a, lit) do { slang_string *a_ = S(a); EXPECT_STR(fn(a_), lit); slang_release(a_); } while (0)
#define STR2(fn, a, b, lit) do { slang_string *a_ = S(a), *b_ = S(b); EXPECT_STR(fn(a_, b_), lit); \
        slang_release(a_); slang_release(b_); } while (0)

static bool slice_is(const slang_slice *s, int n, const char *const *v) {
    if (s->len != n) return false;
    for (int i = 0; i < n; i++)
        if (!STR_IS(((slang_string **)s->data)[i], v[i])) return false;
    return true;
}

static void strs(slang_slice *out, int n, const char *const *v) {
    slang_slice_new_ops(out, 8, n, n, &slang_ops_ref);
    for (int i = 0; i < n; i++) ((slang_string **)out->data)[i] = S(v[i]);
}

static bool bytes_hex_is(const slang_slice *b, const char *hex) {
    slang_string *h = slang_encoding_hex_encode(b);
    bool ok = STR_IS(h, hex);
    if (!ok) fprintf(stderr, "bytes %s want %s\n", slang_string_data(h), hex);
    slang_release(h);
    return ok;
}

/* ---------------------------------------------------------------- time */

TEST(time_calendar) {
    assert(slang_time_year(0) == 1970 && slang_time_month(0) == 1 && slang_time_day(0) == 1);
    assert(slang_time_weekday(0) == 4);   /* Thursday */
    int64_t t = 1700000000123;            /* 2023-11-14 22:13:20.123 UTC, Tuesday */
    assert(slang_time_year(t) == 2023 && slang_time_month(t) == 11 && slang_time_day(t) == 14);
    assert(slang_time_hour(t) == 22 && slang_time_minute(t) == 13 && slang_time_second(t) == 20);
    assert(slang_time_weekday(t) == 2);
    /* negative times floor to the previous second */
    assert(slang_time_year(-1) == 1969 && slang_time_second(-1) == 59 && slang_time_hour(-1) == 23);
    /* leap day */
    int64_t leap = 951782400000;          /* 2000-02-29 00:00:00 UTC */
    assert(slang_time_month(leap) == 2 && slang_time_day(leap) == 29);
}

TEST(time_format_parse) {
    slang_string *l = S("%Y-%m-%d %H:%M:%S.%L");
    EXPECT_STR(slang_time_format(1700000000123, l), "2023-11-14 22:13:20.123");
    EXPECT_STR(slang_time_format(-1, l), "1969-12-31 23:59:59.999");
    EXPECT_STR(slang_time_format(0, l), "1970-01-01 00:00:00.000");
    slang_release(l);
    l = S("%A %d %B %Y, %%, %j");
    EXPECT_STR(slang_time_format(0, l), "Thursday 01 January 1970, %, 001");
    slang_release(l);
    EXPECT_STR(slang_time_format(0, NULL), "");
    l = S("%p");   /* AM/PM in C locale */
    EXPECT_STR(slang_time_format(0, l), "AM");
    slang_release(l);
    /* long output grows the buffer */
    slang_string *y = S("%Y-"), *ly = slang_strings_repeat(y, 300);
    slang_string *r = slang_time_format(0, ly);
    assert(slang_string_len(r) == 300 * 5);
    slang_release(r); slang_release(ly); slang_release(y);

    slang_error *e = NULL;
    slang_string *s = S("2023-11-14 22:13:20"), *lay = S("%Y-%m-%d %H:%M:%S");
    assert(slang_time_parse(s, lay, &e) == 1700000000000 && e == NULL);
    slang_release(s);
    s = S("2023-11-14 22:13:20x");   /* trailing garbage */
    assert(slang_time_parse(s, lay, &e) == 0 && e != NULL);
    slang_string *m = slang_error_message(e);
    assert(strstr(slang_string_data(m), "cannot parse") != NULL);
    slang_release(m); slang_release(e); e = NULL;
    slang_release(s);
    s = S("nope");
    assert(slang_time_parse(s, lay, &e) == 0 && e != NULL);
    slang_release(e); e = NULL;
    slang_release(s); slang_release(lay);
    /* timezone offset via %z */
    s = S("2023-11-14T23:13:20+0100"); lay = S("%Y-%m-%dT%H:%M:%S%z");
    assert(slang_time_parse(s, lay, &e) == 1700000000000 && e == NULL);
    slang_release(s); slang_release(lay);
    /* date only, before the epoch */
    s = S("1969-12-31"); lay = S("%Y-%m-%d");
    assert(slang_time_parse(s, lay, &e) == -86400000 && e == NULL);
    slang_release(s); slang_release(lay);
    /* round trip */
    lay = S("%Y-%m-%d %H:%M:%S");
    s = slang_time_format(1234567890000, lay);
    assert(slang_time_parse(s, lay, NULL) == 1234567890000);
    slang_release(s); slang_release(lay);
}

TEST(time_clock) {
    int64_t a = slang_time_now(), an = slang_time_now_nanos();
    assert(a > 1600000000000 && an / 1000000 >= a - 1000);
    slang_time_sleep(20);
    assert(slang_time_since(a) >= 20);
    slang_time_sleep(0);
    slang_time_sleep(-5);
    assert(slang_time_now_nanos() > an);
}

/* ---------------------------------------------------------------- os */

TEST(os) {
    slang_string *n = S("SLANG_TEST_STD2_VAR"), *v = S("héllo=1");
    EXPECT_STR(slang_os_getenv(n), "");
    slang_error *e = NULL;
    slang_os_setenv(n, v, &e);
    assert(e == NULL);
    EXPECT_STR(slang_os_getenv(n), "héllo=1");
    slang_os_setenv(n, NULL, &e);
    assert(e == NULL);
    EXPECT_STR(slang_os_getenv(n), "");
    slang_release(n);
    n = S("BAD=NAME");
    slang_os_setenv(n, v, &e);
    assert(e != NULL);
    slang_release(e); e = NULL;
    slang_os_setenv(NULL, v, &e);
    assert(e != NULL);
    slang_release(e); e = NULL;
    slang_os_setenv(NULL, v, NULL);   /* discarded error does not leak */
    slang_release(n); slang_release(v);

    assert(slang_os_pid() == (int64_t)getpid());
    slang_string *h = slang_os_hostname(&e);
    assert(e == NULL && slang_string_len(h) > 0);
    slang_release(h);

    slang_string *cwd = slang_os_cwd(&e);
#if !defined(_WIN32)
    assert(e == NULL && slang_string_data(cwd)[0] == '/');
    slang_string *tmp = S("/");
    slang_os_chdir(tmp, &e);
    assert(e == NULL);
    EXPECT_STR(slang_os_cwd(NULL), "/");
#else
    assert(e == NULL && slang_string_data(cwd)[1] == ':' && slang_string_data(cwd)[2] == '\\');  /* C:\... */
    slang_string *tmp = S("C:\\");
    slang_os_chdir(tmp, &e);
    assert(e == NULL);
    EXPECT_STR(slang_os_cwd(NULL), "C:\\");
#endif
    slang_os_chdir(cwd, &e);
    assert(e == NULL);
    slang_release(tmp);
    tmp = S("/definitely/not/here");
    slang_os_chdir(tmp, &e);
    assert(e != NULL);
    slang_string *m = slang_error_message(e);
    assert(strstr(slang_string_data(m), "chdir /definitely/not/here: ") != NULL);
    slang_release(m); slang_release(e); slang_release(tmp);
    slang_string *now = slang_os_cwd(NULL);
    assert(slang_string_eq(now, cwd));
    slang_release(now); slang_release(cwd);

    /* Args aliases the runtime's argv slice */
    slang_slice args;
    slang_os_args(&args);
    assert(args.len == 1);
    slang_slice_release(&args);
}

static void exit_child(void) {
    slang_print_cstr("bye");
    slang_os_exit(7);
}

TEST(os_exit) {
    char buf[64];
    assert(run_child_fd(exit_child, 1, buf, sizeof buf) == 7);
    assert(strcmp(buf, "bye") == 0);   /* stdout flushed before exiting */
}

/* ---------------------------------------------------------------- fs */

static slang_string *join(const char *a, const char *b) {
    slang_string *x = S(a), *y = S(b), *r = slang_fs_join(x, y);
    slang_release(x); slang_release(y);
    return r;
}

TEST(fs_paths) {
    EXPECT_STR(join("a", "b"), "a/b");
    EXPECT_STR(join("a/", "/b"), "a/b");
    EXPECT_STR(join("a/b", "../c"), "a/c");
    EXPECT_STR(join("/", "x"), "/x");
    EXPECT_STR(join("", "b"), "b");
    EXPECT_STR(join("a", ""), "a");
    EXPECT_STR(join("", ""), "");
    EXPECT_STR(join("a", "../.."), "..");
    EXPECT_STR(join("/a", "../../.."), "/");
    EXPECT_STR(join("./a/./b/", "."), "a/b");
    EXPECT_STR(slang_fs_join(NULL, NULL), "");

    STR1(slang_fs_base, "/a/b.txt", "b.txt");
    STR1(slang_fs_base, "a/b/", "b");
    STR1(slang_fs_base, "///", "/");
    STR1(slang_fs_base, "", ".");
    STR1(slang_fs_base, "x", "x");

    STR1(slang_fs_dir, "/a/b.txt", "/a");
    STR1(slang_fs_dir, "a/b/", "a/b");
    STR1(slang_fs_dir, "b", ".");
    STR1(slang_fs_dir, "/", "/");
    STR1(slang_fs_dir, "", ".");
    STR1(slang_fs_dir, "/x", "/");
    STR1(slang_fs_dir, "a//b", "a");

    STR1(slang_fs_ext, "a/b.tar.gz", ".gz");
    STR1(slang_fs_ext, "a.d/b", "");
    STR1(slang_fs_ext, ".bashrc", ".bashrc");
    STR1(slang_fs_ext, "noext", "");
    STR1(slang_fs_ext, "dot.", ".");

    slang_string *p = S("/x/../y/./z");
    EXPECT_STR(slang_fs_abs(p, NULL), "/y/z");
    slang_release(p);
    p = S("rel/f");
    slang_string *a = slang_fs_abs(p, NULL), *cwd = slang_os_cwd(NULL);
    assert(slang_string_has_prefix(a, cwd) && slang_string_has_suffix(a, p));
    slang_release(a); slang_release(cwd); slang_release(p);

    slang_string *td = slang_fs_temp_dir();
    assert(slang_string_len(td) > 0);
    slang_release(td);
}

static slang_string *in_dir(const slang_string *dir, const char *name) {
    slang_string *n = S(name), *r = slang_fs_join(dir, n);
    slang_release(n);
    return r;
}

static void must_ok(slang_error *e) {
    if (e) {
        slang_string *m = slang_error_message(e);
        fprintf(stderr, "unexpected error: %s\n", slang_string_data(m));
        assert(0);
    }
}

TEST(fs_files) {
    char tmpl[] = "/tmp/slang_std2_XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    slang_string *root = S(tmpl);
    slang_error *e = NULL;
    assert(slang_fs_exists(root) && slang_fs_is_dir(root));

    slang_string *deep = in_dir(root, "a/b/c");
    slang_fs_mkdir_all(deep, &e); must_ok(e);
    slang_fs_mkdir_all(deep, &e); must_ok(e);        /* already exists: ok */
    assert(slang_fs_is_dir(deep));
    slang_fs_mkdir(deep, &e);                        /* plain Mkdir: EEXIST */
    assert(e != NULL); slang_release(e); e = NULL;

    slang_string *f = in_dir(root, "a/file.txt"), *d1 = S("hello "), *d2 = S("world\n");
    assert(!slang_fs_exists(f));
    slang_fs_append_file(f, d1, &e); must_ok(e);
    slang_fs_append_file(f, d2, &e); must_ok(e);
    slang_fs_append_file(f, NULL, &e); must_ok(e);
    assert(slang_fs_exists(f) && !slang_fs_is_dir(f));
    assert(slang_fs_size(f, &e) == 12); must_ok(e);
    int64_t mt = slang_fs_mod_time(f, &e); must_ok(e);
    assert(llabs(mt - slang_time_now()) < 60000);

    /* MkdirAll through a file fails */
    slang_string *thru = in_dir(root, "a/file.txt/x");
    slang_fs_mkdir_all(thru, &e);
    assert(e != NULL); slang_release(e); e = NULL;
    slang_release(thru);

    slang_string *g = in_dir(root, "a/copy.txt");
    slang_fs_copy(f, g, &e); must_ok(e);
    slang_string *c = slang_read_file(g, &e); must_ok(e);
    assert(STR_IS(c, "hello world\n"));
    slang_release(c);
    slang_fs_copy(deep, g, &e);                      /* copying a directory */
    assert(e != NULL); slang_release(e); e = NULL;

    slang_string *dir_a = in_dir(root, "a");
    slang_slice names;
    slang_fs_read_dir(dir_a, &names, &e); must_ok(e);
    static const char *const want[] = { "b", "copy.txt", "file.txt" };
    assert(slice_is(&names, 3, want));
    slang_slice_release(&names);

    slang_string *h = in_dir(root, "a/renamed.txt");
    slang_fs_rename(g, h, &e); must_ok(e);
    assert(!slang_fs_exists(g) && slang_fs_exists(h));

    slang_fs_remove(dir_a, &e);                      /* not empty */
    assert(e != NULL); slang_release(e); e = NULL;
    slang_fs_remove(h, &e); must_ok(e);
    slang_fs_remove(h, &e);                          /* already gone */
    assert(e != NULL);
    slang_string *m = slang_error_message(e);
    assert(strstr(slang_string_data(m), "remove ") && strstr(slang_string_data(m), "renamed.txt: "));
    slang_release(m); slang_release(e); e = NULL;

    /* errors on missing paths */
    slang_string *missing = in_dir(root, "nope");
    assert(!slang_fs_exists(missing) && !slang_fs_is_dir(missing));
    assert(slang_fs_size(missing, &e) == 0 && e); slang_release(e); e = NULL;
    assert(slang_fs_mod_time(missing, &e) == 0 && e); slang_release(e); e = NULL;
    slang_fs_read_dir(missing, &names, &e);
    assert(e && names.len == 0); slang_release(e); e = NULL;
    slang_fs_rename(missing, h, &e); assert(e); slang_release(e); e = NULL;
    slang_fs_copy(missing, h, &e); assert(e); slang_release(e); e = NULL;
    slang_fs_remove_all(missing, &e); must_ok(e);    /* Go: missing is fine */
    slang_fs_remove_all(NULL, &e); must_ok(e);
    slang_string *nul = slang_string_new("/tmp\0x", 6);   /* embedded NUL: never a file */
    assert(!slang_fs_exists(NULL) && !slang_fs_exists(nul) && !slang_fs_is_dir(nul));
    slang_fs_mkdir(nul, &e); assert(e); slang_release(e); e = NULL;
    slang_release(nul);

    /* empty dir lists as [] */
    slang_fs_read_dir(deep, &names, &e); must_ok(e);
    assert(names.len == 0);
    slang_slice_release(&names);

    /* a symlink inside is removed, its target is not */
    slang_string *outside = S("/tmp"), *link = in_dir(root, "a/b/lnk");
    assert(symlink("/tmp", slang_string_data(link)) == 0);
    slang_fs_remove_all(root, &e); must_ok(e);
    assert(!slang_fs_exists(root) && slang_fs_is_dir(outside));

    slang_release(outside); slang_release(link);
    slang_release(missing); slang_release(h); slang_release(dir_a); slang_release(g);
    slang_release(f); slang_release(d1); slang_release(d2); slang_release(deep);
    slang_release(root);
}

/* ---------------------------------------------------------------- io */

TEST(io_lines) {
    char path[] = "/tmp/slang_std2_lines_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    slang_string *p = S(path);
    slang_error *e = NULL;
    static const char *const v[] = { "one", "", "three é" };
    slang_slice lines, got;
    strs(&lines, 3, v);
    slang_io_write_lines(p, &lines, &e); must_ok(e);
    EXPECT_STR(slang_read_file(p, NULL), "one\n\nthree é\n");
    slang_io_read_lines(p, &got, &e); must_ok(e);
    assert(slice_is(&got, 3, v));
    slang_slice_release(&got);
    slang_slice_release(&lines);

    /* CRLF and no final newline */
    slang_string *raw = S("a\r\nb\nc");
    slang_io_write_file(p, raw, &e); must_ok(e);
    slang_io_read_lines(p, &got, &e); must_ok(e);
    static const char *const v2[] = { "a", "b", "c" };
    assert(slice_is(&got, 3, v2));
    slang_slice_release(&got);
    slang_release(raw);

    /* empty file and nil slice */
    slang_io_write_lines(p, NULL, &e); must_ok(e);
    slang_io_read_lines(p, &got, &e); must_ok(e);
    assert(got.len == 0);
    slang_slice_release(&got);
    raw = S("\n");
    slang_io_write_file(p, raw, &e); must_ok(e);
    slang_io_read_lines(p, &got, &e); must_ok(e);
    static const char *const v3[] = { "" };
    assert(slice_is(&got, 1, v3));
    slang_slice_release(&got);
    slang_release(raw);

    unlink(path);
    slang_io_read_lines(p, &got, &e);
    assert(e != NULL && got.len == 0);
    slang_release(e); e = NULL;
    slang_io_read_lines(p, &got, NULL);
    slang_string *bad = S("/nonexistent-dir/x");
    slang_io_write_lines(bad, NULL, &e);
    assert(e != NULL); slang_release(e);
    slang_release(bad);
    slang_release(p);
}

static const char *g_stdin_path;
static void read_all_child(void) {
    /* g_stdin_path survives fork; a re-executed child (Windows) gets the env */
    const char *in = g_stdin_path ? g_stdin_path : getenv("SLANG_STD2_STDIN");
    assert(in && freopen(in, "r", stdin) != NULL);
    slang_error *e = NULL;
    slang_string *s = slang_io_read_all(&e);
    assert(e == NULL);
    printf("[%s]%lld", slang_string_data(s), (long long)slang_string_len(s));
    slang_release(s);
    s = slang_io_read_all(&e);   /* at EOF: "" and no error */
    assert(e == NULL && slang_string_len(s) == 0);
    slang_release(s);
    slang_io_eprintln(NULL);
    fflush(stdout);
}

TEST(io_read_all) {
    char path[] = "/tmp/slang_std2_stdin_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, "x\ny\n", 4) == 4);
    close(fd);
    g_stdin_path = path;
    setenv("SLANG_STD2_STDIN", path, 1);
    char buf[64];
    assert(run_child_fd(read_all_child, 1, buf, sizeof buf) == 0);
    assert(strcmp(buf, "[x\ny\n]4") == 0);
    unlink(path);
}

static void eprint_child(void) {
    slang_string *a = S("err "), *b = S("line");
    slang_io_eprint(a);
    slang_io_eprintln(b);
    slang_release(a); slang_release(b);
}

TEST(io_eprint) {
    char buf[64];
    assert(run_child(eprint_child, buf, sizeof buf) == 0);
    assert(strcmp(buf, "err line\n") == 0);
}

/* ---------------------------------------------------------------- strings */

#define SL(call, ...) do {                                                       \
        static const char *const exp_[] = { __VA_ARGS__ };                       \
        slang_slice r_; call;                                                    \
        assert(slice_is(&r_, (int)(sizeof exp_ / sizeof *exp_), exp_));          \
        slang_slice_release(&r_);                                                \
    } while (0)

TEST(strings_more) {
    slang_string *s = S("  foo bar\t\n baz  "), *x, *y;
    SL(slang_strings_fields(s, &r_), "foo", "bar", "baz");
    slang_release(s);
    slang_slice r;
    slang_strings_fields(NULL, &r); assert(r.len == 0); slang_slice_release(&r);
    s = S(" \t "); slang_strings_fields(s, &r); assert(r.len == 0); slang_slice_release(&r); slang_release(s);
    s = S("a\xc2\xa0" "b\xe3\x80\x80" "c");   /* NBSP and ideographic space */
    SL(slang_strings_fields(s, &r_), "a", "b", "c");
    slang_release(s);

    x = S("cheese"); y = S("e");
    assert(slang_strings_count(x, y) == 3);
    slang_release(y); y = S("");
    assert(slang_strings_count(x, y) == 7);   /* runes + 1 */
    slang_release(y); y = S("ee");
    assert(slang_strings_count(x, y) == 1);
    assert(slang_strings_last_index(x, y) == 2);
    slang_release(y); y = S("e");
    assert(slang_strings_last_index(x, y) == 5);
    slang_release(y); y = S("");
    assert(slang_strings_last_index(x, y) == 6);
    slang_release(y); y = S("z");
    assert(slang_strings_last_index(x, y) == -1);
    assert(slang_strings_count(NULL, NULL) == 1);
    slang_release(x); slang_release(y);

    STR2(slang_strings_trim_prefix, "prefix-body", "prefix-", "body");
    STR2(slang_strings_trim_prefix, "body", "x", "body");
    STR2(slang_strings_trim_suffix, "file.go", ".go", "file");
    STR2(slang_strings_trim_suffix, "file.go", "", "file.go");
    STR2(slang_strings_trim_left, "xxyhixy", "xy", "hixy");
    STR2(slang_strings_trim_right, "xxyhixy", "xy", "xxyhi");
    STR2(slang_strings_trim_left, "ééaé", "é", "aé");
    STR2(slang_strings_trim_right, "aéé", "é", "a");
    STR2(slang_strings_trim_right, "abc", "", "abc");
    STR2(slang_strings_trim_left, "aaa", "a", "");
    /* trimming an invalid byte only with that byte in the cutset */
    x = slang_string_new("\xff" "a\xff", 3); y = slang_string_new("\xff", 1);
    slang_string *t = slang_strings_trim_left(x, y);
    assert(slang_string_len(t) == 2); slang_release(t);
    t = slang_strings_trim_right(x, y);
    assert(slang_string_len(t) == 2); slang_release(t);
    slang_release(y); y = S("\xef\xbf\xbd");   /* U+FFFD does not match invalid bytes */
    t = slang_strings_trim_left(x, y);
    assert(slang_string_len(t) == 3); slang_release(t);
    slang_release(x); slang_release(y);

    x = S("a,b,c,d"); y = S(",");
    SL(slang_strings_split_n(x, y, 2, &r_), "a", "b,c,d");
    SL(slang_strings_split_n(x, y, 1, &r_), "a,b,c,d");
    SL(slang_strings_split_n(x, y, -1, &r_), "a", "b", "c", "d");
    SL(slang_strings_split_n(x, y, 10, &r_), "a", "b", "c", "d");
    slang_strings_split_n(x, y, 0, &r); assert(r.len == 0); slang_slice_release(&r);
    slang_release(y); y = S("");
    SL(slang_strings_split_n(x, y, 3, &r_), "a", ",", "b,c,d");
    slang_release(x); slang_release(y);
    x = S(""); y = S(",");
    SL(slang_strings_split_n(x, y, 2, &r_), "");
    slang_release(x); slang_release(y);

    x = S("Go"); y = S("GO");
    assert(slang_strings_equal_fold(x, y));
    slang_release(y); y = S("Gö");
    assert(!slang_strings_equal_fold(x, y));
    slang_release(x); slang_release(y);
    x = S("ΣΊΣΥΦΟΣ"); y = S("σίσυφος");   /* final sigma folds */
    assert(slang_strings_equal_fold(x, y));
    slang_release(x); slang_release(y);
    x = S("ПРИВЕТ"); y = S("привет");
    assert(slang_strings_equal_fold(x, y));
    assert(slang_strings_compare(x, y) == -1 && slang_strings_compare(y, x) == 1);
    assert(slang_strings_compare(x, x) == 0 && slang_strings_compare(NULL, NULL) == 0);
    slang_release(x); slang_release(y);

    STR1(slang_strings_reverse, "héllo", "olléh");
    STR1(slang_strings_reverse, "", "");
    STR1(slang_strings_reverse, "日本語", "語本日");
    x = slang_string_new("a\xff" "b", 3);
    t = slang_strings_reverse(x);
    assert(memcmp(slang_string_data(t), "b\xff" "a", 3) == 0);
    slang_release(t); slang_release(x);

    x = S("42"); y = S("0");
    EXPECT_STR(slang_strings_pad_left(x, 5, y), "00042");
    EXPECT_STR(slang_strings_pad_right(x, 5, y), "42000");
    EXPECT_STR(slang_strings_pad_left(x, 1, y), "42");
    EXPECT_STR(slang_strings_pad_left(x, -3, y), "42");
    EXPECT_STR(slang_strings_pad_left(x, 5, NULL), "42");
    slang_release(y); y = S("ab");
    EXPECT_STR(slang_strings_pad_left(x, 7, y), "ababa42");
    slang_release(x); x = S("é");
    slang_release(y); y = S("·");
    EXPECT_STR(slang_strings_pad_right(x, 3, y), "é··");   /* width counts runes */
    slang_release(x); slang_release(y);

    STR1(slang_strings_title, "hello wOrld  of slang", "Hello WOrld  Of Slang");
    STR1(slang_strings_title, "élan über", "Élan Über");
    STR1(slang_strings_title, "", "");

    /* runes / bytes */
    x = S("añ€😀");
    slang_strings_runes(x, &r);
    assert(r.len == 4);
    int32_t *rs = r.data;
    assert(rs[0] == 'a' && rs[1] == 0xF1 && rs[2] == 0x20AC && rs[3] == 0x1F600);
    EXPECT_STR(slang_strings_from_runes(&r), "añ€😀");
    rs[0] = -1; rs[1] = 0xD800; rs[2] = 0x110000;
    EXPECT_STR(slang_strings_from_runes(&r), "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd😀");
    slang_slice_release(&r);
    EXPECT_STR(slang_strings_from_runes(NULL), "");
    slang_strings_bytes(x, &r);
    assert(r.len == 1 + 2 + 3 + 4 && ((uint8_t *)r.data)[0] == 'a' && ((uint8_t *)r.data)[1] == 0xC3);
    EXPECT_STR(slang_strings_from_bytes(&r), "añ€😀");
    slang_slice_release(&r);
    EXPECT_STR(slang_strings_from_bytes(NULL), "");
    slang_strings_bytes(NULL, &r); assert(r.len == 0); slang_slice_release(&r);
    slang_strings_runes(NULL, &r); assert(r.len == 0); slang_slice_release(&r);
    slang_release(x);
    x = slang_string_new("\xff", 1);
    slang_strings_runes(x, &r);
    assert(r.len == 1 && ((int32_t *)r.data)[0] == 0xFFFD);
    slang_slice_release(&r);
    slang_release(x);
}

/* ---------------------------------------------------------------- math */

TEST(math_more) {
    assert(slang_math_round(2.5) == 3 && slang_math_round(-2.5) == -3 && slang_math_round(2.4) == 2);
    assert(slang_math_trunc(-2.7) == -2 && slang_math_trunc(2.7) == 2);
    assert(slang_math_mod(7, 3) == 1 && slang_math_mod(-7, 3) == -1 && isnan(slang_math_mod(1, 0)));
    assert(slang_math_hypot(3, 4) == 5);
    assert(fabs(slang_math_atan2(1, 1) - M_PI / 4) < 1e-15);
    assert(slang_math_atan2(0, -1) == M_PI);
    assert(slang_math_log2(8) == 3 && slang_math_log10(1000) == 3);
    assert(slang_math_is_nan(slang_math_nan()) && !slang_math_is_nan(1));
    double pinf = slang_math_inf(1), ninf = slang_math_inf(-1);
    assert(pinf > 0 && isinf(pinf) && ninf < 0 && isinf(ninf) && slang_math_inf(0) == pinf);
    assert(slang_math_is_inf(pinf, 1) && !slang_math_is_inf(pinf, -1) && slang_math_is_inf(pinf, 0));
    assert(slang_math_is_inf(ninf, -1) && !slang_math_is_inf(ninf, 1) && slang_math_is_inf(ninf, 0));
    assert(!slang_math_is_inf(slang_math_nan(), 0) && !slang_math_is_inf(1e308, 0));
    assert(slang_math_imin(3, -4) == -4 && slang_math_imax(3, -4) == 3);
    assert(slang_math_imin(INT64_MIN, INT64_MAX) == INT64_MIN);
    assert(slang_math_iabs(-5) == 5 && slang_math_iabs(5) == 5 && slang_math_iabs(0) == 0);
    assert(slang_math_iabs(INT64_MIN) == INT64_MIN);   /* wraps */
}

/* ---------------------------------------------------------------- random */

static void intn_zero_child(void) { slang_random_int_n(0); }
static void bytes_neg_child(void) { slang_slice b; slang_random_bytes(-1, &b); }

TEST(prng) {
    slang_random_seed(42);
    int64_t a1 = slang_random_int(), a2 = slang_random_int_n(1000);
    double a3 = slang_random_float();
    slang_random_seed(42);
    assert(slang_random_int() == a1 && slang_random_int_n(1000) == a2 && slang_random_float() == a3);
    assert(a1 >= 0 && a2 >= 0 && a2 < 1000 && a3 >= 0 && a3 < 1);
    slang_random_seed(43);
    assert(slang_random_int() != a1);

    int hits[10] = { 0 };
    for (int i = 0; i < 10000; i++) {
        int64_t v = slang_random_int_n(10);
        assert(v >= 0 && v < 10);
        hits[v]++;
        double f = slang_random_float();
        assert(f >= 0 && f < 1);
    }
    for (int i = 0; i < 10; i++) assert(hits[i] > 800 && hits[i] < 1200);
    assert(slang_random_int_n(1) == 0);
    int64_t big = slang_random_int_n(INT64_MAX);
    assert(big >= 0);

    slang_slice b;
    slang_random_bytes(13, &b);
    assert(b.len == 13);
    slang_slice_release(&b);
    slang_random_bytes(0, &b);
    assert(b.len == 0);
    slang_slice_release(&b);

    int64_t vals[20], sum = 0;
    for (int i = 0; i < 20; i++) vals[i] = i;
    slang_slice s;
    slang_slice_from(&s, 8, NULL, vals, 20);
    slang_random_shuffle(&s);
    int seen[20] = { 0 };
    bool moved = false;
    for (int i = 0; i < 20; i++) {
        int64_t v = ((int64_t *)s.data)[i];
        seen[v]++;
        sum += v;
        if (v != i) moved = true;
    }
    for (int i = 0; i < 20; i++) assert(seen[i] == 1);
    assert(sum == 190 && moved);
    slang_slice_release(&s);
    slang_random_shuffle(NULL);

    char buf[256];
    assert(run_child(intn_zero_child, buf, sizeof buf) != 0);
    assert(strstr(buf, "random: invalid argument to IntN"));
    assert(run_child(bytes_neg_child, buf, sizeof buf) != 0);
}

/* ---------------------------------------------------------------- crypto */

#define HEXFN(fn, in, hex) STR1(fn, in, hex)

TEST(hashes) {
    static const char *L448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const char *L896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                              "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    HEXFN(slang_crypto_hash_sha256_hex, "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    HEXFN(slang_crypto_hash_sha256_hex, "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    HEXFN(slang_crypto_hash_sha256_hex, L448, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    HEXFN(slang_crypto_hash_sha512_hex, "", "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                                            "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    HEXFN(slang_crypto_hash_sha512_hex, "abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                                               "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    HEXFN(slang_crypto_hash_sha512_hex, L896, "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                                              "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    HEXFN(slang_crypto_hash_sha1_hex, "", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    HEXFN(slang_crypto_hash_sha1_hex, "abc", "a9993e364706816aba3e25717850c26c9cd0d89d");
    HEXFN(slang_crypto_hash_sha1_hex, L448, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    /* RFC 1321 A.5 */
    HEXFN(slang_crypto_hash_md5_hex, "", "d41d8cd98f00b204e9800998ecf8427e");
    HEXFN(slang_crypto_hash_md5_hex, "a", "0cc175b9c0f1b6a831c399e269772661");
    HEXFN(slang_crypto_hash_md5_hex, "abc", "900150983cd24fb0d6963f7d28e17f72");
    HEXFN(slang_crypto_hash_md5_hex, "message digest", "f96b697d7cb7938d525a2f31aaf161d0");
    HEXFN(slang_crypto_hash_md5_hex, "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b");
    HEXFN(slang_crypto_hash_md5_hex, "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
          "57edf4a22be3c955ac49da2e2107b67a");
    EXPECT_STR(slang_crypto_hash_md5_hex(NULL), "d41d8cd98f00b204e9800998ecf8427e");

    /* one million 'a' (FIPS 180 long-message vectors) */
    char *mil = malloc(1000000);
    memset(mil, 'a', 1000000);
    slang_string *m = slang_string_new(mil, 1000000);
    free(mil);
    EXPECT_STR(slang_crypto_hash_sha256_hex(m), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    EXPECT_STR(slang_crypto_hash_sha1_hex(m), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    EXPECT_STR(slang_crypto_hash_sha512_hex(m), "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                                               "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
    slang_release(m);

    /* padding boundaries: 55/56/63/64 bytes (SHA-256) and 111/112 (SHA-512) agree with byte form */
    for (int n = 50; n < 130; n++) {
        char buf[130];
        memset(buf, 'x', (size_t)n);
        slang_string *s = slang_string_new(buf, n);
        slang_slice b;
        slang_crypto_hash_sha256(s, &b);
        slang_string *h1 = slang_encoding_hex_encode(&b), *h2 = slang_crypto_hash_sha256_hex(s);
        assert(b.len == 32 && slang_string_eq(h1, h2));
        slang_slice_release(&b); slang_release(h1); slang_release(h2);
        slang_crypto_hash_sha512(s, &b); assert(b.len == 64); slang_slice_release(&b);
        slang_crypto_hash_sha1(s, &b); assert(b.len == 20); slang_slice_release(&b);
        slang_crypto_hash_md5(s, &b); assert(b.len == 16); slang_slice_release(&b);
        slang_release(s);
    }
    {   /* 56 'a' bytes: known SHA-256 vector across the padding boundary */
        slang_string *a = S("a"), *s = slang_strings_repeat(a, 56);
        slang_release(a);
        EXPECT_STR(slang_crypto_hash_sha256_hex(s), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
        slang_release(s);
    }
    slang_slice b;
    slang_string *abc = S("abc");
    slang_crypto_hash_md5(abc, &b);
    assert(bytes_hex_is(&b, "900150983cd24fb0d6963f7d28e17f72"));
    slang_slice_release(&b);
    slang_release(abc);

    slang_string *s = S("123456789");
    assert(slang_crypto_hash_crc32(s) == 0xCBF43926);
    slang_release(s);
    s = S("The quick brown fox jumps over the lazy dog");
    assert(slang_crypto_hash_crc32(s) == 0x414FA339);
    slang_release(s);
    assert(slang_crypto_hash_crc32(NULL) == 0);
}

TEST(hmac) {
    /* RFC 4231 test cases 1, 2, 6 and 7 */
    char k1[20];
    memset(k1, 0x0b, 20);
    slang_string *key = slang_string_new(k1, 20), *data = S("Hi There");
    EXPECT_STR(slang_crypto_hmac_sha256_hex(key, data), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    slang_release(key); slang_release(data);
    key = S("Jefe"); data = S("what do ya want for nothing?");
    EXPECT_STR(slang_crypto_hmac_sha256_hex(key, data), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    slang_slice m1, m2;
    slang_crypto_hmac_sha256(key, data, &m1);
    assert(bytes_hex_is(&m1, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    slang_crypto_hmac_sha256(key, data, &m2);
    assert(slang_crypto_hmac_equal(&m1, &m2));
    ((uint8_t *)m2.data)[31] ^= 1;
    assert(!slang_crypto_hmac_equal(&m1, &m2));
    slang_slice short_;
    slang_slice_sub(&short_, &m1, 0, 31, 1);   /* different length */
    assert(!slang_crypto_hmac_equal(&m1, &short_));
    slang_slice_release(&short_);
    slang_slice_release(&m2);
    slang_slice_release(&m1);
    assert(slang_crypto_hmac_equal(NULL, NULL));
    slang_release(key); slang_release(data);
    char k6[131];
    memset(k6, 0xaa, 131);
    key = slang_string_new(k6, 131);
    data = S("Test Using Larger Than Block-Size Key - Hash Key First");
    EXPECT_STR(slang_crypto_hmac_sha256_hex(key, data), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    slang_release(data);
    data = S("This is a test using a larger than block-size key and a larger than block-size data. "
             "The key needs to be hashed before being used by the HMAC algorithm.");
    EXPECT_STR(slang_crypto_hmac_sha256_hex(key, data), "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
    slang_release(key); slang_release(data);
    EXPECT_STR(slang_crypto_hmac_sha256_hex(NULL, NULL), "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
}

TEST(crypto_rand) {
    slang_error *e = NULL;
    slang_slice b;
    slang_crypto_rand_bytes(32, &b, &e);
    must_ok(e);
    assert(b.len == 32);
    int nz = 0;
    for (int i = 0; i < 32; i++) nz += ((uint8_t *)b.data)[i] != 0;
    assert(nz > 0);
    slang_slice_release(&b);
    slang_crypto_rand_bytes(0, &b, &e);
    must_ok(e);
    assert(b.len == 0);
    slang_slice_release(&b);
    slang_crypto_rand_bytes(-1, &b, &e);
    assert(e != NULL && b.len == 0);
    slang_release(e); e = NULL;
    int64_t v = slang_crypto_rand_int(&e);
    must_ok(e);
    assert(v >= 0);
    assert(slang_crypto_rand_int(&e) != v);   /* 2^-63 chance of a false failure */
    slang_string *h = slang_crypto_rand_hex(16, &e);
    must_ok(e);
    assert(slang_string_len(h) == 32);
    slang_slice dec;
    slang_encoding_hex_decode(h, &dec, &e);
    must_ok(e);
    assert(dec.len == 16);
    slang_slice_release(&dec);
    slang_release(h);
    h = slang_crypto_rand_hex(0, &e);
    must_ok(e);
    assert(slang_string_len(h) == 0);
    slang_release(h);
    assert(slang_crypto_rand_hex(-3, &e) == NULL && e != NULL);
    slang_release(e);
}

/* ---------------------------------------------------------------- encoding */

static void b64_ok(const char *plain, const char *enc) {
    slang_string *p = S(plain), *en = S(enc);
    EXPECT_STR(slang_encoding_base64_encode_string(p), enc);
    slang_slice b;
    slang_string_to_bytes(&b, p);
    EXPECT_STR(slang_encoding_base64_encode(&b), enc);
    slang_slice_release(&b);
    slang_error *e = NULL;
    EXPECT_STR(slang_encoding_base64_decode_string(en, &e), plain);
    must_ok(e);
    slang_encoding_base64_decode(en, &b, &e);
    must_ok(e);
    slang_string *back = slang_string_from_bytes(&b);
    assert(slang_string_eq(back, p));
    slang_release(back);
    slang_slice_release(&b);
    slang_release(p); slang_release(en);
}

static void b64_bad(const char *enc, bool url, const char *want_msg) {
    slang_string *en = S(enc);
    slang_error *e = NULL;
    slang_slice b;
    if (url) slang_encoding_base64_decode_url(en, &b, &e);
    else slang_encoding_base64_decode(en, &b, &e);
    if (!e) { fprintf(stderr, "base64 '%s' decoded unexpectedly\n", enc); assert(0); }
    assert(b.len == 0);
    slang_string *m = slang_error_message(e);
    if (want_msg && strcmp(slang_string_data(m), want_msg) != 0) {
        fprintf(stderr, "base64 '%s': %s\n", enc, slang_string_data(m));
        assert(0);
    }
    slang_release(m);
    slang_release(e);
    e = NULL;
    if (!url) {
        assert(slang_encoding_base64_decode_string(en, &e) == NULL && e != NULL);
        slang_release(e);
    }
    slang_release(en);
}

TEST(base64) {
    /* RFC 4648 section 10 */
    b64_ok("", "");
    b64_ok("f", "Zg==");
    b64_ok("fo", "Zm8=");
    b64_ok("foo", "Zm9v");
    b64_ok("foob", "Zm9vYg==");
    b64_ok("fooba", "Zm9vYmE=");
    b64_ok("foobar", "Zm9vYmFy");
    b64_ok("héllo wörld", "aMOpbGxvIHfDtnJsZA==");
    EXPECT_STR(slang_encoding_base64_encode(NULL), "");

    slang_error *e = NULL;
    slang_string *nl = S("Zm9v\r\nYmFy\n");
    EXPECT_STR(slang_encoding_base64_decode_string(nl, &e), "foobar");
    must_ok(e);
    slang_release(nl);

    b64_bad("Zg=", false, "illegal base64 data at input byte 2");
    b64_bad("Zg", false, "illegal base64 data at input byte 2");
    b64_bad("Zm9v!", false, "illegal base64 data at input byte 4");
    b64_bad("Z===", false, NULL);
    b64_bad("Zg==Zg==", false, NULL);
    b64_bad("Z", false, NULL);
    b64_bad("Zm9-", false, NULL);   /* URL alphabet in std decoder */

    /* URL-safe: unpadded output, padding optional on input */
    uint8_t raw[] = { 0xfb, 0xff, 0xbf };
    slang_slice b, d;
    slang_slice_from(&b, 1, NULL, raw, 2);
    EXPECT_STR(slang_encoding_base64_encode(&b), "+/8=");
    EXPECT_STR(slang_encoding_base64_encode_url(&b), "-_8");
    slang_slice_release(&b);
    slang_slice_from(&b, 1, NULL, raw, 3);
    EXPECT_STR(slang_encoding_base64_encode_url(&b), "-_-_");
    slang_slice_release(&b);
    static const char *const ok_url[] = { "-_8", "-_8=" };
    for (int i = 0; i < 2; i++) {
        slang_string *s = S(ok_url[i]);
        slang_encoding_base64_decode_url(s, &d, &e);
        must_ok(e);
        assert(bytes_hex_is(&d, "fbff"));
        slang_slice_release(&d);
        slang_release(s);
    }
    b64_bad("+/8", true, NULL);
    b64_bad("-_8==", true, NULL);
    b64_bad("-_=", true, NULL);
    slang_encoding_base64_decode_url(NULL, &d, &e);
    must_ok(e);
    assert(d.len == 0);
    slang_slice_release(&d);

    /* all byte values round-trip */
    uint8_t all[256];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    for (int n = 0; n <= 256; n += 37) {
        slang_slice_from(&b, 1, NULL, all, n);
        slang_string *s1 = slang_encoding_base64_encode(&b), *s2 = slang_encoding_base64_encode_url(&b);
        slang_encoding_base64_decode(s1, &d, &e); must_ok(e);
        assert(d.len == n && memcmp(d.data, all, (size_t)n) == 0);
        slang_slice_release(&d);
        slang_encoding_base64_decode_url(s2, &d, &e); must_ok(e);
        assert(d.len == n && (n == 0 || memcmp(d.data, all, (size_t)n) == 0));
        slang_slice_release(&d);
        slang_release(s1); slang_release(s2);
        slang_slice_release(&b);
    }
}

TEST(hex) {
    STR1(slang_encoding_hex_encode_string, "Hello", "48656c6c6f");
    STR1(slang_encoding_hex_encode_string, "", "");
    EXPECT_STR(slang_encoding_hex_encode(NULL), "");
    slang_error *e = NULL;
    slang_string *s = S("48656C6c6f");
    EXPECT_STR(slang_encoding_hex_decode_string(s, &e), "Hello");
    must_ok(e);
    slang_slice b;
    slang_encoding_hex_decode(s, &b, &e);
    must_ok(e);
    assert(b.len == 5 && ((uint8_t *)b.data)[0] == 0x48);
    EXPECT_STR(slang_encoding_hex_encode(&b), "48656c6c6f");
    slang_slice_release(&b);
    slang_release(s);
    s = S("4G");
    slang_encoding_hex_decode(s, &b, &e);
    assert(e && b.len == 0);
    slang_string *m = slang_error_message(e);
    assert(STR_IS(m, "encoding/hex: invalid byte at offset 1"));
    slang_release(m); slang_release(e); e = NULL;
    slang_release(s);
    s = S("abc");
    assert(slang_encoding_hex_decode_string(s, &e) == NULL && e);
    m = slang_error_message(e);
    assert(STR_IS(m, "encoding/hex: odd length hex string"));
    slang_release(m); slang_release(e); e = NULL;
    slang_release(s);
    slang_encoding_hex_decode(NULL, &b, &e);
    must_ok(e);
    assert(b.len == 0);
    slang_slice_release(&b);
}

/* ---------------------------------------------------------------- uuid */

TEST(uuid) {
    for (int i = 0; i < 50; i++) {
        slang_string *u = slang_uuid_new();
        const char *p = slang_string_data(u);
        assert(slang_string_len(u) == 36 && slang_uuid_is_valid(u));
        assert(p[14] == '4' && strchr("89ab", p[19]) != NULL);
        for (int k = 0; k < 36; k++) assert(!(p[k] >= 'A' && p[k] <= 'F'));
        slang_string *v = slang_uuid_new();
        assert(!slang_string_eq(u, v));
        slang_release(u); slang_release(v);
    }
    static const char *const good[] = { "123e4567-e89b-12d3-a456-426614174000",
                                        "00000000-0000-0000-0000-000000000000",
                                        "A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11" };
    static const char *const bad[] = { "", "123e4567e89b12d3a456426614174000",
                                       "123e4567-e89b-12d3-a456-42661417400",
                                       "123e4567-e89b-12d3-a456-4266141740000",
                                       "123e4567-e89b-12d3-a456_426614174000",
                                       "g23e4567-e89b-12d3-a456-426614174000",
                                       "{23e4567-e89b-12d3-a456-426614174000}" };
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
        slang_string *s = S(good[i]); assert(slang_uuid_is_valid(s)); slang_release(s);
    }
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        slang_string *s = S(bad[i]); assert(!slang_uuid_is_valid(s)); slang_release(s);
    }
    assert(!slang_uuid_is_valid(NULL));
}

int main(int argc, char **argv) {
    (void)argc;
    slang_runtime_init(1, argv);
    printf("test_std2\n");
    int64_t live = slang_live_objects();
    RUN(time_calendar);
    RUN(time_format_parse);
    RUN(time_clock);
    RUN(os);
    RUN(os_exit);
    RUN(fs_paths);
    RUN(fs_files);
    RUN(io_lines);
    RUN(io_read_all);
    RUN(io_eprint);
    RUN(strings_more);
    RUN(math_more);
    RUN(prng);
    RUN(hashes);
    RUN(hmac);
    RUN(crypto_rand);
    RUN(base64);
    RUN(hex);
    RUN(uuid);
    if (slang_live_objects() != live) {
        fprintf(stderr, "leaked %lld objects\n", (long long)(slang_live_objects() - live));
        assert(0);
    }
    slang_runtime_shutdown();
    return 0;
}
