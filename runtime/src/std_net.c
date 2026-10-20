/* std_net.c - runtime side of std.net (slang_net_*): thin, platform-
 * independent wrappers over the socket layer (platform/net_posix.c) and the
 * readiness waits (task_io.c), called by the Slang-source std.net
 * (compiler/src/sema/stdlib_src/net.sl). Every socket is non-blocking:
 * operations that would block return -1 (accept/write/sendto) or an empty
 * slice with slang_net_rt_again() == true (read/recvfrom), and the Slang
 * code then awaits slang_net_rt_wait_readable/writable. See ABI.md 14.4. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Whether the last read/recvfrom on this thread would have blocked, and the
 * sender of the last datagram. Read right after the call (no await between). */
static _Thread_local bool t_again;
static _Thread_local char t_from[64];
static _Thread_local int64_t t_from_port;

static void set_err(slang_error **err, const char *op, const char *what, int64_t code) {
    char eb[128], msg[512];
    const char *reason = slang__plat_strerror((int)(code < 0 ? -code : code), eb, sizeof eb);
    if (what && *what) snprintf(msg, sizeof msg, "%s %s: %s", op, what, reason);
    else snprintf(msg, sizeof msg, "%s: %s", op, reason);
    if (err) *err = slang_error_new_cstr(msg);
}

static void addr_of(char *buf, size_t cap, const slang_string *ip, int64_t port) {
    const char *s = slang_string_data(ip);
    if (strchr(s, ':')) snprintf(buf, cap, "[%s]:%lld", s, (long long)port);
    else snprintf(buf, cap, "%s:%lld", s, (long long)port);
}

int64_t slang_net_rt_connect(const slang_string *ip, int64_t port, slang_error **err) {
    int64_t fd = slang__plat_tcp_connect(slang_string_data(ip), port);
    if (fd < 0) {
        char a[96];
        addr_of(a, sizeof a, ip, port);
        set_err(err, "dial tcp", a, fd);
        return -1;
    }
    return fd;
}

void slang_net_rt_connect_result(int64_t fd, slang_error **err) {
    int e = slang__plat_tcp_connect_result(fd);
    if (e) set_err(err, "connect", "", e);
}

int64_t slang_net_rt_listen(const slang_string *ip, int64_t port, slang_error **err) {
    int64_t fd = slang__plat_tcp_listen(slang_string_data(ip), port, 128);
    if (fd < 0) {
        char a[96];
        addr_of(a, sizeof a, ip, port);
        set_err(err, "listen tcp", a, fd);
        return -1;
    }
    return fd;
}

int64_t slang_net_rt_accept(int64_t fd, slang_error **err) {
    int64_t c = slang__plat_tcp_accept(fd);
    if (c == -SLANG_PLAT_EAGAIN) return -1;
    if (c < 0) {
        set_err(err, "accept", "", c);
        return -1;
    }
    return c;
}

void slang_net_rt_read(int64_t fd, int64_t n, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    t_again = false;
    if (n <= 0) return;
    slang_slice buf;
    slang_slice_new(&buf, 1, n, n);
    int64_t r = slang__plat_sock_recv(fd, buf.data, n);
    if (r <= 0) {
        slang_slice_release(&buf);
        if (r == -SLANG_PLAT_EAGAIN) t_again = true;
        else if (r < 0) set_err(err, "read", "", r);
        return;
    }
    buf.len = r;
    *out = buf;
}

bool slang_net_rt_again(void) { return t_again; }

static int64_t write_bytes(int64_t fd, const char *p, int64_t len, int64_t off, slang_error **err) {
    if (off < 0 || off > len) slang_panic("net write: offset out of range");
    if (off == len) return 0;
    int64_t r = slang__plat_sock_send(fd, p + off, len - off);
    if (r == -SLANG_PLAT_EAGAIN) return -1;
    if (r < 0) {
        set_err(err, "write", "", r);
        return -1;
    }
    return r;
}

int64_t slang_net_rt_write(int64_t fd, const slang_slice *data, int64_t off, slang_error **err) {
    return write_bytes(fd, data->data, data->len, off, err);
}

int64_t slang_net_rt_write_str(int64_t fd, const slang_string *s, int64_t off, slang_error **err) {
    return write_bytes(fd, slang_string_data(s), slang_string_len(s), off, err);
}

void slang_net_rt_close(int64_t fd) {
    if (fd < 0) return;
    slang_io_forget(fd);
    slang__plat_sock_close(fd);
}

slang_task *slang_net_rt_wait_readable(int64_t fd, int64_t ms) { return slang_io_wait_readable(fd, ms > 0 ? ms : -1); }
slang_task *slang_net_rt_wait_writable(int64_t fd, int64_t ms) { return slang_io_wait_writable(fd, ms > 0 ? ms : -1); }

slang_string *slang_net_rt_local_addr(int64_t fd) {
    char b[96];
    return slang__plat_sock_addr(fd, false, b, sizeof b) ? slang_string_from_cstr(b) : NULL;
}

slang_string *slang_net_rt_remote_addr(int64_t fd) {
    char b[96];
    return slang__plat_sock_addr(fd, true, b, sizeof b) ? slang_string_from_cstr(b) : NULL;
}

int64_t slang_net_rt_port(int64_t fd) {
    int64_t p = slang__plat_sock_port(fd);
    return p < 0 ? 0 : p;
}

bool slang_net_rt_is_ip(const slang_string *s) { return slang__plat_is_ip(slang_string_data(s)); }

/* ---- name resolution: getaddrinfo on a helper thread ---- */

typedef struct resolve_req {
    char *host;
    slang_task *task;     /* owned until completed */
    char **addrs;
    int n;
    const char *errmsg;
} resolve_req;

static void *resolve_main(void *arg) {
    resolve_req *r = arg;
    r->errmsg = slang__plat_resolve(r->host, &r->addrs, &r->n);
    int64_t h = (int64_t)(intptr_t)r;
    slang_task *t = r->task;
    r->task = NULL;
    slang_task_complete(t, &h);
    slang_release(t);
    return NULL;
}

slang_task *slang_net_rt_resolve(const slang_string *host) {
    slang_task *t = slang_task_new_plain(8, NULL);
    resolve_req *r = calloc(1, sizeof *r);
    if (!r) slang_panic("out of memory (resolve)");
    r->host = strdup(slang_string_data(host));
    if (!r->host) slang_panic("out of memory (resolve)");
    if (slang__plat_is_ip(r->host)) {
        r->addrs = calloc(2, sizeof *r->addrs);
        r->addrs[0] = strdup(r->host);
        r->n = 1;
        int64_t h = (int64_t)(intptr_t)r;
        slang_task_complete(t, &h);
        return t;
    }
    slang_retain(t);
    r->task = t;
    if (!slang__plat_thread_start_detached(resolve_main, r)) {
        r->errmsg = "cannot start the resolver thread";
        r->task = NULL;
        int64_t h = (int64_t)(intptr_t)r;
        slang_task_complete(t, &h);
        slang_release(t);
    }
    return t;
}

void slang_net_rt_resolve_take(int64_t handle, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    resolve_req *r = (resolve_req *)(intptr_t)handle;
    if (!r) slang_panic_nil();
    if (r->errmsg) {
        char msg[512];
        snprintf(msg, sizeof msg, "lookup %s: %s", r->host, r->errmsg);
        *err = slang_error_new_cstr(msg);
    } else {
        slang_slice res;
        slang_slice_new_ops(&res, 8, 0, r->n, &slang_ops_ref);
        for (int i = 0; i < r->n; i++) {
            slang_string *s = slang_string_from_cstr(r->addrs[i]);
            slang_slice_append(&res, 8, &slang_ops_ref, &s);
            slang_release(s);
        }
        *out = res;
    }
    for (int i = 0; i < r->n; i++) free(r->addrs[i]);
    free(r->addrs);
    free(r->host);
    free(r);
}

/* ---- UDP ---- */

int64_t slang_net_rt_udp_bind(const slang_string *ip, int64_t port, slang_error **err) {
    int64_t fd = slang__plat_udp_bind(slang_string_data(ip), port);
    if (fd < 0) {
        char a[96];
        addr_of(a, sizeof a, ip, port);
        set_err(err, "listen udp", a, fd);
        return -1;
    }
    return fd;
}

void slang_net_rt_recv_from(int64_t fd, int64_t n, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    t_again = false;
    t_from[0] = 0;
    t_from_port = 0;
    if (n <= 0) return;
    slang_slice buf;
    slang_slice_new(&buf, 1, n, n);
    int64_t r = slang__plat_udp_recvfrom(fd, buf.data, n, t_from, sizeof t_from, &t_from_port);
    if (r < 0) {
        slang_slice_release(&buf);
        if (r == -SLANG_PLAT_EAGAIN) t_again = true;
        else set_err(err, "recvfrom", "", r);
        return;
    }
    buf.len = r;
    *out = buf;
}

slang_string *slang_net_rt_last_from(void) { return slang_string_from_cstr(t_from); }
int64_t slang_net_rt_last_from_port(void) { return t_from_port; }

int64_t slang_net_rt_send_to(int64_t fd, const slang_slice *data, const slang_string *ip, int64_t port, slang_error **err) {
    int64_t r = slang__plat_udp_sendto(fd, data->data, data->len, slang_string_data(ip), port);
    if (r == -SLANG_PLAT_EAGAIN) return -1;
    if (r < 0) {
        char a[96];
        addr_of(a, sizeof a, ip, port);
        set_err(err, "sendto", a, r);
        return -1;
    }
    return r;
}
