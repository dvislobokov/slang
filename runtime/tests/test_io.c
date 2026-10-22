#include "test.h"
#include <fcntl.h>

static void prints(void) {
    ZELL_STATIC_STRING(hi, "hi");
    zell_print_str(hi); zell_print_space(); zell_print_int(-42); zell_print_space();
    zell_print_float(2.5); zell_print_space(); zell_print_bool(true); zell_print_space();
    zell_print_rune(0x1F600); zell_print_newline();
    zell_println_str(NULL);
    zell_println_int(7);
    zell_println_uint(18446744073709551615ULL);
    zell_println_float(0.1);
    zell_println_bool(false);
    zell_println_cstr("end");
    zell_println_rune('z');
    zell_flush();
}

TEST(print) {
    char buf[4096];
    assert(run_child_fd(prints, 1, buf, sizeof buf) == 0);
    assert(strcmp(buf, "hi -42 2.5 true \xF0\x9F\x98\x80\n\n7\n18446744073709551615\n0.1\nfalse\nend\nz\n") == 0);
}

TEST(files) {
    char dir[] = "/tmp/zell_rt_test_XXXXXX";
    assert(mkdtemp(dir));
    char path[256];
    snprintf(path, sizeof path, "%s/out.txt", dir);
    zell_string *p = S(path);
    zell_string *data = zell_string_new("line1\nline2\0bin", 15);
    assert(zell_write_file(p, data) == NULL);
    zell_error *err = (zell_error *)1;
    zell_string *back = zell_read_file(p, &err);
    assert(err == NULL && zell_string_eq(back, data));
    zell_slice bytes;
    zell_string_to_bytes(&bytes, data);
    assert(zell_write_file_bytes(p, &bytes) == NULL);
    zell_slice_release(&bytes);
    zell_release(back);
    back = zell_read_file(p, &err);
    assert(zell_string_len(back) == 15);
    zell_release(back);
    /* large file exercises buffer growth */
    char *big = malloc(100000);
    for (int i = 0; i < 100000; i++) big[i] = (char)('a' + i % 26);
    zell_string *bigs = zell_string_new(big, 100000);
    free(big);
    assert(zell_write_file(p, bigs) == NULL);
    back = zell_read_file(p, &err);
    assert(zell_string_eq(back, bigs));
    zell_release(back); zell_release(bigs);
    unlink(path);
    rmdir(dir);
    zell_string *missing = S("/nonexistent/dir/file");
    back = zell_read_file(missing, &err);
    assert(back == NULL && err != NULL);
    zell_string *m = zell_error_message(err);
    assert(strstr(zell_string_data(m), "open /nonexistent/dir/file: No such file"));
    zell_release(m); zell_release(err);
    err = zell_write_file(missing, data);
    assert(err);
    zell_release(err);
    zell_release(missing); zell_release(p); zell_release(data);
}

TEST(read_line) {
    int fds[2];
    assert(pipe(fds) == 0);
    const char *in = "first\nsecond\r\n\nlast";
    assert(write(fds[1], in, strlen(in)) == (ssize_t)strlen(in));
    close(fds[1]);
    int saved = dup(0);
    dup2(fds[0], 0);
    close(fds[0]);
    const char *want[] = { "first", "second", "", "last" };
    zell_error *err;
    for (int i = 0; i < 4; i++) {
        zell_string *l = zell_read_line(&err);
        assert(err == NULL && STR_IS(l, want[i]));
        zell_release(l);
    }
    assert(zell_read_line(&err) == NULL && err == zell_err_eof);
    assert(zell_error_is(err, zell_err_eof));
    dup2(saved, 0);
    close(saved);
}

int main(void) {
    printf("test_io\n");
    int64_t live = zell_live_objects();
    RUN(print);
    RUN(files);
    RUN(read_line);
    assert(zell_live_objects() == live);
    return 0;
}
