/* Minimal assert-based test helpers. */
#ifndef SLANG_TEST_H
#define SLANG_TEST_H
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include "slang_rt.h"

#define S(lit) slang_string_new((lit), (int64_t)strlen(lit))
#define STR_IS(s, lit) (strcmp(slang_string_data(s), (lit)) == 0 && slang_string_len(s) == (int64_t)strlen(lit))

#define TEST(name) static void name(void)
#define RUN(name) do { name(); printf("  ok  %s\n", #name); } while (0)

/* Run fn in a child process; capture its fd (1 or 2); return exit status. */
static inline int run_child_fd(void (*fn)(void), int fd, char *errbuf, size_t cap) {
    int fds[2];
    assert(pipe(fds) == 0);
    fflush(stdout);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], fd);
        if (fd != 2) { int nul = open("/dev/null", O_WRONLY); dup2(nul, 2); }
        fn();
        _exit(0);
    }
    close(fds[1]);
    size_t len = 0;
    ssize_t n;
    while ((n = read(fds[0], errbuf + len, cap - 1 - len)) > 0) len += (size_t)n;
    errbuf[len] = 0;
    close(fds[0]);
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static inline int run_child(void (*fn)(void), char *errbuf, size_t cap) {
    return run_child_fd(fn, 2, errbuf, cap);
}

#endif
