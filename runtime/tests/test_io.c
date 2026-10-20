#include "test.h"
#include <fcntl.h>

static void prints(void) {
    SLANG_STATIC_STRING(hi, "hi");
    slang_print_str(hi); slang_print_space(); slang_print_int(-42); slang_print_space();
    slang_print_float(2.5); slang_print_space(); slang_print_bool(true); slang_print_space();
    slang_print_rune(0x1F600); slang_print_newline();
    slang_println_str(NULL);
    slang_println_int(7);
    slang_println_uint(18446744073709551615ULL);
    slang_println_float(0.1);
    slang_println_bool(false);
    slang_println_cstr("end");
    slang_println_rune('z');
    slang_flush();
}

TEST(print) {
    char buf[4096];
    assert(run_child_fd(prints, 1, buf, sizeof buf) == 0);
    assert(strcmp(buf, "hi -42 2.5 true \xF0\x9F\x98\x80\n\n7\n18446744073709551615\n0.1\nfalse\nend\nz\n") == 0);
}

TEST(files) {
    char dir[] = "/tmp/slang_rt_test_XXXXXX";
    assert(mkdtemp(dir));
    char path[256];
    snprintf(path, sizeof path, "%s/out.txt", dir);
    slang_string *p = S(path);
    slang_string *data = slang_string_new("line1\nline2\0bin", 15);
    assert(slang_write_file(p, data) == NULL);
    slang_error *err = (slang_error *)1;
    slang_string *back = slang_read_file(p, &err);
    assert(err == NULL && slang_string_eq(back, data));
    slang_slice bytes;
    slang_string_to_bytes(&bytes, data);
    assert(slang_write_file_bytes(p, &bytes) == NULL);
    slang_slice_release(&bytes);
    slang_release(back);
    back = slang_read_file(p, &err);
    assert(slang_string_len(back) == 15);
    slang_release(back);
    /* large file exercises buffer growth */
    char *big = malloc(100000);
    for (int i = 0; i < 100000; i++) big[i] = (char)('a' + i % 26);
    slang_string *bigs = slang_string_new(big, 100000);
    free(big);
    assert(slang_write_file(p, bigs) == NULL);
    back = slang_read_file(p, &err);
    assert(slang_string_eq(back, bigs));
    slang_release(back); slang_release(bigs);
    unlink(path);
    rmdir(dir);
    slang_string *missing = S("/nonexistent/dir/file");
    back = slang_read_file(missing, &err);
    assert(back == NULL && err != NULL);
    slang_string *m = slang_error_message(err);
    assert(strstr(slang_string_data(m), "open /nonexistent/dir/file: No such file"));
    slang_release(m); slang_release(err);
    err = slang_write_file(missing, data);
    assert(err);
    slang_release(err);
    slang_release(missing); slang_release(p); slang_release(data);
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
    slang_error *err;
    for (int i = 0; i < 4; i++) {
        slang_string *l = slang_read_line(&err);
        assert(err == NULL && STR_IS(l, want[i]));
        slang_release(l);
    }
    assert(slang_read_line(&err) == NULL && err == slang_err_eof);
    assert(slang_error_is(err, slang_err_eof));
    dup2(saved, 0);
    close(saved);
}

int main(void) {
    printf("test_io\n");
    int64_t live = slang_live_objects();
    RUN(print);
    RUN(files);
    RUN(read_line);
    assert(slang_live_objects() == live);
    return 0;
}
