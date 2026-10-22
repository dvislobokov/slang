/* Tests for the second batch of std.* runtime functions: time, os, fs, io
 * additions, strings additions, math additions, random, crypto, encoding,
 * uuid. Hash/HMAC/base64 vectors are from FIPS 180-4 examples, RFC 1321,
 * RFC 4231 and RFC 4648. */
#include "test.h"
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

#define EXPECT_STR(call, lit) do { zell_string *r_ = (call); assert(r_ != NULL); \
        if (!STR_IS(r_, lit)) { fprintf(stderr, "got '%s' want '%s' (%s:%d)\n",  \
            zell_string_data(r_), lit, __FILE__, __LINE__); assert(0); }         \
        zell_release(r_); } while (0)

/* call taking one string literal argument */
#define STR1(fn, a, lit) do { zell_string *a_ = S(a); EXPECT_STR(fn(a_), lit); zell_release(a_); } while (0)
#define STR2(fn, a, b, lit) do { zell_string *a_ = S(a), *b_ = S(b); EXPECT_STR(fn(a_, b_), lit); \
        zell_release(a_); zell_release(b_); } while (0)

static bool slice_is(const zell_slice *s, int n, const char *const *v) {
    if (s->len != n) return false;
    for (int i = 0; i < n; i++)
        if (!STR_IS(((zell_string **)s->data)[i], v[i])) return false;
    return true;
}

static void strs(zell_slice *out, int n, const char *const *v) {
    zell_slice_new_ops(out, 8, n, n, &zell_ops_ref);
    for (int i = 0; i < n; i++) ((zell_string **)out->data)[i] = S(v[i]);
}

static bool bytes_hex_is(const zell_slice *b, const char *hex) {
    zell_string *h = zell_encoding_hex_encode(b);
    bool ok = STR_IS(h, hex);
    if (!ok) fprintf(stderr, "bytes %s want %s\n", zell_string_cstr(h), hex);
    zell_release(h);
    return ok;
}

/* ---------------------------------------------------------------- time */

TEST(time_calendar) {
    assert(zell_time_year(0) == 1970 && zell_time_month(0) == 1 && zell_time_day(0) == 1);
    assert(zell_time_weekday(0) == 4);   /* Thursday */
    int64_t t = 1700000000123;            /* 2023-11-14 22:13:20.123 UTC, Tuesday */
    assert(zell_time_year(t) == 2023 && zell_time_month(t) == 11 && zell_time_day(t) == 14);
    assert(zell_time_hour(t) == 22 && zell_time_minute(t) == 13 && zell_time_second(t) == 20);
    assert(zell_time_weekday(t) == 2);
    /* negative times floor to the previous second */
    assert(zell_time_year(-1) == 1969 && zell_time_second(-1) == 59 && zell_time_hour(-1) == 23);
    /* leap day */
    int64_t leap = 951782400000;          /* 2000-02-29 00:00:00 UTC */
    assert(zell_time_month(leap) == 2 && zell_time_day(leap) == 29);
}

TEST(time_format_parse) {
    zell_string *l = S("%Y-%m-%d %H:%M:%S.%L");
    EXPECT_STR(zell_time_format(1700000000123, l), "2023-11-14 22:13:20.123");
    EXPECT_STR(zell_time_format(-1, l), "1969-12-31 23:59:59.999");
    EXPECT_STR(zell_time_format(0, l), "1970-01-01 00:00:00.000");
    zell_release(l);
    l = S("%A %d %B %Y, %%, %j");
    EXPECT_STR(zell_time_format(0, l), "Thursday 01 January 1970, %, 001");
    zell_release(l);
    EXPECT_STR(zell_time_format(0, NULL), "");
    l = S("%p");   /* AM/PM in C locale */
    EXPECT_STR(zell_time_format(0, l), "AM");
    zell_release(l);
    /* long output grows the buffer */
    zell_string *y = S("%Y-"), *ly = zell_strings_repeat(y, 300);
    zell_string *r = zell_time_format(0, ly);
    assert(zell_string_len(r) == 300 * 5);
    zell_release(r); zell_release(ly); zell_release(y);

    zell_error *e = NULL;
    zell_string *s = S("2023-11-14 22:13:20"), *lay = S("%Y-%m-%d %H:%M:%S");
    assert(zell_time_parse(s, lay, &e) == 1700000000000 && e == NULL);
    zell_release(s);
    s = S("2023-11-14 22:13:20x");   /* trailing garbage */
    assert(zell_time_parse(s, lay, &e) == 0 && e != NULL);
    zell_string *m = zell_error_message(e);
    assert(strstr(zell_string_data(m), "cannot parse") != NULL);
    zell_release(m); zell_release(e); e = NULL;
    zell_release(s);
    s = S("nope");
    assert(zell_time_parse(s, lay, &e) == 0 && e != NULL);
    zell_release(e); e = NULL;
    zell_release(s); zell_release(lay);
    /* timezone offset via %z */
    s = S("2023-11-14T23:13:20+0100"); lay = S("%Y-%m-%dT%H:%M:%S%z");
    assert(zell_time_parse(s, lay, &e) == 1700000000000 && e == NULL);
    zell_release(s); zell_release(lay);
    /* date only, before the epoch */
    s = S("1969-12-31"); lay = S("%Y-%m-%d");
    assert(zell_time_parse(s, lay, &e) == -86400000 && e == NULL);
    zell_release(s); zell_release(lay);
    /* round trip */
    lay = S("%Y-%m-%d %H:%M:%S");
    s = zell_time_format(1234567890000, lay);
    assert(zell_time_parse(s, lay, NULL) == 1234567890000);
    zell_release(s); zell_release(lay);
}

TEST(time_clock) {
    int64_t a = zell_time_now(), an = zell_time_now_nanos();
    assert(a > 1600000000000 && an / 1000000 >= a - 1000);
    zell_time_sleep(20);
    assert(zell_time_since(a) >= 20);
    zell_time_sleep(0);
    zell_time_sleep(-5);
    assert(zell_time_now_nanos() > an);
}

/* ---------------------------------------------------------------- os */

TEST(os) {
    zell_string *n = S("ZELL_TEST_STD2_VAR"), *v = S("héllo=1");
    EXPECT_STR(zell_os_getenv(n), "");
    zell_error *e = NULL;
    zell_os_setenv(n, v, &e);
    assert(e == NULL);
    EXPECT_STR(zell_os_getenv(n), "héllo=1");
    zell_os_setenv(n, NULL, &e);
    assert(e == NULL);
    EXPECT_STR(zell_os_getenv(n), "");
    zell_release(n);
    n = S("BAD=NAME");
    zell_os_setenv(n, v, &e);
    assert(e != NULL);
    zell_release(e); e = NULL;
    zell_os_setenv(NULL, v, &e);
    assert(e != NULL);
    zell_release(e); e = NULL;
    zell_os_setenv(NULL, v, NULL);   /* discarded error does not leak */
    zell_release(n); zell_release(v);

    assert(zell_os_pid() == (int64_t)getpid());
    zell_string *h = zell_os_hostname(&e);
    assert(e == NULL && zell_string_len(h) > 0);
    zell_release(h);

    zell_string *cwd = zell_os_cwd(&e);
#if !defined(_WIN32)
    assert(e == NULL && zell_string_data(cwd)[0] == '/');
    zell_string *tmp = S("/");
    zell_os_chdir(tmp, &e);
    assert(e == NULL);
    EXPECT_STR(zell_os_cwd(NULL), "/");
#else
    assert(e == NULL && zell_string_data(cwd)[1] == ':' && zell_string_data(cwd)[2] == '\\');  /* C:\... */
    zell_string *tmp = S("C:\\");
    zell_os_chdir(tmp, &e);
    assert(e == NULL);
    EXPECT_STR(zell_os_cwd(NULL), "C:\\");
#endif
    zell_os_chdir(cwd, &e);
    assert(e == NULL);
    zell_release(tmp);
    tmp = S("/definitely/not/here");
    zell_os_chdir(tmp, &e);
    assert(e != NULL);
    zell_string *m = zell_error_message(e);
    assert(strstr(zell_string_data(m), "chdir /definitely/not/here: ") != NULL);
    zell_release(m); zell_release(e); zell_release(tmp);
    zell_string *now = zell_os_cwd(NULL);
    assert(zell_string_eq(now, cwd));
    zell_release(now); zell_release(cwd);

    /* Args aliases the runtime's argv slice */
    zell_slice args;
    zell_os_args(&args);
    assert(args.len == 1);
    zell_slice_release(&args);
}

static void exit_child(void) {
    zell_print_cstr("bye");
    zell_os_exit(7);
}

TEST(os_exit) {
    char buf[64];
    assert(run_child_fd(exit_child, 1, buf, sizeof buf) == 7);
    assert(strcmp(buf, "bye") == 0);   /* stdout flushed before exiting */
}

/* ---------------------------------------------------------------- fs */

static zell_string *join(const char *a, const char *b) {
    zell_string *x = S(a), *y = S(b), *r = zell_fs_join(x, y);
    zell_release(x); zell_release(y);
    return r;
}

/* Expected paths are written with '/'; std.fs gives the platform's
 * separator (Windows: '\\'). */
static const char *native_path(const char *lit) {
    static char buf[4][256];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, sizeof buf[0], "%s", lit);
#if defined(_WIN32)
    for (char *c = b; *c; c++)
        if (*c == '/') *c = '\\';
#endif
    return b;
}
#define EXPECT_PATH(call, lit) do { zell_string *r_ = (call); const char *w_ = native_path(lit); assert(r_ != NULL); \
        if (!STR_IS(r_, w_)) { fprintf(stderr, "got '%s' want '%s' (%s:%d)\n", zell_string_cstr(r_), w_, __FILE__, __LINE__); assert(0); } \
        zell_release(r_); } while (0)
#define PATH1(fn, a, lit) do { zell_string *a_ = S(a); EXPECT_PATH(fn(a_), lit); zell_release(a_); } while (0)

TEST(fs_paths) {
    EXPECT_PATH(join("a", "b"), "a/b");
    EXPECT_PATH(join("a/", "/b"), "a/b");
    EXPECT_PATH(join("a/b", "../c"), "a/c");
    EXPECT_PATH(join("/", "x"), "/x");
    EXPECT_PATH(join("", "b"), "b");
    EXPECT_PATH(join("a", ""), "a");
    EXPECT_PATH(join("", ""), "");
    EXPECT_PATH(join("a", "../.."), "..");
    EXPECT_PATH(join("/a", "../../.."), "/");
    EXPECT_PATH(join("./a/./b/", "."), "a/b");
    EXPECT_PATH(zell_fs_join(NULL, NULL), "");

    PATH1(zell_fs_base, "/a/b.txt", "b.txt");
    PATH1(zell_fs_base, "a/b/", "b");
    PATH1(zell_fs_base, "///", "/");
    PATH1(zell_fs_base, "", ".");
    PATH1(zell_fs_base, "x", "x");

    PATH1(zell_fs_dir, "/a/b.txt", "/a");
    PATH1(zell_fs_dir, "a/b/", "a/b");
    PATH1(zell_fs_dir, "b", ".");
    PATH1(zell_fs_dir, "/", "/");
    PATH1(zell_fs_dir, "", ".");
    PATH1(zell_fs_dir, "/x", "/");
    PATH1(zell_fs_dir, "a//b", "a");

    STR1(zell_fs_ext, "a/b.tar.gz", ".gz");
    STR1(zell_fs_ext, "a.d/b", "");
    STR1(zell_fs_ext, ".bashrc", ".bashrc");
    STR1(zell_fs_ext, "noext", "");
    STR1(zell_fs_ext, "dot.", ".");

#if !defined(_WIN32)
    zell_string *p = S("/x/../y/./z");
    EXPECT_STR(zell_fs_abs(p, NULL), "/y/z");
    zell_release(p);
#else
    zell_string *p = S("C:/x/../y/./z");  /* Windows: absolute means a drive (or UNC) */
    EXPECT_STR(zell_fs_abs(p, NULL), "C:\\y\\z");
    zell_release(p);
#endif
    p = S("rel/f");
    zell_string *a = zell_fs_abs(p, NULL), *cwd = zell_os_cwd(NULL), *q = S(native_path("rel/f"));
    assert(zell_string_has_prefix(a, cwd) && zell_string_has_suffix(a, q));
    zell_release(a); zell_release(cwd); zell_release(p); zell_release(q);

    zell_string *td = zell_fs_temp_dir();
    assert(zell_string_len(td) > 0);
    zell_release(td);
}

static zell_string *in_dir(const zell_string *dir, const char *name) {
    zell_string *n = S(name), *r = zell_fs_join(dir, n);
    zell_release(n);
    return r;
}

static void must_ok(zell_error *e) {
    if (e) {
        zell_string *m = zell_error_message(e);
        fprintf(stderr, "unexpected error: %s\n", zell_string_cstr(m));
        assert(0);
    }
}

TEST(fs_files) {
    char tmpl[] = "/tmp/zell_std2_XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    zell_string *root = S(tmpl);
    zell_error *e = NULL;
    assert(zell_fs_exists(root) && zell_fs_is_dir(root));

    zell_string *deep = in_dir(root, "a/b/c");
    zell_fs_mkdir_all(deep, &e); must_ok(e);
    zell_fs_mkdir_all(deep, &e); must_ok(e);        /* already exists: ok */
    assert(zell_fs_is_dir(deep));
    zell_fs_mkdir(deep, &e);                        /* plain Mkdir: EEXIST */
    assert(e != NULL); zell_release(e); e = NULL;

    zell_string *f = in_dir(root, "a/file.txt"), *d1 = S("hello "), *d2 = S("world\n");
    assert(!zell_fs_exists(f));
    zell_fs_append_file(f, d1, &e); must_ok(e);
    zell_fs_append_file(f, d2, &e); must_ok(e);
    zell_fs_append_file(f, NULL, &e); must_ok(e);
    assert(zell_fs_exists(f) && !zell_fs_is_dir(f));
    assert(zell_fs_size(f, &e) == 12); must_ok(e);
    int64_t mt = zell_fs_mod_time(f, &e); must_ok(e);
    assert(llabs(mt - zell_time_now()) < 60000);

    /* MkdirAll through a file fails */
    zell_string *thru = in_dir(root, "a/file.txt/x");
    zell_fs_mkdir_all(thru, &e);
    assert(e != NULL); zell_release(e); e = NULL;
    zell_release(thru);

    zell_string *g = in_dir(root, "a/copy.txt");
    zell_fs_copy(f, g, &e); must_ok(e);
    zell_string *c = zell_read_file(g, &e); must_ok(e);
    assert(STR_IS(c, "hello world\n"));
    zell_release(c);
    zell_fs_copy(deep, g, &e);                      /* copying a directory */
    assert(e != NULL); zell_release(e); e = NULL;

    zell_string *dir_a = in_dir(root, "a");
    zell_slice names;
    zell_fs_read_dir(dir_a, &names, &e); must_ok(e);
    static const char *const want[] = { "b", "copy.txt", "file.txt" };
    assert(slice_is(&names, 3, want));
    zell_slice_release(&names);

    zell_string *h = in_dir(root, "a/renamed.txt");
    zell_fs_rename(g, h, &e); must_ok(e);
    assert(!zell_fs_exists(g) && zell_fs_exists(h));

    zell_fs_remove(dir_a, &e);                      /* not empty */
    assert(e != NULL); zell_release(e); e = NULL;
    zell_fs_remove(h, &e); must_ok(e);
    zell_fs_remove(h, &e);                          /* already gone */
    assert(e != NULL);
    zell_string *m = zell_error_message(e);
    assert(strstr(zell_string_data(m), "remove ") && strstr(zell_string_data(m), "renamed.txt: "));
    zell_release(m); zell_release(e); e = NULL;

    /* errors on missing paths */
    zell_string *missing = in_dir(root, "nope");
    assert(!zell_fs_exists(missing) && !zell_fs_is_dir(missing));
    assert(zell_fs_size(missing, &e) == 0 && e); zell_release(e); e = NULL;
    assert(zell_fs_mod_time(missing, &e) == 0 && e); zell_release(e); e = NULL;
    zell_fs_read_dir(missing, &names, &e);
    assert(e && names.len == 0); zell_release(e); e = NULL;
    zell_fs_rename(missing, h, &e); assert(e); zell_release(e); e = NULL;
    zell_fs_copy(missing, h, &e); assert(e); zell_release(e); e = NULL;
    zell_fs_remove_all(missing, &e); must_ok(e);    /* Go: missing is fine */
    zell_fs_remove_all(NULL, &e); must_ok(e);
    zell_string *nul = zell_string_new("/tmp\0x", 6);   /* embedded NUL: never a file */
    assert(!zell_fs_exists(NULL) && !zell_fs_exists(nul) && !zell_fs_is_dir(nul));
    zell_fs_mkdir(nul, &e); assert(e); zell_release(e); e = NULL;
    zell_release(nul);

    /* empty dir lists as [] */
    zell_fs_read_dir(deep, &names, &e); must_ok(e);
    assert(names.len == 0);
    zell_slice_release(&names);

    /* a symlink inside is removed, its target is not */
    zell_string *outside = S("/tmp"), *link = in_dir(root, "a/b/lnk");
    assert(symlink("/tmp", zell_string_data(link)) == 0);
    zell_fs_remove_all(root, &e); must_ok(e);
    assert(!zell_fs_exists(root) && zell_fs_is_dir(outside));

    zell_release(outside); zell_release(link);
    zell_release(missing); zell_release(h); zell_release(dir_a); zell_release(g);
    zell_release(f); zell_release(d1); zell_release(d2); zell_release(deep);
    zell_release(root);
}

/* ---------------------------------------------------------------- io */

TEST(io_lines) {
    char path[] = "/tmp/zell_std2_lines_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    zell_string *p = S(path);
    zell_error *e = NULL;
    static const char *const v[] = { "one", "", "three é" };
    zell_slice lines, got;
    strs(&lines, 3, v);
    zell_io_write_lines(p, &lines, &e); must_ok(e);
    EXPECT_STR(zell_read_file(p, NULL), "one\n\nthree é\n");
    zell_io_read_lines(p, &got, &e); must_ok(e);
    assert(slice_is(&got, 3, v));
    zell_slice_release(&got);
    zell_slice_release(&lines);

    /* CRLF and no final newline */
    zell_string *raw = S("a\r\nb\nc");
    zell_io_write_file(p, raw, &e); must_ok(e);
    zell_io_read_lines(p, &got, &e); must_ok(e);
    static const char *const v2[] = { "a", "b", "c" };
    assert(slice_is(&got, 3, v2));
    zell_slice_release(&got);
    zell_release(raw);

    /* empty file and nil slice */
    zell_io_write_lines(p, NULL, &e); must_ok(e);
    zell_io_read_lines(p, &got, &e); must_ok(e);
    assert(got.len == 0);
    zell_slice_release(&got);
    raw = S("\n");
    zell_io_write_file(p, raw, &e); must_ok(e);
    zell_io_read_lines(p, &got, &e); must_ok(e);
    static const char *const v3[] = { "" };
    assert(slice_is(&got, 1, v3));
    zell_slice_release(&got);
    zell_release(raw);

    unlink(path);
    zell_io_read_lines(p, &got, &e);
    assert(e != NULL && got.len == 0);
    zell_release(e); e = NULL;
    zell_io_read_lines(p, &got, NULL);
    zell_string *bad = S("/nonexistent-dir/x");
    zell_io_write_lines(bad, NULL, &e);
    assert(e != NULL); zell_release(e);
    zell_release(bad);
    zell_release(p);
}

static const char *g_stdin_path;
static void read_all_child(void) {
    /* g_stdin_path survives fork; a re-executed child (Windows) gets the env */
    const char *in = g_stdin_path ? g_stdin_path : getenv("ZELL_STD2_STDIN");
    assert(in && freopen(in, "r", stdin) != NULL);
    zell_error *e = NULL;
    zell_string *s = zell_io_read_all(&e);
    assert(e == NULL);
    printf("[%s]%lld", zell_string_cstr(s), (long long)zell_string_len(s));
    zell_release(s);
    s = zell_io_read_all(&e);   /* at EOF: "" and no error */
    assert(e == NULL && zell_string_len(s) == 0);
    zell_release(s);
    zell_io_eprintln(NULL);
    fflush(stdout);
}

TEST(io_read_all) {
    char path[] = "/tmp/zell_std2_stdin_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, "x\ny\n", 4) == 4);
    close(fd);
    g_stdin_path = path;
    setenv("ZELL_STD2_STDIN", path, 1);
    char buf[64];
    assert(run_child_fd(read_all_child, 1, buf, sizeof buf) == 0);
    assert(strcmp(buf, "[x\ny\n]4") == 0);
    unlink(path);
}

static void eprint_child(void) {
    zell_string *a = S("err "), *b = S("line");
    zell_io_eprint(a);
    zell_io_eprintln(b);
    zell_release(a); zell_release(b);
}

TEST(io_eprint) {
    char buf[64];
    assert(run_child(eprint_child, buf, sizeof buf) == 0);
    assert(strcmp(buf, "err line\n") == 0);
}

/* ---------------------------------------------------------------- strings */

#define SL(call, ...) do {                                                       \
        static const char *const exp_[] = { __VA_ARGS__ };                       \
        zell_slice r_; call;                                                    \
        assert(slice_is(&r_, (int)(sizeof exp_ / sizeof *exp_), exp_));          \
        zell_slice_release(&r_);                                                \
    } while (0)

TEST(strings_more) {
    zell_string *s = S("  foo bar\t\n baz  "), *x, *y;
    SL(zell_strings_fields(s, &r_), "foo", "bar", "baz");
    zell_release(s);
    zell_slice r;
    zell_strings_fields(NULL, &r); assert(r.len == 0); zell_slice_release(&r);
    s = S(" \t "); zell_strings_fields(s, &r); assert(r.len == 0); zell_slice_release(&r); zell_release(s);
    s = S("a\xc2\xa0" "b\xe3\x80\x80" "c");   /* NBSP and ideographic space */
    SL(zell_strings_fields(s, &r_), "a", "b", "c");
    zell_release(s);

    x = S("cheese"); y = S("e");
    assert(zell_strings_count(x, y) == 3);
    zell_release(y); y = S("");
    assert(zell_strings_count(x, y) == 7);   /* runes + 1 */
    zell_release(y); y = S("ee");
    assert(zell_strings_count(x, y) == 1);
    assert(zell_strings_last_index(x, y) == 2);
    zell_release(y); y = S("e");
    assert(zell_strings_last_index(x, y) == 5);
    zell_release(y); y = S("");
    assert(zell_strings_last_index(x, y) == 6);
    zell_release(y); y = S("z");
    assert(zell_strings_last_index(x, y) == -1);
    assert(zell_strings_count(NULL, NULL) == 1);
    zell_release(x); zell_release(y);

    STR2(zell_strings_trim_prefix, "prefix-body", "prefix-", "body");
    STR2(zell_strings_trim_prefix, "body", "x", "body");
    STR2(zell_strings_trim_suffix, "file.go", ".go", "file");
    STR2(zell_strings_trim_suffix, "file.go", "", "file.go");
    STR2(zell_strings_trim_left, "xxyhixy", "xy", "hixy");
    STR2(zell_strings_trim_right, "xxyhixy", "xy", "xxyhi");
    STR2(zell_strings_trim_left, "ééaé", "é", "aé");
    STR2(zell_strings_trim_right, "aéé", "é", "a");
    STR2(zell_strings_trim_right, "abc", "", "abc");
    STR2(zell_strings_trim_left, "aaa", "a", "");
    /* trimming an invalid byte only with that byte in the cutset */
    x = zell_string_new("\xff" "a\xff", 3); y = zell_string_new("\xff", 1);
    zell_string *t = zell_strings_trim_left(x, y);
    assert(zell_string_len(t) == 2); zell_release(t);
    t = zell_strings_trim_right(x, y);
    assert(zell_string_len(t) == 2); zell_release(t);
    zell_release(y); y = S("\xef\xbf\xbd");   /* U+FFFD does not match invalid bytes */
    t = zell_strings_trim_left(x, y);
    assert(zell_string_len(t) == 3); zell_release(t);
    zell_release(x); zell_release(y);

    x = S("a,b,c,d"); y = S(",");
    SL(zell_strings_split_n(x, y, 2, &r_), "a", "b,c,d");
    SL(zell_strings_split_n(x, y, 1, &r_), "a,b,c,d");
    SL(zell_strings_split_n(x, y, -1, &r_), "a", "b", "c", "d");
    SL(zell_strings_split_n(x, y, 10, &r_), "a", "b", "c", "d");
    zell_strings_split_n(x, y, 0, &r); assert(r.len == 0); zell_slice_release(&r);
    zell_release(y); y = S("");
    SL(zell_strings_split_n(x, y, 3, &r_), "a", ",", "b,c,d");
    zell_release(x); zell_release(y);
    x = S(""); y = S(",");
    SL(zell_strings_split_n(x, y, 2, &r_), "");
    zell_release(x); zell_release(y);

    x = S("Go"); y = S("GO");
    assert(zell_strings_equal_fold(x, y));
    zell_release(y); y = S("Gö");
    assert(!zell_strings_equal_fold(x, y));
    zell_release(x); zell_release(y);
    x = S("ΣΊΣΥΦΟΣ"); y = S("σίσυφος");   /* final sigma folds */
    assert(zell_strings_equal_fold(x, y));
    zell_release(x); zell_release(y);
    x = S("ПРИВЕТ"); y = S("привет");
    assert(zell_strings_equal_fold(x, y));
    assert(zell_strings_compare(x, y) == -1 && zell_strings_compare(y, x) == 1);
    assert(zell_strings_compare(x, x) == 0 && zell_strings_compare(NULL, NULL) == 0);
    zell_release(x); zell_release(y);

    STR1(zell_strings_reverse, "héllo", "olléh");
    STR1(zell_strings_reverse, "", "");
    STR1(zell_strings_reverse, "日本語", "語本日");
    x = zell_string_new("a\xff" "b", 3);
    t = zell_strings_reverse(x);
    assert(memcmp(zell_string_data(t), "b\xff" "a", 3) == 0);
    zell_release(t); zell_release(x);

    x = S("42"); y = S("0");
    EXPECT_STR(zell_strings_pad_left(x, 5, y), "00042");
    EXPECT_STR(zell_strings_pad_right(x, 5, y), "42000");
    EXPECT_STR(zell_strings_pad_left(x, 1, y), "42");
    EXPECT_STR(zell_strings_pad_left(x, -3, y), "42");
    EXPECT_STR(zell_strings_pad_left(x, 5, NULL), "42");
    zell_release(y); y = S("ab");
    EXPECT_STR(zell_strings_pad_left(x, 7, y), "ababa42");
    zell_release(x); x = S("é");
    zell_release(y); y = S("·");
    EXPECT_STR(zell_strings_pad_right(x, 3, y), "é··");   /* width counts runes */
    zell_release(x); zell_release(y);

    STR1(zell_strings_title, "hello wOrld  of zell", "Hello WOrld  Of Zell");
    STR1(zell_strings_title, "élan über", "Élan Über");
    STR1(zell_strings_title, "", "");

    /* runes / bytes */
    x = S("añ€😀");
    zell_strings_runes(x, &r);
    assert(r.len == 4);
    int32_t *rs = r.data;
    assert(rs[0] == 'a' && rs[1] == 0xF1 && rs[2] == 0x20AC && rs[3] == 0x1F600);
    EXPECT_STR(zell_strings_from_runes(&r), "añ€😀");
    rs[0] = -1; rs[1] = 0xD800; rs[2] = 0x110000;
    EXPECT_STR(zell_strings_from_runes(&r), "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd😀");
    zell_slice_release(&r);
    EXPECT_STR(zell_strings_from_runes(NULL), "");
    zell_strings_bytes(x, &r);
    assert(r.len == 1 + 2 + 3 + 4 && ((uint8_t *)r.data)[0] == 'a' && ((uint8_t *)r.data)[1] == 0xC3);
    EXPECT_STR(zell_strings_from_bytes(&r), "añ€😀");
    zell_slice_release(&r);
    EXPECT_STR(zell_strings_from_bytes(NULL), "");
    zell_strings_bytes(NULL, &r); assert(r.len == 0); zell_slice_release(&r);
    zell_strings_runes(NULL, &r); assert(r.len == 0); zell_slice_release(&r);
    zell_release(x);
    x = zell_string_new("\xff", 1);
    zell_strings_runes(x, &r);
    assert(r.len == 1 && ((int32_t *)r.data)[0] == 0xFFFD);
    zell_slice_release(&r);
    zell_release(x);
}

/* ---------------------------------------------------------------- math */

TEST(math_more) {
    assert(zell_math_round(2.5) == 3 && zell_math_round(-2.5) == -3 && zell_math_round(2.4) == 2);
    assert(zell_math_trunc(-2.7) == -2 && zell_math_trunc(2.7) == 2);
    assert(zell_math_mod(7, 3) == 1 && zell_math_mod(-7, 3) == -1 && isnan(zell_math_mod(1, 0)));
    assert(zell_math_hypot(3, 4) == 5);
    assert(fabs(zell_math_atan2(1, 1) - M_PI / 4) < 1e-15);
    assert(zell_math_atan2(0, -1) == M_PI);
    assert(zell_math_log2(8) == 3 && zell_math_log10(1000) == 3);
    assert(zell_math_is_nan(zell_math_nan()) && !zell_math_is_nan(1));
    double pinf = zell_math_inf(1), ninf = zell_math_inf(-1);
    assert(pinf > 0 && isinf(pinf) && ninf < 0 && isinf(ninf) && zell_math_inf(0) == pinf);
    assert(zell_math_is_inf(pinf, 1) && !zell_math_is_inf(pinf, -1) && zell_math_is_inf(pinf, 0));
    assert(zell_math_is_inf(ninf, -1) && !zell_math_is_inf(ninf, 1) && zell_math_is_inf(ninf, 0));
    assert(!zell_math_is_inf(zell_math_nan(), 0) && !zell_math_is_inf(1e308, 0));
    assert(zell_math_imin(3, -4) == -4 && zell_math_imax(3, -4) == 3);
    assert(zell_math_imin(INT64_MIN, INT64_MAX) == INT64_MIN);
    assert(zell_math_iabs(-5) == 5 && zell_math_iabs(5) == 5 && zell_math_iabs(0) == 0);
    assert(zell_math_iabs(INT64_MIN) == INT64_MIN);   /* wraps */
}

/* ---------------------------------------------------------------- random */

static void intn_zero_child(void) { zell_random_int_n(0); }
static void bytes_neg_child(void) { zell_slice b; zell_random_bytes(-1, &b); }

TEST(prng) {
    zell_random_seed(42);
    int64_t a1 = zell_random_int(), a2 = zell_random_int_n(1000);
    double a3 = zell_random_float();
    zell_random_seed(42);
    assert(zell_random_int() == a1 && zell_random_int_n(1000) == a2 && zell_random_float() == a3);
    assert(a1 >= 0 && a2 >= 0 && a2 < 1000 && a3 >= 0 && a3 < 1);
    zell_random_seed(43);
    assert(zell_random_int() != a1);

    int hits[10] = { 0 };
    for (int i = 0; i < 10000; i++) {
        int64_t v = zell_random_int_n(10);
        assert(v >= 0 && v < 10);
        hits[v]++;
        double f = zell_random_float();
        assert(f >= 0 && f < 1);
    }
    for (int i = 0; i < 10; i++) assert(hits[i] > 800 && hits[i] < 1200);
    assert(zell_random_int_n(1) == 0);
    int64_t big = zell_random_int_n(INT64_MAX);
    assert(big >= 0);

    zell_slice b;
    zell_random_bytes(13, &b);
    assert(b.len == 13);
    zell_slice_release(&b);
    zell_random_bytes(0, &b);
    assert(b.len == 0);
    zell_slice_release(&b);

    int64_t vals[20], sum = 0;
    for (int i = 0; i < 20; i++) vals[i] = i;
    zell_slice s;
    zell_slice_from(&s, 8, NULL, vals, 20);
    zell_random_shuffle(&s);
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
    zell_slice_release(&s);
    zell_random_shuffle(NULL);

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
    HEXFN(zell_crypto_hash_sha256_hex, "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    HEXFN(zell_crypto_hash_sha256_hex, "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    HEXFN(zell_crypto_hash_sha256_hex, L448, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    HEXFN(zell_crypto_hash_sha512_hex, "", "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                                            "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    HEXFN(zell_crypto_hash_sha512_hex, "abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                                               "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    HEXFN(zell_crypto_hash_sha512_hex, L896, "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                                              "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    HEXFN(zell_crypto_hash_sha1_hex, "", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    HEXFN(zell_crypto_hash_sha1_hex, "abc", "a9993e364706816aba3e25717850c26c9cd0d89d");
    HEXFN(zell_crypto_hash_sha1_hex, L448, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    /* RFC 1321 A.5 */
    HEXFN(zell_crypto_hash_md5_hex, "", "d41d8cd98f00b204e9800998ecf8427e");
    HEXFN(zell_crypto_hash_md5_hex, "a", "0cc175b9c0f1b6a831c399e269772661");
    HEXFN(zell_crypto_hash_md5_hex, "abc", "900150983cd24fb0d6963f7d28e17f72");
    HEXFN(zell_crypto_hash_md5_hex, "message digest", "f96b697d7cb7938d525a2f31aaf161d0");
    HEXFN(zell_crypto_hash_md5_hex, "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b");
    HEXFN(zell_crypto_hash_md5_hex, "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
          "57edf4a22be3c955ac49da2e2107b67a");
    EXPECT_STR(zell_crypto_hash_md5_hex(NULL), "d41d8cd98f00b204e9800998ecf8427e");

    /* one million 'a' (FIPS 180 long-message vectors) */
    char *mil = malloc(1000000);
    memset(mil, 'a', 1000000);
    zell_string *m = zell_string_new(mil, 1000000);
    free(mil);
    EXPECT_STR(zell_crypto_hash_sha256_hex(m), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    EXPECT_STR(zell_crypto_hash_sha1_hex(m), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    EXPECT_STR(zell_crypto_hash_sha512_hex(m), "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                                               "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
    zell_release(m);

    /* padding boundaries: 55/56/63/64 bytes (SHA-256) and 111/112 (SHA-512) agree with byte form */
    for (int n = 50; n < 130; n++) {
        char buf[130];
        memset(buf, 'x', (size_t)n);
        zell_string *s = zell_string_new(buf, n);
        zell_slice b;
        zell_crypto_hash_sha256(s, &b);
        zell_string *h1 = zell_encoding_hex_encode(&b), *h2 = zell_crypto_hash_sha256_hex(s);
        assert(b.len == 32 && zell_string_eq(h1, h2));
        zell_slice_release(&b); zell_release(h1); zell_release(h2);
        zell_crypto_hash_sha512(s, &b); assert(b.len == 64); zell_slice_release(&b);
        zell_crypto_hash_sha1(s, &b); assert(b.len == 20); zell_slice_release(&b);
        zell_crypto_hash_md5(s, &b); assert(b.len == 16); zell_slice_release(&b);
        zell_release(s);
    }
    {   /* 56 'a' bytes: known SHA-256 vector across the padding boundary */
        zell_string *a = S("a"), *s = zell_strings_repeat(a, 56);
        zell_release(a);
        EXPECT_STR(zell_crypto_hash_sha256_hex(s), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
        zell_release(s);
    }
    zell_slice b;
    zell_string *abc = S("abc");
    zell_crypto_hash_md5(abc, &b);
    assert(bytes_hex_is(&b, "900150983cd24fb0d6963f7d28e17f72"));
    zell_slice_release(&b);
    zell_release(abc);

    zell_string *s = S("123456789");
    assert(zell_crypto_hash_crc32(s) == 0xCBF43926);
    zell_release(s);
    s = S("The quick brown fox jumps over the lazy dog");
    assert(zell_crypto_hash_crc32(s) == 0x414FA339);
    zell_release(s);
    assert(zell_crypto_hash_crc32(NULL) == 0);
}

TEST(hmac) {
    /* RFC 4231 test cases 1, 2, 6 and 7 */
    char k1[20];
    memset(k1, 0x0b, 20);
    zell_string *key = zell_string_new(k1, 20), *data = S("Hi There");
    EXPECT_STR(zell_crypto_hmac_sha256_hex(key, data), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    zell_release(key); zell_release(data);
    key = S("Jefe"); data = S("what do ya want for nothing?");
    EXPECT_STR(zell_crypto_hmac_sha256_hex(key, data), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    zell_slice m1, m2;
    zell_crypto_hmac_sha256(key, data, &m1);
    assert(bytes_hex_is(&m1, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    zell_crypto_hmac_sha256(key, data, &m2);
    assert(zell_crypto_hmac_equal(&m1, &m2));
    ((uint8_t *)m2.data)[31] ^= 1;
    assert(!zell_crypto_hmac_equal(&m1, &m2));
    zell_slice short_;
    zell_slice_sub(&short_, &m1, 0, 31, 1);   /* different length */
    assert(!zell_crypto_hmac_equal(&m1, &short_));
    zell_slice_release(&short_);
    zell_slice_release(&m2);
    zell_slice_release(&m1);
    assert(zell_crypto_hmac_equal(NULL, NULL));
    zell_release(key); zell_release(data);
    char k6[131];
    memset(k6, 0xaa, 131);
    key = zell_string_new(k6, 131);
    data = S("Test Using Larger Than Block-Size Key - Hash Key First");
    EXPECT_STR(zell_crypto_hmac_sha256_hex(key, data), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    zell_release(data);
    data = S("This is a test using a larger than block-size key and a larger than block-size data. "
             "The key needs to be hashed before being used by the HMAC algorithm.");
    EXPECT_STR(zell_crypto_hmac_sha256_hex(key, data), "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
    zell_release(key); zell_release(data);
    EXPECT_STR(zell_crypto_hmac_sha256_hex(NULL, NULL), "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
}

TEST(crypto_rand) {
    zell_error *e = NULL;
    zell_slice b;
    zell_crypto_rand_bytes(32, &b, &e);
    must_ok(e);
    assert(b.len == 32);
    int nz = 0;
    for (int i = 0; i < 32; i++) nz += ((uint8_t *)b.data)[i] != 0;
    assert(nz > 0);
    zell_slice_release(&b);
    zell_crypto_rand_bytes(0, &b, &e);
    must_ok(e);
    assert(b.len == 0);
    zell_slice_release(&b);
    zell_crypto_rand_bytes(-1, &b, &e);
    assert(e != NULL && b.len == 0);
    zell_release(e); e = NULL;
    int64_t v = zell_crypto_rand_int(&e);
    must_ok(e);
    assert(v >= 0);
    assert(zell_crypto_rand_int(&e) != v);   /* 2^-63 chance of a false failure */
    zell_string *h = zell_crypto_rand_hex(16, &e);
    must_ok(e);
    assert(zell_string_len(h) == 32);
    zell_slice dec;
    zell_encoding_hex_decode(h, &dec, &e);
    must_ok(e);
    assert(dec.len == 16);
    zell_slice_release(&dec);
    zell_release(h);
    h = zell_crypto_rand_hex(0, &e);
    must_ok(e);
    assert(zell_string_len(h) == 0);
    zell_release(h);
    assert(zell_crypto_rand_hex(-3, &e) == NULL && e != NULL);
    zell_release(e);
}

/* ---------------------------------------------------------------- encoding */

static void b64_ok(const char *plain, const char *enc) {
    zell_string *p = S(plain), *en = S(enc);
    EXPECT_STR(zell_encoding_base64_encode_string(p), enc);
    zell_slice b;
    zell_string_to_bytes(&b, p);
    EXPECT_STR(zell_encoding_base64_encode(&b), enc);
    zell_slice_release(&b);
    zell_error *e = NULL;
    EXPECT_STR(zell_encoding_base64_decode_string(en, &e), plain);
    must_ok(e);
    zell_encoding_base64_decode(en, &b, &e);
    must_ok(e);
    zell_string *back = zell_string_from_bytes(&b);
    assert(zell_string_eq(back, p));
    zell_release(back);
    zell_slice_release(&b);
    zell_release(p); zell_release(en);
}

static void b64_bad(const char *enc, bool url, const char *want_msg) {
    zell_string *en = S(enc);
    zell_error *e = NULL;
    zell_slice b;
    if (url) zell_encoding_base64_decode_url(en, &b, &e);
    else zell_encoding_base64_decode(en, &b, &e);
    if (!e) { fprintf(stderr, "base64 '%s' decoded unexpectedly\n", enc); assert(0); }
    assert(b.len == 0);
    zell_string *m = zell_error_message(e);
    if (want_msg && strcmp(zell_string_cstr(m), want_msg) != 0) {
        fprintf(stderr, "base64 '%s': %s\n", enc, zell_string_cstr(m));
        assert(0);
    }
    zell_release(m);
    zell_release(e);
    e = NULL;
    if (!url) {
        assert(zell_encoding_base64_decode_string(en, &e) == NULL && e != NULL);
        zell_release(e);
    }
    zell_release(en);
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
    EXPECT_STR(zell_encoding_base64_encode(NULL), "");

    zell_error *e = NULL;
    zell_string *nl = S("Zm9v\r\nYmFy\n");
    EXPECT_STR(zell_encoding_base64_decode_string(nl, &e), "foobar");
    must_ok(e);
    zell_release(nl);

    b64_bad("Zg=", false, "illegal base64 data at input byte 2");
    b64_bad("Zg", false, "illegal base64 data at input byte 2");
    b64_bad("Zm9v!", false, "illegal base64 data at input byte 4");
    b64_bad("Z===", false, NULL);
    b64_bad("Zg==Zg==", false, NULL);
    b64_bad("Z", false, NULL);
    b64_bad("Zm9-", false, NULL);   /* URL alphabet in std decoder */

    /* URL-safe: unpadded output, padding optional on input */
    uint8_t raw[] = { 0xfb, 0xff, 0xbf };
    zell_slice b, d;
    zell_slice_from(&b, 1, NULL, raw, 2);
    EXPECT_STR(zell_encoding_base64_encode(&b), "+/8=");
    EXPECT_STR(zell_encoding_base64_encode_url(&b), "-_8");
    zell_slice_release(&b);
    zell_slice_from(&b, 1, NULL, raw, 3);
    EXPECT_STR(zell_encoding_base64_encode_url(&b), "-_-_");
    zell_slice_release(&b);
    static const char *const ok_url[] = { "-_8", "-_8=" };
    for (int i = 0; i < 2; i++) {
        zell_string *s = S(ok_url[i]);
        zell_encoding_base64_decode_url(s, &d, &e);
        must_ok(e);
        assert(bytes_hex_is(&d, "fbff"));
        zell_slice_release(&d);
        zell_release(s);
    }
    b64_bad("+/8", true, NULL);
    b64_bad("-_8==", true, NULL);
    b64_bad("-_=", true, NULL);
    zell_encoding_base64_decode_url(NULL, &d, &e);
    must_ok(e);
    assert(d.len == 0);
    zell_slice_release(&d);

    /* all byte values round-trip */
    uint8_t all[256];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    for (int n = 0; n <= 256; n += 37) {
        zell_slice_from(&b, 1, NULL, all, n);
        zell_string *s1 = zell_encoding_base64_encode(&b), *s2 = zell_encoding_base64_encode_url(&b);
        zell_encoding_base64_decode(s1, &d, &e); must_ok(e);
        assert(d.len == n && memcmp(d.data, all, (size_t)n) == 0);
        zell_slice_release(&d);
        zell_encoding_base64_decode_url(s2, &d, &e); must_ok(e);
        assert(d.len == n && (n == 0 || memcmp(d.data, all, (size_t)n) == 0));
        zell_slice_release(&d);
        zell_release(s1); zell_release(s2);
        zell_slice_release(&b);
    }
}

TEST(hex) {
    STR1(zell_encoding_hex_encode_string, "Hello", "48656c6c6f");
    STR1(zell_encoding_hex_encode_string, "", "");
    EXPECT_STR(zell_encoding_hex_encode(NULL), "");
    zell_error *e = NULL;
    zell_string *s = S("48656C6c6f");
    EXPECT_STR(zell_encoding_hex_decode_string(s, &e), "Hello");
    must_ok(e);
    zell_slice b;
    zell_encoding_hex_decode(s, &b, &e);
    must_ok(e);
    assert(b.len == 5 && ((uint8_t *)b.data)[0] == 0x48);
    EXPECT_STR(zell_encoding_hex_encode(&b), "48656c6c6f");
    zell_slice_release(&b);
    zell_release(s);
    s = S("4G");
    zell_encoding_hex_decode(s, &b, &e);
    assert(e && b.len == 0);
    zell_string *m = zell_error_message(e);
    assert(STR_IS(m, "encoding/hex: invalid byte at offset 1"));
    zell_release(m); zell_release(e); e = NULL;
    zell_release(s);
    s = S("abc");
    assert(zell_encoding_hex_decode_string(s, &e) == NULL && e);
    m = zell_error_message(e);
    assert(STR_IS(m, "encoding/hex: odd length hex string"));
    zell_release(m); zell_release(e); e = NULL;
    zell_release(s);
    zell_encoding_hex_decode(NULL, &b, &e);
    must_ok(e);
    assert(b.len == 0);
    zell_slice_release(&b);
}

/* ---------------------------------------------------------------- uuid */

TEST(uuid) {
    for (int i = 0; i < 50; i++) {
        zell_string *u = zell_uuid_new();
        const char *p = zell_string_data(u);
        assert(zell_string_len(u) == 36 && zell_uuid_is_valid(u));
        assert(p[14] == '4' && strchr("89ab", p[19]) != NULL);
        for (int k = 0; k < 36; k++) assert(!(p[k] >= 'A' && p[k] <= 'F'));
        zell_string *v = zell_uuid_new();
        assert(!zell_string_eq(u, v));
        zell_release(u); zell_release(v);
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
        zell_string *s = S(good[i]); assert(zell_uuid_is_valid(s)); zell_release(s);
    }
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        zell_string *s = S(bad[i]); assert(!zell_uuid_is_valid(s)); zell_release(s);
    }
    assert(!zell_uuid_is_valid(NULL));
}

int main(int argc, char **argv) {
    (void)argc;
    zell_runtime_init(1, argv);
    printf("test_std2\n");
    /* os.Args is built on first use and kept by the runtime until shutdown */
    zell_slice args0;
    zell_os_args(&args0);
    zell_slice_release(&args0);
    int64_t live = zell_live_objects();
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
    if (zell_live_objects() != live) {
        fprintf(stderr, "leaked %lld objects\n", (long long)(zell_live_objects() - live));
        assert(0);
    }
    zell_runtime_shutdown();
    return 0;
}
