#include "test.h"

static void p_msg(void) { slang_panic("something broke"); }
static void p_nil(void) { slang_panic_nil(); }
static void p_bounds(void) { slang_panic_bounds(5, 3); }
static void p_err(void) {
    slang_string *c = S("ctx");
    slang_error *e = slang_error_wrap(slang_error_new_cstr("inner"), c);
    slang_panic_error(e);
}
static void p_str(void) { slang_panic_str(S("from string")); }
static void p_stdout_flushed(void) {
    slang_print_cstr("before panic");
    slang_panic("x");
}
static void p_exit(void) { slang_print_cstr("bye"); slang_exit(7); }

TEST(messages) {
    char buf[16384];
    assert(run_child(p_msg, buf, sizeof buf) == 2);
    assert(strncmp(buf, "panic: something broke\n", 23) == 0);
    assert(strstr(buf, "backtrace"));
    assert(run_child(p_nil, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: runtime error: invalid memory address or nil pointer dereference\n"));
    assert(run_child(p_bounds, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: runtime error: index out of range [5] with length 3\n"));
    assert(run_child(p_err, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: ctx: inner\n"));
    assert(run_child(p_str, buf, sizeof buf) == 2);
    assert(strstr(buf, "panic: from string\n"));
}

TEST(flush_and_exit) {
    char buf[4096];
    assert(run_child_fd(p_stdout_flushed, 1, buf, sizeof buf) == 2);
    assert(strcmp(buf, "before panic") == 0);
    assert(run_child_fd(p_exit, 1, buf, sizeof buf) == 7);
    assert(strcmp(buf, "bye") == 0);
}

int main(void) {
    printf("test_panic\n");
    RUN(messages);
    RUN(flush_and_exit);
    return 0;
}
