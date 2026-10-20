/* Tests for the sharded I/O pollers (task_io.c), the platform poller
 * (one-shot level-triggered arming, wake-ups, EINTR), the socket handles
 * of std_net.c (close races, double close, ErrClosed, error sentinels),
 * addresses, Unix sockets, UDP, sendfile and the resolver pool. Built
 * with -fsanitize=thread by `make tsan`. */
#include "test.h"
#include "../src/platform/platform.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>

int64_t slang__io_shard_of(int64_t fd);
int64_t slang__io_shards(void);

static int64_t now_ms(void) { return slang__plat_mono_ns() / 1000000; }

static int64_t wait_code(slang_task *t) {
    int64_t r = *(int64_t *)slang_task_await(t);
    slang_release(t);
    return r;
}

static void pair(int sv[2]) { assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0); }

static bool msg_has(slang_error *e, const char *needle) {
    slang_string *m = slang_error_message(e);
    bool ok = strstr(slang_string_data(m), needle) != NULL;
    slang_release(m);
    return ok;
}

/* ---- the platform poller ---- */

TEST(poller_oneshot_level) {
    slang__plat_poller *p = slang__plat_poller_new();
    int sv[2];
    pair(sv);
    slang__plat_event ev[8];
    assert(write(sv[1], "ab", 2) == 2);
    /* data already there: reported at once (level-triggered) */
    assert(slang__plat_poller_arm(p, sv[0], SLANG_PLAT_READ, 5, false) == 0);
    assert(slang__plat_poller_wait(p, ev, 8, 1000) == 1 && ev[0].token == 5);
    /* one-shot: no second report until armed again, though data remains */
    assert(slang__plat_poller_wait(p, ev, 8, 20) == 0);
    assert(slang__plat_poller_arm(p, sv[0], SLANG_PLAT_READ | SLANG_PLAT_WRITE, 6, true) == 0);
    int n = slang__plat_poller_wait(p, ev, 8, 1000);
    assert(n == 1 && ev[0].token == 6 && (ev[0].events & SLANG_PLAT_READ) && (ev[0].events & SLANG_PLAT_WRITE));
    /* disarm keeps the registration */
    assert(slang__plat_poller_arm(p, sv[0], 0, 7, true) == 0);
    assert(slang__plat_poller_wait(p, ev, 8, 20) == 0);
    /* peer hang-up reports readability (end of stream) */
    close(sv[1]);
    assert(slang__plat_poller_arm(p, sv[0], SLANG_PLAT_READ, 8, true) == 0);
    assert(slang__plat_poller_wait(p, ev, 8, 1000) == 1 && (ev[0].events & SLANG_PLAT_READ));
    slang__plat_poller_remove(p, sv[0]);
    close(sv[0]);
    /* a regular file cannot be polled; a closed fd is EBADF */
    int f = open("/dev/null", O_RDONLY);
    int rc = slang__plat_poller_arm(p, f, SLANG_PLAT_READ, 9, false);
    assert(rc == 0 || rc == SLANG_PLAT_ENOTPOLLABLE);
    close(f);
    assert(slang__plat_poller_arm(p, 1000000, SLANG_PLAT_READ, 9, false) == EBADF);
    slang__plat_poller_free(p);
}

typedef struct waker_arg {
    slang__plat_poller *p;
    pthread_t target;
    int mode;  /* 0 = wake, 1 = signal (EINTR) */
} waker_arg;

static void on_usr1(int sig) { (void)sig; }

static void *waker(void *a) {
    waker_arg *w = a;
    struct timespec ts = { 0, 30 * 1000000 };
    nanosleep(&ts, NULL);
    if (w->mode == 0) slang__plat_poller_wake(w->p);
    else pthread_kill(w->target, SIGUSR1);
    return NULL;
}

TEST(poller_wake_and_eintr) {
    slang__plat_poller *p = slang__plat_poller_new();
    slang__plat_event ev[4];
    /* a wake-up from another thread ends a blocked wait */
    waker_arg w = { p, pthread_self(), 0 };
    pthread_t th;
    pthread_create(&th, NULL, waker, &w);
    int64_t t0 = now_ms();
    int n = slang__plat_poller_wait(p, ev, 4, 5000);
    assert(n == 1 && ev[0].token == SLANG_PLAT_WAKE_TOKEN && now_ms() - t0 < 4000);
    pthread_join(th, NULL);
    /* wake-ups coalesce and are drained */
    for (int i = 0; i < 100; i++) slang__plat_poller_wake(p);
    assert(slang__plat_poller_wait(p, ev, 4, 1000) == 1);
    assert(slang__plat_poller_wait(p, ev, 4, 10) == 0);
    /* a signal (EINTR) returns 0 events instead of failing */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;  /* no SA_RESTART */
    sigaction(SIGUSR1, &sa, NULL);
    w.mode = 1;
    pthread_create(&th, NULL, waker, &w);
    t0 = now_ms();
    n = slang__plat_poller_wait(p, ev, 4, 5000);
    assert(n == 0 && now_ms() - t0 < 4000);
    pthread_join(th, NULL);
    signal(SIGUSR1, SIG_DFL);
    slang__plat_poller_free(p);
}

/* ---- sharded waits ---- */

TEST(io_shards_balance) {
    enum { N = 64 };
    int sv[N][2];
    for (int i = 0; i < N; i++) {
        pair(sv[i]);
        slang_io_register(sv[i][0]);
    }
    int64_t ns = slang__io_shards();
    assert(ns >= 1);
    int64_t count[64] = { 0 };
    for (int i = 0; i < N; i++) {
        int64_t s = slang__io_shard_of(sv[i][0]);
        assert(s >= 0 && s < ns);
        count[s]++;
    }
    for (int64_t s = 0; s < ns; s++) assert(count[s] >= N / ns - 1 && count[s] <= N / ns + 2);
    for (int i = 0; i < N; i++) {
        slang_io_forget(sv[i][0]);
        assert(slang__io_shard_of(sv[i][0]) == -1);
        close(sv[i][0]);
        close(sv[i][1]);
    }
}

TEST(io_two_readers_and_timeouts) {
    int sv[2];
    pair(sv);
    slang_task *a = slang_io_wait_readable(sv[0], -1);
    slang_task *b = slang_io_wait_readable(sv[0], 5000);
    slang_task *c = slang_io_wait_readable(sv[0], 20);   /* times out first */
    assert(wait_code(c) == 1);
    assert(!slang_task_is_done(a) && !slang_task_is_done(b));
    assert(write(sv[1], "x", 1) == 1);
    assert(wait_code(a) == 0 && wait_code(b) == 0);      /* every waiter is woken */
    /* deadlines in any order complete in deadline order */
    char buf[4];
    assert(read(sv[0], buf, 4) == 1);
    slang_task *t[5];
    int64_t ms[5] = { 80, 20, 60, 40, 10 };
    for (int i = 0; i < 5; i++) t[i] = slang_io_wait_readable(sv[0], ms[i]);
    int64_t t0 = now_ms();
    for (int i = 0; i < 5; i++) {
        assert(wait_code(t[i]) == 1);
        assert(now_ms() - t0 >= ms[i] - 1);
    }
    slang_io_forget(sv[0]);
    close(sv[0]);
    close(sv[1]);
}

typedef struct stress_arg {
    int id;
    int rounds;
    int64_t done;
} stress_arg;

/* Each thread: socketpairs whose waits are completed by writes from the
 * peer thread, by forget/close racing with new waits, and by timeouts. */
static void *stress_thread(void *a) {
    stress_arg *s = a;
    for (int r = 0; r < s->rounds; r++) {
        int sv[2];
        pair(sv);
        slang_task *rd = slang_io_wait_readable(sv[0], 2000);
        slang_task *wr = slang_io_wait_writable(sv[1], 2000);
        assert(wait_code(wr) == 0);
        assert(write(sv[1], "z", 1) == 1);
        assert(wait_code(rd) == 0);
        /* close race: a wait on a descriptor forgotten right away */
        slang_task *late = slang_io_wait_readable(sv[1], 2000);
        slang_io_forget(sv[1]);
        int64_t code = wait_code(late);
        assert(code == 2 || code == 0);
        close(sv[1]);
        slang_io_forget(sv[0]);
        close(sv[0]);
        s->done++;
    }
    return NULL;
}

TEST(io_multithread_stress) {
    enum { T = 8 };
    pthread_t th[T];
    stress_arg args[T];
    for (int i = 0; i < T; i++) {
        args[i] = (stress_arg){ i, 300, 0 };
        pthread_create(&th[i], NULL, stress_thread, &args[i]);
    }
    for (int i = 0; i < T; i++) {
        pthread_join(th[i], NULL);
        assert(args[i].done == 300);
    }
}

/* Tasks on worker threads wake each other through the pollers. */
static void ping_worker(slang_task *t, void *ctx) {
    int *sv = ctx;
    for (int i = 0; i < 200; i++) {
        assert(write(sv[1], "p", 1) == 1);
        char c;
        while (read(sv[1], &c, 1) != 1) assert(wait_code(slang_io_wait_readable(sv[1], 5000)) == 0);
    }
    slang_task_complete(t, NULL);
}

static void pong_worker(slang_task *t, void *ctx) {
    int *sv = ctx;
    for (int i = 0; i < 200; i++) {
        char c;
        while (read(sv[0], &c, 1) != 1) assert(wait_code(slang_io_wait_readable(sv[0], 5000)) == 0);
        assert(write(sv[0], "q", 1) == 1);
    }
    slang_task_complete(t, NULL);
}

TEST(io_worker_ping_pong) {
    int sv[2];
    pair(sv);
    slang_task *a = slang_task_spawn(ping_worker, sv);
    slang_task *b = slang_task_spawn(pong_worker, sv);
    slang_task_await(a);
    slang_task_await(b);
    slang_release(a);
    slang_release(b);
    slang_io_forget(sv[0]);
    slang_io_forget(sv[1]);
    close(sv[0]);
    close(sv[1]);
}

TEST(io_cancel_wakes_wait) {
    int sv[2];
    pair(sv);
    slang_task *scope = slang_task_new_scope();
    slang_task *prev = slang_task_enter_scope(scope);
    slang_task *w = slang_io_wait_readable(sv[0], -1);
    slang_task_leave_scope(prev);
    assert(!slang_task_is_done(w));
    slang_task_cancel(scope);
    assert(wait_code(w) == 3);
    /* a wait started under a cancelled task completes at once */
    prev = slang_task_enter_scope(scope);
    w = slang_io_wait_readable(sv[0], -1);
    slang_task_leave_scope(prev);
    assert(wait_code(w) == 3);
    slang_release(scope);
    slang_io_forget(sv[0]);
    close(sv[0]);
    close(sv[1]);
}

static void pending_at_shutdown(void) {
    int64_t live = slang_live_objects();
    slang_runtime_init(0, NULL);
    int sv[2];
    pair(sv);
    slang_task *w = slang_io_wait_readable(sv[0], -1);
    slang_task *w2 = slang_io_wait_readable(sv[0], 60000);
    slang_release(w);
    slang_release(w2);
    slang_runtime_shutdown();  /* must not hang; the waits are dropped */
    if (slang_live_objects() != live) _exit(3);
    _exit(0);
}

TEST(io_shutdown_with_pending_waits) {
    char buf[256];
    assert(run_child(pending_at_shutdown, buf, sizeof buf) == 0);
}

/* ---- socket handles ---- */

static int64_t listen_lo(void) {
    slang_error *err = NULL;
    slang_string *lo = S("127.0.0.1");
    int64_t l = slang_net_rt_listen(lo, 0, 0, 0, &err);
    assert(!err && l != -1);
    slang_release(lo);
    return l;
}

static int64_t dial_lo(int64_t port) {
    slang_error *err = NULL;
    slang_string *lo = S("127.0.0.1"), *none = S("");
    int64_t c = slang_net_rt_dial(lo, port, none, 0, &err);
    assert(!err && c != -1);
    assert(wait_code(slang_net_rt_wait_writable(c, 2000)) == 0);
    slang_net_rt_connect_result(c, &err);
    assert(!err);
    slang_release(lo);
    slang_release(none);
    return c;
}

static int64_t accept_one(int64_t l) {
    slang_error *err = NULL;
    int64_t a;
    while ((a = slang_net_rt_accept(l, &err)) == -1) {
        assert(!err);
        assert(wait_code(slang_net_rt_wait_readable(l, 2000)) == 0);
    }
    return a;
}

TEST(net_round_trip_and_close) {
    slang_error *err = NULL;
    int64_t l = listen_lo();
    int64_t port = slang_net_rt_port(l);
    assert(port > 0);
    int64_t c = dial_lo(port);
    int64_t a = accept_one(l);
    slang_string *ra = slang_net_rt_remote_addr(a);
    assert(strncmp(slang_string_data(ra), "127.0.0.1:", 10) == 0);
    slang_release(ra);
    slang_string *msg = S("ping");
    assert(slang_net_rt_write_str(c, msg, 0, &err) == 4 && !err);
    slang_slice buf;
    slang_slice_new(&buf, 1, 16, 16);
    int64_t k;
    while ((k = slang_net_rt_read_into(a, &buf, 0, &err)) == -1)
        assert(wait_code(slang_net_rt_wait_readable(a, 2000)) == 0);
    assert(!err && k == 4 && memcmp(buf.data, "ping", 4) == 0);
    /* a reader waiting when the socket is closed is woken with 2 */
    slang_task *w = slang_net_rt_wait_readable(a, -1);
    slang_net_rt_close(a);
    slang_net_rt_close(a);  /* double close: no-op */
    assert(wait_code(w) == 2);
    assert(slang_net_rt_is_closed(a) && slang_net_rt_fd(a) == -1);
    /* every operation on a closed handle fails with ErrClosed */
    assert(slang_net_rt_read_into(a, &buf, 0, &err) == -1 && err == slang_net_err_closed());
    err = NULL;
    assert(slang_net_rt_write_str(a, msg, 0, &err) == -1 && err == slang_net_err_closed());
    err = NULL;
    assert(wait_code(slang_net_rt_wait_writable(a, -1)) == 2);
    /* the peer address survives Close */
    ra = slang_net_rt_remote_addr(a);
    assert(strncmp(slang_string_data(ra), "127.0.0.1:", 10) == 0);
    slang_release(ra);
    /* the other end sees end of stream, then a write fails with ErrReset */
    while ((k = slang_net_rt_read_into(c, &buf, 0, &err)) == -1)
        assert(wait_code(slang_net_rt_wait_readable(c, 2000)) == 0);
    assert(k == 0 && !err);
    for (int i = 0; i < 100 && !err; i++) {
        slang_net_rt_write_str(c, msg, 0, &err);
        if (!err) { struct timespec ts = { 0, 2000000 }; nanosleep(&ts, NULL); }
    }
    assert(err && slang_error_is(err, slang_net_err_reset()));
    slang_release(err);
    err = NULL;
    slang_net_rt_free(a);
    slang_net_rt_free(c);
    /* refused: the sentinel */
    slang_net_rt_close(l);
    slang_string *lo = S("127.0.0.1"), *none = S("");
    int64_t c2 = slang_net_rt_dial(lo, port, none, 0, &err);
    if (!err) {
        assert(wait_code(slang_net_rt_wait_writable(c2, 2000)) == 0);
        slang_net_rt_connect_result(c2, &err);
        slang_net_rt_free(c2);
    }
    assert(err == slang_net_err_refused());
    err = NULL;
    /* accept on a closed listener */
    assert(slang_net_rt_accept(l, &err) == -1 && err == slang_net_err_closed());
    err = NULL;
    slang_net_rt_free(l);
    slang_slice_release(&buf);
    slang_release(msg);
    slang_release(lo);
    slang_release(none);
}

typedef struct race_arg {
    int64_t h;
    int64_t ops;  /* atomic */
} race_arg;

static void *hammer(void *p) {
    race_arg *r = p;
    slang_slice buf;
    slang_slice_new(&buf, 1, 8, 8);
    for (;;) {
        slang_error *err = NULL;
        slang_net_rt_read_into(r->h, &buf, 0, &err);
        slang_release(err);
        slang_task *w = slang_net_rt_wait_readable(r->h, 1);
        int64_t code = wait_code(w);
        assert(code <= 2);
        __atomic_add_fetch(&r->ops, 1, __ATOMIC_RELAXED);
        if (slang_net_rt_is_closed(r->h)) break;
    }
    slang_slice_release(&buf);
    return NULL;
}

/* Close while other threads read and wait on the same handle: no wait is
 * lost, the descriptor is closed exactly once, after the last operation. */
TEST(net_close_race) {
    for (int round = 0; round < 50; round++) {
        int64_t l = listen_lo();
        int64_t c = dial_lo(slang_net_rt_port(l));
        int64_t a = accept_one(l);
        race_arg r = { a, 0 };
        pthread_t th[4];
        for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, hammer, &r);
        struct timespec ts = { 0, 200000 };
        nanosleep(&ts, NULL);
        slang_net_rt_close(a);
        for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
        slang_net_rt_free(a);
        slang_net_rt_free(c);
        slang_net_rt_free(l);
    }
}

TEST(net_unix_udp_sendfile) {
    slang_error *err = NULL;
    char path[64];
    snprintf(path, sizeof path, "/tmp/slang-test-%d.sock", (int)getpid());
    slang_string *ps = S(path);
    int64_t l = slang_net_rt_listen_unix(ps, 0, &err);
    assert(!err && l != -1);
    assert(access(path, F_OK) == 0);
    int64_t c = slang_net_rt_dial_unix(ps, &err);
    assert(!err && c != -1);
    int64_t a = accept_one(l);
    /* sendfile from a temp file */
    char fpath[64];
    snprintf(fpath, sizeof fpath, "/tmp/slang-test-%d.dat", (int)getpid());
    FILE *f = fopen(fpath, "w");
    for (int i = 0; i < 10000; i++) fputc('a' + i % 26, f);
    fclose(f);
    slang_string *fp = S(fpath);
    int64_t ffd = slang_net_rt_open_file(fp, &err);
    assert(!err && slang_net_rt_file_size(ffd) == 10000);
    int64_t off = 0;
    while (off < 10000) {
        int64_t k = slang_net_rt_send_file(c, ffd, off, 10000 - off, &err);
        assert(!err);
        if (k < 0) assert(wait_code(slang_net_rt_wait_writable(c, 2000)) == 0);
        else off += k;
    }
    slang_net_rt_close_file(ffd);
    unlink(fpath);
    slang_slice buf;
    slang_slice_new(&buf, 1, 20000, 20000);
    int64_t got = 0;
    while (got < 10000) {
        int64_t k = slang_net_rt_read_into(a, &buf, got, &err);
        assert(!err && k != 0);
        if (k < 0) assert(wait_code(slang_net_rt_wait_readable(a, 2000)) == 0);
        else got += k;
    }
    assert(((char *)buf.data)[9999] == 'a' + 9999 % 26);
    slang_net_rt_free(a);
    slang_net_rt_free(c);
    slang_net_rt_close(l);
    assert(access(path, F_OK) != 0);  /* the listener removed its file */
    slang_net_rt_free(l);
    slang_release(ps);
    slang_release(fp);
    /* UDP: truncation is reported */
    slang_string *lo = S("127.0.0.1");
    int64_t u1 = slang_net_rt_udp(lo, 0, 0, &err), u2 = slang_net_rt_udp(lo, 0, 0, &err);
    assert(!err);
    slang_slice data;
    slang_slice_new(&data, 1, 100, 100);
    memset(data.data, 'u', 100);
    assert(slang_net_rt_send_to(u1, &data, lo, slang_net_rt_port(u2), &err) == 100);
    slang_slice small;
    slang_slice_new(&small, 1, 10, 10);
    int64_t k;
    while ((k = slang_net_rt_read_from(u2, &small, &err)) == -1)
        assert(wait_code(slang_net_rt_wait_readable(u2, 2000)) == 0);
    assert(k == 10 && slang_net_rt_last_trunc());
    assert(slang_net_rt_last_from_port() == slang_net_rt_port(u1));
    slang_net_rt_free(u1);
    slang_net_rt_free(u2);
    slang_slice_release(&data);
    slang_slice_release(&small);
    slang_slice_release(&buf);
    slang_release(lo);
}

TEST(net_addresses_and_options) {
    slang_string *s = S("2001:db8::1");
    slang_slice b;
    slang_net_rt_parse_ip(s, &b);
    assert(b.len == 16);
    slang_string *t = slang_net_rt_format_ip(&b);
    assert(STR_IS(t, "2001:db8::1"));
    slang_release(t);
    slang_slice_release(&b);
    slang_release(s);
    s = S("300.1.1.1");
    slang_net_rt_parse_ip(s, &b);
    assert(b.len == 0);
    slang_release(s);
    unsigned char ip4[4] = { 10, 0, 0, 1 };
    slang__plat_addr a;
    assert(slang__plat_addr_ip(&a, ip4, 4, 8080, 0));
    char buf[128];
    slang__plat_addr_format(&a, buf, sizeof buf);
    assert(strcmp(buf, "10.0.0.1:8080") == 0);
    unsigned char ip6[16] = { 0xfe, 0x80, [15] = 1 };
    assert(slang__plat_addr_ip(&a, ip6, 16, 80, 1));
    slang__plat_addr_format(&a, buf, sizeof buf);
    assert(strncmp(buf, "[fe80::1%", 9) == 0);
    assert(slang__plat_addr_unix(&a, "/tmp/x.sock"));
    slang__plat_addr_format(&a, buf, sizeof buf);
    assert(strcmp(buf, "/tmp/x.sock") == 0);
#if defined(__linux__)
    assert(slang__plat_addr_unix(&a, "@abstract"));
    slang__plat_addr_format(&a, buf, sizeof buf);
    assert(strcmp(buf, "@abstract") == 0);
#endif
    assert(slang__plat_err_class(ECONNREFUSED) == SLANG_PLAT_E_REFUSED);
    assert(slang__plat_err_class(EPIPE) == SLANG_PLAT_E_RESET);
    char eb[64];
    assert(strcmp(slang__plat_strerror(ECONNREFUSED, eb, sizeof eb), "connection refused") == 0);
    /* options round-trip */
    int64_t l = listen_lo();
    int64_t c = dial_lo(slang_net_rt_port(l));
    slang_error *err = NULL;
    slang_net_rt_set_opt(c, SLANG_PLAT_OPT_NODELAY, 0, &err);
    assert(!err && slang_net_rt_get_opt(c, SLANG_PLAT_OPT_NODELAY, &err) == 0);
    slang_net_rt_set_opt(c, SLANG_PLAT_OPT_KEEPALIVE, 1, &err);
    assert(!err && slang_net_rt_get_opt(c, SLANG_PLAT_OPT_KEEPALIVE, &err) != 0);
    slang_net_rt_set_opt(c, SLANG_PLAT_OPT_RCVBUF, 65536, &err);
    assert(!err && slang_net_rt_get_opt(c, SLANG_PLAT_OPT_RCVBUF, &err) >= 65536);
    slang_net_rt_set_opt(c, SLANG_PLAT_OPT_LINGER, 0, &err);
    assert(!err && slang_net_rt_get_opt(c, SLANG_PLAT_OPT_LINGER, &err) == 0);
    slang_net_rt_free(c);
    slang_net_rt_free(l);
    /* address in use */
    slang_string *lo = S("127.0.0.1");
    int64_t l1 = slang_net_rt_listen(lo, 0, 0, 1 /* no SO_REUSEADDR */, &err);
    assert(!err);
    int64_t l2 = slang_net_rt_listen(lo, slang_net_rt_port(l1), 0, 1, &err);
    assert(l2 == -1 && err == slang_net_err_addr_in_use());
    err = NULL;
    /* SO_REUSEPORT: two listeners share the port */
    int64_t r1 = slang_net_rt_listen(lo, 0, 0, 2, &err);
    assert(!err);
    int64_t r2 = slang_net_rt_listen(lo, slang_net_rt_port(r1), 0, 2, &err);
    assert(!err && r2 != -1);
    slang_net_rt_free(r1);
    slang_net_rt_free(r2);
    slang_net_rt_free(l1);
    slang_release(lo);
}

/* ---- the resolver pool ---- */

TEST(resolver_pool) {
    enum { N = 32 };
    slang_string *h = S("localhost");
    slang_task *ts[N];
    for (int i = 0; i < N; i++) ts[i] = slang_net_rt_resolve(h, 1);
    for (int i = 0; i < N; i++) {
        int64_t hnd = *(int64_t *)slang_task_await(ts[i]);
        slang_release(ts[i]);
        slang_slice out;
        slang_error *err = NULL;
        slang_net_rt_resolve_take(hnd, &out, &err);
        assert(!err && out.len >= 1);
        assert(STR_IS(*(slang_string **)slang_slice_index(&out, 0, 8), "127.0.0.1"));
        slang_slice_release(&out);
    }
    slang_release(h);
    /* a name that does not exist wraps ErrNotFound */
    h = S("nosuchhost.invalid");
    slang_task *t = slang_net_rt_resolve(h, 0);
    int64_t hnd = *(int64_t *)slang_task_await(t);
    slang_release(t);
    slang_slice out;
    slang_error *err = NULL;
    slang_net_rt_resolve_take(hnd, &out, &err);
    assert(err && msg_has(err, "lookup nosuchhost.invalid"));
    assert(slang_error_is(err, slang_net_err_not_found()));
    slang_release(err);
    slang_release(h);
    /* literals complete at once, zones included */
    h = S("fe80::1%1");
    t = slang_net_rt_resolve(h, 0);
    assert(slang_task_is_done(t));
    hnd = *(int64_t *)slang_task_result(t);
    slang_release(t);
    err = NULL;
    slang_net_rt_resolve_take(hnd, &out, &err);
    assert(!err && out.len == 1);
    slang_slice_release(&out);
    slang_release(h);
    h = S("http");
    slang_string *tcp = S("tcp");
    int64_t port = slang_net_rt_lookup_port(tcp, h);
    assert(port == 80 || port == -1);  /* -1 without /etc/services */
    slang_release(h);
    slang_release(tcp);
}

int main(int argc, char **argv) {
    printf("test_net\n");
    setenv("SLANG_WORKERS", "4", 1);
    setenv("SLANG_IO_POLLERS", "3", 1);
    int64_t live = slang_live_objects();
    RUN(poller_oneshot_level);
    RUN(poller_wake_and_eintr);
    RUN(io_shutdown_with_pending_waits);
    slang_runtime_init(argc, argv);
    RUN(io_shards_balance);
    RUN(io_two_readers_and_timeouts);
    RUN(io_multithread_stress);
    RUN(io_worker_ping_pong);
    RUN(io_cancel_wakes_wait);
    RUN(net_round_trip_and_close);
    RUN(net_close_race);
    RUN(net_unix_udp_sendfile);
    RUN(net_addresses_and_options);
    RUN(resolver_pool);
    slang_runtime_shutdown();
    assert(slang_live_objects() == live);
    return 0;
}
