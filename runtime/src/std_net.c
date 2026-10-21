/* std_net.c - runtime side of std.net (slang_net_*): platform-independent
 * wrappers over the socket layer (platform/net_posix.c), the readiness
 * waits (task_io.c) and a resolver thread pool, called by the Slang-source
 * std.net (compiler/src/sema/stdlib_src/net.sl). See ABI.md 16.
 *
 * Sockets are handles: a net_sock owns the descriptor, counts the
 * operations in flight and closes the descriptor only when it is closed
 * AND no operation is running, so a descriptor number is never reused
 * while some thread may still pass it to a system call (the close race of
 * raw fds). Every operation acquires the handle, so a closed socket fails
 * with net.ErrClosed instead of touching someone else's descriptor. The
 * Slang socket object frees the handle in its Drop.
 *
 * Every socket is non-blocking: operations that would block return -1 (or
 * an empty slice with slang_net_rt_again() == true for the []byte reads),
 * and the Slang code then awaits slang_net_rt_wait_readable/writable. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- sentinel errors (std.net.Err*) ---- */

#define SENTINEL(cname, text)                                                                 \
    static SLANG_STRING_STORAGE(sizeof text) cname##_msg = SLANG_STRING_INIT(cname##_msg, text); \
    static slang_error cname##_err = { .hdr = { SLANG_RC_IMMORTAL, &slang_type_error },       \
                                       .message = (slang_string *)&cname##_msg };             \
    slang_error *slang_net_err_##cname(void) { return &cname##_err; }

SENTINEL(closed, "use of closed network connection")
SENTINEL(timeout, "i/o timeout")
SENTINEL(refused, "connection refused")
SENTINEL(reset, "connection reset by peer")
SENTINEL(unreachable, "network is unreachable")
SENTINEL(addr_in_use, "address already in use")
SENTINEL(not_found, "no such host")

slang_error *slang_net_err_eof(void) { return slang_err_eof; }

/* The error for an errno-style code (negative or positive): a sentinel for
 * the classes std.net names, else a plain error with the system text. +1. */
static slang_error *code_err(int64_t code) {
    int c = (int)(code < 0 ? -code : code);
    switch (slang__plat_err_class(c)) {
    case SLANG_PLAT_E_REFUSED: return &refused_err;
    case SLANG_PLAT_E_RESET: return &reset_err;
    case SLANG_PLAT_E_UNREACHABLE: return &unreachable_err;
    case SLANG_PLAT_E_ADDRINUSE: return &addr_in_use_err;
    case SLANG_PLAT_E_TIMEOUT: return &timeout_err;
    case SLANG_PLAT_E_CLOSED: return &closed_err;
    default: break;
    }
    char eb[160];
    return slang_error_new_cstr(slang__plat_strerror(c, eb, sizeof eb));
}

static void set_code(slang_error **err, int64_t code) {
    if (err) *err = code_err(code);
}

static void set_msg(slang_error **err, const char *msg) {
    if (err) *err = slang_error_new_cstr(msg);
}

/* rtOpError: an error whose own text is `prefix` and whose inner error is
 * `inner` (so the message is "prefix: inner" and errors.Is sees through
 * it), carrying the boxed OpError value of `boxed` for errors.As. +1. */
slang_error *slang_net_rt_op_error(const slang_error *boxed, const slang_string *prefix, const slang_error *inner) {
    slang_error *e = slang_error_wrap((slang_error *)inner, prefix);
    if (!e) {
        e = slang_error_new(prefix);
    }
    if (boxed && boxed->user_type) {
        e->user_type = boxed->user_type;
        slang_retain(boxed->payload);
        e->payload = boxed->payload;
    }
    return e;
}

/* ---- socket handles ---- */

typedef struct net_sock {
    int64_t fd;
    int64_t state;          /* atomic: (operations in flight << 1) | closed */
    int32_t released;       /* atomic: the descriptor was closed */
    int32_t family;         /* SLANG_PLAT_AF_* */
    int32_t type;           /* SLANG_PLAT_STREAM / DGRAM */
    int32_t has_peer;
    slang__plat_addr peer;  /* remote address known at dial/accept */
    char *unlink_path;      /* Unix listener: socket file removed on close */
} net_sock;

static net_sock *sock_of(int64_t h) {
    net_sock *s = (net_sock *)(intptr_t)h;
    if (!s) slang_panic("std.net: invalid socket handle");
    return s;
}

static int64_t sock_new(int64_t fd, int family, int type) {
    net_sock *s = calloc(1, sizeof *s);
    if (!s) slang_panic("out of memory (socket)");
    s->fd = fd;
    s->family = family;
    s->type = type;
    slang_io_register(fd);  /* the socket owns its poller registration */
    return (int64_t)(intptr_t)s;
}

static void final_close(net_sock *s) {
    if (__atomic_exchange_n(&s->released, 1, __ATOMIC_ACQ_REL)) return;
    /* A wait registered while an operation was in flight is released too. */
    slang_io_forget(s->fd);
    slang__plat_sock_close(s->fd);
    if (s->unlink_path) slang__plat_unlink(s->unlink_path);
}

static bool acquire(net_sock *s) {
    int64_t old = __atomic_fetch_add(&s->state, 2, __ATOMIC_ACQ_REL);
    if (old & 1) {
        if (__atomic_sub_fetch(&s->state, 2, __ATOMIC_ACQ_REL) == 1) final_close(s);
        return false;
    }
    return true;
}

static void release_op(net_sock *s) {
    if (__atomic_sub_fetch(&s->state, 2, __ATOMIC_ACQ_REL) == 1) final_close(s);
}

/* Acquires h for an operation; on a closed socket stores ErrClosed. */
#define ACQUIRE_OR(s, h, err, ret)                     \
    net_sock *s = sock_of(h);                          \
    if (!acquire(s)) {                                 \
        if (err) *(slang_error **)(err) = &closed_err; \
        return ret;                                    \
    }

void slang_net_rt_close(int64_t h) {
    if (!h) return;
    net_sock *s = sock_of(h);
    int64_t old = __atomic_fetch_or(&s->state, 1, __ATOMIC_ACQ_REL);
    if (old & 1) return;  /* double close: no-op */
    /* Wake every waiter now (they fail with ErrClosed); the descriptor is
     * closed once the operations in flight are done. */
    slang_io_forget(s->fd);
    if ((old >> 1) == 0) final_close(s);
}

void slang_net_rt_free(int64_t h) {
    if (!h) return;
    net_sock *s = sock_of(h);
    slang_net_rt_close(h);
    /* Only the Slang object's Drop frees: no operation can be in flight. */
    free(s->unlink_path);
    free(s);
}

bool slang_net_rt_is_closed(int64_t h) {
    return !h || (__atomic_load_n(&sock_of(h)->state, __ATOMIC_ACQUIRE) & 1);
}

int64_t slang_net_rt_fd(int64_t h) {
    net_sock *s = sock_of(h);
    return (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) & 1) ? -1 : s->fd;
}

/* ---- addresses ---- */

/* Parses "ip" or "ip%zone" into a socket address. */
static bool parse_ip_zone(const char *text, int64_t port, slang__plat_addr *a) {
    char ip[96];
    snprintf(ip, sizeof ip, "%s", text);
    uint32_t scope = 0;
    char *pct = strchr(ip, '%');
    if (pct) {
        *pct = 0;
        const char *z = pct + 1;
        char *end;
        unsigned long v = strtoul(z, &end, 10);
        scope = (*z && !*end) ? (uint32_t)v : slang__plat_if_index(z);
        if (!scope) return false;
    }
    unsigned char b[16];
    int len = slang__plat_parse_ip(ip, b);
    if (!len || (pct && len != 16)) return false;
    return slang__plat_addr_ip(a, b, len, port, scope);
}

void slang_net_rt_parse_ip(const slang_string *s, slang_slice *out) {
    memset(out, 0, sizeof *out);
    unsigned char b[16];
    int len = slang__plat_parse_ip(slang_string_cstr(s), b);
    if (!len) return;
    slang_slice_new(out, 1, len, len);
    memcpy(out->data, b, (size_t)len);
}

slang_string *slang_net_rt_format_ip(const slang_slice *b) {
    char t[64];
    if (!slang__plat_format_ip(b->data, (int)b->len, t, sizeof t)) return slang_string_from_cstr("<invalid IP>");
    return slang_string_from_cstr(t);
}

bool slang_net_rt_is_ip(const slang_string *s) { return slang__plat_is_ip(slang_string_cstr(s)); }

int64_t slang_net_rt_if_index(const slang_string *name) { return slang__plat_if_index(slang_string_cstr(name)); }

slang_string *slang_net_rt_if_name(int64_t index) {
    char b[64];
    if (index <= 0 || !slang__plat_if_name((uint32_t)index, b, sizeof b)) return slang_string_from_cstr("");
    return slang_string_from_cstr(b);
}

/* Formats a, showing IPv4-mapped IPv6 peers of dual-stack sockets as
 * plain IPv4 ("127.0.0.1:80", like Go). */
static slang_string *addr_string(const slang__plat_addr *a) {
    char b[160];
    unsigned char ip[16];
    int len;
    int64_t port;
    uint32_t scope;
    slang__plat_addr_parts(a, ip, &len, &port, &scope);
    static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (len == 16 && memcmp(ip, mapped, 12) == 0) {
        slang__plat_addr v4;
        slang__plat_addr_ip(&v4, ip + 12, 4, port, 0);
        slang__plat_addr_format(&v4, b, sizeof b);
        return slang_string_from_cstr(b);
    }
    slang__plat_addr_format(a, b, sizeof b);
    return slang_string_from_cstr(b);
}

slang_string *slang_net_rt_local_addr(int64_t h) {
    net_sock *s = sock_of(h);
    if (!acquire(s)) return slang_string_from_cstr("");
    slang__plat_addr a;
    slang_string *r = slang__plat_sock_name(s->fd, false, &a) ? addr_string(&a) : slang_string_from_cstr("");
    release_op(s);
    return r;
}

slang_string *slang_net_rt_remote_addr(int64_t h) {
    net_sock *s = sock_of(h);
    if (s->has_peer) return addr_string(&s->peer);  /* known at dial/accept: works after Close too */
    if (!acquire(s)) return slang_string_from_cstr("");
    slang__plat_addr a;
    slang_string *r = slang__plat_sock_name(s->fd, true, &a) ? addr_string(&a) : slang_string_from_cstr("");
    release_op(s);
    return r;
}

int64_t slang_net_rt_port(int64_t h) {
    net_sock *s = sock_of(h);
    if (!acquire(s)) return 0;
    slang__plat_addr a;
    int64_t port = 0;
    if (slang__plat_sock_name(s->fd, false, &a)) {
        unsigned char b[16];
        int len;
        uint32_t scope;
        slang__plat_addr_parts(&a, b, &len, &port, &scope);
    }
    release_op(s);
    return port;
}

/* ---- connect / listen / accept ---- */

static int family_of(const slang__plat_addr *a) { return slang__plat_addr_family(a); }

/* Optional local bind: lip == "" and lport == 0 mean none. */
static int64_t bind_local(int64_t fd, const slang_string *lip, int64_t lport, int fam) {
    const char *l = slang_string_cstr(lip);
    if (!*l && lport == 0) return 0;
    slang__plat_addr la;
    if (*l) {
        if (!parse_ip_zone(l, lport, &la)) return -22;
    } else {
        unsigned char zero[16] = { 0 };
        slang__plat_addr_ip(&la, zero, fam == SLANG_PLAT_AF_INET6 ? 16 : 4, lport, 0);
    }
    if (family_of(&la) != fam) return -22;
    return slang__plat_sock_bind(fd, &la);
}

int64_t slang_net_rt_dial(const slang_string *ip, int64_t port, const slang_string *lip, int64_t lport, slang_error **err) {
    slang__plat_addr a;
    if (!parse_ip_zone(slang_string_cstr(ip), port, &a)) {
        set_msg(err, "invalid address");
        return -1;
    }
    int fam = family_of(&a);
    int64_t fd = slang__plat_sock_open(fam, SLANG_PLAT_STREAM);
    if (fd < 0) { set_code(err, fd); return -1; }
    int64_t rc = bind_local(fd, lip, lport, fam);
    if (rc < 0) {
        slang__plat_sock_close(fd);
        if (rc == -22) set_msg(err, "local address does not match the remote address family");
        else set_code(err, rc);
        return -1;
    }
    slang__plat_sock_setopt(fd, SLANG_PLAT_OPT_NODELAY, 1);  /* Go's default */
    rc = slang__plat_sock_connect(fd, &a);
    if (rc < 0 && rc != -SLANG_PLAT_EAGAIN) {
        slang__plat_sock_close(fd);
        set_code(err, rc);
        return -1;
    }
    int64_t h = sock_new(fd, fam, SLANG_PLAT_STREAM);
    net_sock *s = sock_of(h);
    s->peer = a;
    s->has_peer = 1;
    return h;
}

int64_t slang_net_rt_dial_unix(const slang_string *path, slang_error **err) {
    slang__plat_addr a;
    if (!slang__plat_addr_unix(&a, slang_string_cstr(path))) {
        set_msg(err, "invalid unix socket path");
        return -1;
    }
    int64_t fd = slang__plat_sock_open(SLANG_PLAT_AF_UNIX, SLANG_PLAT_STREAM);
    if (fd < 0) { set_code(err, fd); return -1; }
    int64_t rc = slang__plat_sock_connect(fd, &a);
    if (rc < 0 && rc != -SLANG_PLAT_EAGAIN) {
        slang__plat_sock_close(fd);
        /* ENOENT: no such socket file; reported like a refused connection
         * by the text, with the system message */
        set_code(err, rc);
        return -1;
    }
    if (rc == -SLANG_PLAT_EAGAIN) {
        /* Unix stream sockets do not connect asynchronously: EAGAIN means
         * the listener's backlog is full. */
        slang__plat_sock_close(fd);
        set_msg(err, "resource temporarily unavailable (listen backlog full)");
        return -1;
    }
    int64_t h = sock_new(fd, SLANG_PLAT_AF_UNIX, SLANG_PLAT_STREAM);
    sock_of(h)->peer = a;
    sock_of(h)->has_peer = 1;
    return h;
}

void slang_net_rt_connect_result(int64_t h, slang_error **err) {
    ACQUIRE_OR(s, h, err, );
    int e = slang__plat_sock_error(s->fd);
    release_op(s);
    if (e) set_code(err, e);
}

/* Listen flags (rtListen / rtUdp): */
enum { LF_NO_REUSEADDR = 1, LF_REUSEPORT = 2, LF_V6ONLY = 4, LF_FASTOPEN = 8 };

static int64_t open_bound(const slang__plat_addr *a, int type, int64_t flags, bool dual, int64_t *err_out) {
    int fam = family_of(a);
    int64_t fd = slang__plat_sock_open(fam, type);
    if (fd < 0) { *err_out = fd; return -1; }
    if (!(flags & LF_NO_REUSEADDR) && type == SLANG_PLAT_STREAM) slang__plat_sock_setopt(fd, SLANG_PLAT_OPT_REUSEADDR, 1);
    if (flags & LF_REUSEPORT) {
        int e = slang__plat_sock_setopt(fd, SLANG_PLAT_OPT_REUSEPORT, 1);
        if (e) { slang__plat_sock_close(fd); *err_out = -e; return -1; }
    }
    if (fam == SLANG_PLAT_AF_INET6) slang__plat_sock_setopt(fd, SLANG_PLAT_OPT_V6ONLY, (flags & LF_V6ONLY) || !dual ? 1 : 0);
    int64_t rc = slang__plat_sock_bind(fd, a);
    if (rc < 0) { slang__plat_sock_close(fd); *err_out = rc; return -1; }
    return fd;
}

/* ip "" = every interface: "::" dual-stack, or 0.0.0.0 without IPv6. */
static bool listen_addr(const slang_string *ip, int64_t port, slang__plat_addr *a, bool *dual) {
    const char *s = slang_string_cstr(ip);
    *dual = false;
    if (!*s) {
        unsigned char zero[16] = { 0 };
        *dual = true;
        return slang__plat_addr_ip(a, zero, 16, port, 0);
    }
    return parse_ip_zone(s, port, a);
}

int64_t slang_net_rt_listen(const slang_string *ip, int64_t port, int64_t backlog, int64_t flags, slang_error **err) {
    slang__plat_addr a;
    bool dual;
    if (!listen_addr(ip, port, &a, &dual)) {
        set_msg(err, "invalid address");
        return -1;
    }
    int64_t code = 0;
    int64_t fd = open_bound(&a, SLANG_PLAT_STREAM, flags, dual, &code);
    if (fd < 0 && dual && slang__plat_err_class((int)-code) != SLANG_PLAT_E_ADDRINUSE) {  /* no IPv6 here */
        unsigned char zero[4] = { 0 };
        slang__plat_addr_ip(&a, zero, 4, port, 0);
        int64_t code4 = 0;
        fd = open_bound(&a, SLANG_PLAT_STREAM, flags, false, &code4);
        if (fd < 0) code = code4;
    }
    if (fd < 0) { set_code(err, code); return -1; }
    if (flags & LF_FASTOPEN) slang__plat_sock_setopt(fd, SLANG_PLAT_OPT_FASTOPEN, 256);
    int64_t rc = slang__plat_sock_listen(fd, backlog > 0 ? (int)backlog : 4096);
    if (rc < 0) { slang__plat_sock_close(fd); set_code(err, rc); return -1; }
    return sock_new(fd, family_of(&a), SLANG_PLAT_STREAM);
}

int64_t slang_net_rt_listen_unix(const slang_string *path, int64_t backlog, slang_error **err) {
    slang__plat_addr a;
    const char *p = slang_string_cstr(path);
    if (!slang__plat_addr_unix(&a, p)) {
        set_msg(err, "invalid unix socket path");
        return -1;
    }
    int64_t fd = slang__plat_sock_open(SLANG_PLAT_AF_UNIX, SLANG_PLAT_STREAM);
    if (fd < 0) { set_code(err, fd); return -1; }
    int64_t rc = slang__plat_sock_bind(fd, &a);
    if (rc >= 0) rc = slang__plat_sock_listen(fd, backlog > 0 ? (int)backlog : 4096);
    if (rc < 0) { slang__plat_sock_close(fd); set_code(err, rc); return -1; }
    int64_t h = sock_new(fd, SLANG_PLAT_AF_UNIX, SLANG_PLAT_STREAM);
    if (p[0] != '@') {
        sock_of(h)->unlink_path = strdup(p);
        if (!sock_of(h)->unlink_path) slang_panic("out of memory (socket)");
    }
    return h;
}

int64_t slang_net_rt_accept(int64_t h, slang_error **err) {
    ACQUIRE_OR(s, h, err, -1);
    slang__plat_addr from;
    int64_t c = slang__plat_sock_accept(s->fd, &from);
    int fam = s->family;
    release_op(s);
    if (c == -SLANG_PLAT_EAGAIN) return -1;
    if (c < 0) { set_code(err, c); return -1; }
    int64_t nh = sock_new(c, fam, SLANG_PLAT_STREAM);
    if (fam != SLANG_PLAT_AF_UNIX) {
        slang__plat_sock_setopt(c, SLANG_PLAT_OPT_NODELAY, 1);
        sock_of(nh)->peer = from;
        sock_of(nh)->has_peer = 1;
    }
    return nh;
}

/* ---- reading and writing ---- */

/* Whether the last []byte read on this thread would have blocked, and the
 * sender of the last datagram. Read right after the call (no await). */
static _Thread_local bool t_again;
static _Thread_local bool t_trunc;
static _Thread_local slang__plat_addr t_from;

bool slang_net_rt_again(void) { return t_again; }

/* Per-thread landing buffer of the allocating reads: recv into it, then
 * copy exactly the bytes received into a slice of that size (a 16 KB
 * Read(n) that gets 100 bytes costs 100 bytes, not a zeroed 16 KB). */
#define SCRATCH 65536
static _Thread_local unsigned char *t_scratch;
static slang__plat_tls_key scratch_key;  /* frees a thread's buffer when it exits */
static slang__plat_once scratch_once = SLANG_PLAT_ONCE_INIT;

static void scratch_key_init(void) { slang__plat_tls_new(&scratch_key, free); }

static unsigned char *scratch(void) {
    if (!t_scratch) {
        t_scratch = malloc(SCRATCH);
        if (!t_scratch) slang_panic("out of memory (net read)");
        slang__plat_once_run(&scratch_once, scratch_key_init);
        slang__plat_tls_set(scratch_key, t_scratch);
    }
    return t_scratch;
}

static void slice_of(slang_slice *out, const void *p, int64_t n) {
    memset(out, 0, sizeof *out);
    if (n <= 0) return;
    slang_slice_new(out, 1, n, n);
    memcpy(out->data, p, (size_t)n);
}

void slang_net_rt_read(int64_t h, int64_t n, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    t_again = false;
    if (n <= 0) return;
    ACQUIRE_OR(s, h, err, );
    if (n <= SCRATCH) {
        unsigned char *b = scratch();
        int64_t r = slang__plat_sock_recv(s->fd, b, n);
        release_op(s);
        if (r == -SLANG_PLAT_EAGAIN) t_again = true;
        else if (r < 0) set_code(err, r);
        else slice_of(out, b, r);
        return;
    }
    slang_slice buf;
    slang_slice_new(&buf, 1, n, n);
    int64_t r = slang__plat_sock_recv(s->fd, buf.data, n);
    release_op(s);
    if (r <= 0) {
        slang_slice_release(&buf);
        if (r == -SLANG_PLAT_EAGAIN) t_again = true;
        else if (r < 0) set_code(err, r);
        return;
    }
    buf.len = r;
    *out = buf;
}

int64_t slang_net_rt_read_into(int64_t h, const slang_slice *buf, int64_t off, slang_error **err) {
    if (off < 0 || off > buf->len) slang_panic("net read: offset out of range");
    if (off == buf->len) return 0;
    ACQUIRE_OR(s, h, err, -1);
    int64_t r = slang__plat_sock_recv(s->fd, (char *)buf->data + off, buf->len - off);
    release_op(s);
    if (r == -SLANG_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    return r;
}

static int64_t write_bytes(int64_t h, const char *p, int64_t len, int64_t off, slang_error **err) {
    if (off < 0 || off > len) slang_panic("net write: offset out of range");
    if (off == len) return 0;
    ACQUIRE_OR(s, h, err, -1);
    int64_t r = slang__plat_sock_send(s->fd, p + off, len - off);
    release_op(s);
    if (r == -SLANG_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    return r;
}

int64_t slang_net_rt_write(int64_t h, const slang_slice *data, int64_t off, slang_error **err) {
    return write_bytes(h, data->data, data->len, off, err);
}

int64_t slang_net_rt_write_str(int64_t h, const slang_string *s, int64_t off, slang_error **err) {
    return write_bytes(h, slang_string_data(s), slang_string_len(s), off, err);
}

/* Gathered write of bufs ([][]byte), skipping the first `off` bytes. */
int64_t slang_net_rt_write_v(int64_t h, const slang_slice *bufs, int64_t off, slang_error **err) {
    slang__plat_iovec iov[64];
    int n = 0;
    int64_t skip = off;
    const slang_slice *v = bufs->data;
    for (int64_t i = 0; i < bufs->len && n < 64; i++) {
        int64_t l = v[i].len;
        if (skip >= l) { skip -= l; continue; }
        iov[n].base = (const char *)v[i].data + skip;
        iov[n].len = (size_t)(l - skip);
        skip = 0;
        n++;
    }
    if (n == 0) return 0;
    ACQUIRE_OR(s, h, err, -1);
    int64_t r = slang__plat_sock_sendv(s->fd, iov, n);
    release_op(s);
    if (r == -SLANG_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    return r;
}

void slang_net_rt_shutdown(int64_t h, int64_t how, slang_error **err) {
    ACQUIRE_OR(s, h, err, );
    int e = slang__plat_sock_shutdown(s->fd, (int)how);
    release_op(s);
    if (e) set_code(err, e);
}

/* ---- files and sendfile ---- */

int64_t slang_net_rt_open_file(const slang_string *path, slang_error **err) {
    int64_t fd = slang__plat_file_open(slang_string_cstr(path));
    if (fd < 0) {
        if (err) *err = slang_error_errno("open", slang_string_cstr(path), (int)-fd);
        return -1;
    }
    return fd;
}

int64_t slang_net_rt_file_size(int64_t fd) {
    int64_t n = slang__plat_file_size(fd);
    return n < 0 ? 0 : n;
}

void slang_net_rt_close_file(int64_t fd) { slang__plat_file_close(fd); }

int64_t slang_net_rt_send_file(int64_t h, int64_t ffd, int64_t off, int64_t n, slang_error **err) {
    ACQUIRE_OR(s, h, err, -1);
    int64_t r = slang__plat_sendfile(s->fd, ffd, off, n);
    release_op(s);
    if (r == -SLANG_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    return r;
}

/* ---- waits ---- */

static slang_task *done_task(int64_t code) {
    slang_task *t = slang_task_new_plain(8, NULL);
    slang_task_complete(t, &code);
    return t;
}

static slang_task *wait_on(int64_t h, bool write, int64_t ms) {
    net_sock *s = sock_of(h);
    if (!acquire(s)) return done_task(2);
    slang_task *t = write ? slang_io_wait_writable(s->fd, ms > 0 ? ms : -1) : slang_io_wait_readable(s->fd, ms > 0 ? ms : -1);
    release_op(s);
    return t;
}

slang_task *slang_net_rt_wait_readable(int64_t h, int64_t ms) { return wait_on(h, false, ms); }
slang_task *slang_net_rt_wait_writable(int64_t h, int64_t ms) { return wait_on(h, true, ms); }

/* ---- options ---- */

void slang_net_rt_set_opt(int64_t h, int64_t opt, int64_t value, slang_error **err) {
    ACQUIRE_OR(s, h, err, );
    int e = slang__plat_sock_setopt(s->fd, (int)opt, value);
    release_op(s);
    if (e) set_code(err, e);
}

int64_t slang_net_rt_get_opt(int64_t h, int64_t opt, slang_error **err) {
    ACQUIRE_OR(s, h, err, 0);
    int64_t v = slang__plat_sock_getopt(s->fd, (int)opt);
    release_op(s);
    if (v < 0 && opt != SLANG_PLAT_OPT_LINGER) { set_code(err, v); return 0; }
    return v;
}

void slang_net_rt_mcast(int64_t h, bool join, const slang_string *group, const slang_string *ifname, slang_error **err) {
    unsigned char b[16];
    int len = slang__plat_parse_ip(slang_string_cstr(group), b);
    if (!len) { set_msg(err, "invalid multicast group"); return; }
    uint32_t idx = 0;
    if (slang_string_len(ifname) > 0) {
        idx = slang__plat_if_index(slang_string_cstr(ifname));
        if (!idx) { set_msg(err, "no such network interface"); return; }
    }
    ACQUIRE_OR(s, h, err, );
    int e = slang__plat_sock_mcast(s->fd, join, b, len, idx);
    release_op(s);
    if (e) set_code(err, e);
}

/* ---- UDP ---- */

int64_t slang_net_rt_udp(const slang_string *ip, int64_t port, int64_t flags, slang_error **err) {
    slang__plat_addr a;
    bool dual;
    if (!listen_addr(ip, port, &a, &dual)) {
        set_msg(err, "invalid address");
        return -1;
    }
    int64_t code = 0;
    /* UDP: no SO_REUSEADDR (on Linux it would let two sockets share a
     * unicast port without SO_REUSEPORT's load balancing) */
    int64_t fd = open_bound(&a, SLANG_PLAT_DGRAM, flags | LF_NO_REUSEADDR, dual, &code);
    if (fd < 0 && dual) {
        unsigned char zero[4] = { 0 };
        slang__plat_addr_ip(&a, zero, 4, port, 0);
        int64_t code4 = 0;
        fd = open_bound(&a, SLANG_PLAT_DGRAM, flags | LF_NO_REUSEADDR, false, &code4);
        if (fd < 0) code = code4;
    }
    if (fd < 0) { set_code(err, code); return -1; }
    return sock_new(fd, family_of(&a), SLANG_PLAT_DGRAM);
}

int64_t slang_net_rt_udp_dial(const slang_string *ip, int64_t port, const slang_string *lip, int64_t lport, slang_error **err) {
    slang__plat_addr a;
    if (!parse_ip_zone(slang_string_cstr(ip), port, &a)) {
        set_msg(err, "invalid address");
        return -1;
    }
    int fam = family_of(&a);
    int64_t fd = slang__plat_sock_open(fam, SLANG_PLAT_DGRAM);
    if (fd < 0) { set_code(err, fd); return -1; }
    int64_t rc = bind_local(fd, lip, lport, fam);
    if (rc >= 0) rc = slang__plat_sock_connect(fd, &a);
    if (rc < 0) {
        slang__plat_sock_close(fd);
        if (rc == -22) set_msg(err, "local address does not match the remote address family");
        else set_code(err, rc);
        return -1;
    }
    int64_t h = sock_new(fd, fam, SLANG_PLAT_DGRAM);
    sock_of(h)->peer = a;
    sock_of(h)->has_peer = 1;
    return h;
}

static void note_from(const slang__plat_addr *from, bool trunc) {
    t_from = *from;
    t_trunc = trunc;
}

void slang_net_rt_recv_from(int64_t h, int64_t n, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    t_again = false;
    memset(&t_from, 0, sizeof t_from);
    t_trunc = false;
    if (n <= 0) return;
    ACQUIRE_OR(s, h, err, );
    bool small = n <= SCRATCH;
    slang_slice buf;
    memset(&buf, 0, sizeof buf);
    if (!small) slang_slice_new(&buf, 1, n, n);
    void *dst = small ? (void *)scratch() : buf.data;
    slang__plat_addr from;
    bool trunc = false;
    int64_t r = slang__plat_sock_recvfrom(s->fd, dst, n, &from, &trunc);
    release_op(s);
    if (r < 0) {
        slang_slice_release(&buf);
        if (r == -SLANG_PLAT_EAGAIN) t_again = true;
        else set_code(err, r);
        return;
    }
    note_from(&from, trunc);
    if (small) {
        slice_of(out, dst, r);
        return;
    }
    buf.len = r;
    *out = buf;
}

int64_t slang_net_rt_read_from(int64_t h, const slang_slice *buf, slang_error **err) {
    memset(&t_from, 0, sizeof t_from);
    t_trunc = false;
    ACQUIRE_OR(s, h, err, -1);
    slang__plat_addr from;
    bool trunc = false;
    int64_t r = slang__plat_sock_recvfrom(s->fd, buf->data, buf->len, &from, &trunc);
    release_op(s);
    if (r == -SLANG_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    note_from(&from, trunc);
    return r;
}

slang_string *slang_net_rt_last_from(void) { return addr_string(&t_from); }

int64_t slang_net_rt_last_from_port(void) {
    unsigned char b[16];
    int len;
    int64_t port;
    uint32_t scope;
    slang__plat_addr_parts(&t_from, b, &len, &port, &scope);
    return port;
}

bool slang_net_rt_last_trunc(void) { return t_trunc; }

int64_t slang_net_rt_send_to(int64_t h, const slang_slice *data, const slang_string *ip, int64_t port, slang_error **err) {
    slang__plat_addr a, *to = NULL;
    if (slang_string_len(ip) > 0) {
        if (!parse_ip_zone(slang_string_cstr(ip), port, &a)) {
            set_msg(err, "invalid address");
            return -1;
        }
        to = &a;
    }
    ACQUIRE_OR(s, h, err, -1);
    if (to && s->family == SLANG_PLAT_AF_INET6 && family_of(to) == SLANG_PLAT_AF_INET) {
        /* dual-stack socket: send to the IPv4-mapped address */
        unsigned char b[16] = { 0 }, v4[16];
        int len;
        int64_t p;
        uint32_t sc;
        slang__plat_addr_parts(to, v4, &len, &p, &sc);
        b[10] = b[11] = 0xff;
        memcpy(b + 12, v4, 4);
        slang__plat_addr_ip(&a, b, 16, p, 0);
    }
    int64_t r = slang__plat_sock_sendto(s->fd, data->data, data->len, to);
    release_op(s);
    if (r == -SLANG_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    return r;
}

/* ---- interfaces ---- */

void slang_net_rt_interfaces(slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    char **lines;
    int n;
    int e = slang__plat_interfaces(&lines, &n);
    if (e) { set_code(err, e); return; }
    slang_slice res;
    slang_slice_new_ops(&res, 8, 0, n, &slang_ops_ref);
    for (int i = 0; i < n; i++) {
        slang_string *s = slang_string_from_cstr(lines[i]);
        slang_slice_append(&res, 8, &slang_ops_ref, &s);
        slang_release(s);
        free(lines[i]);
    }
    free(lines);
    *out = res;
}

int64_t slang_net_rt_lookup_port(const slang_string *network, const slang_string *service) {
    return slang__plat_lookup_port(slang_string_cstr(network), slang_string_cstr(service));
}

/* ---- name resolution: getaddrinfo on a small resolver pool ----
 * Lookups run on up to SLANG_RESOLVERS (default 4) detached threads that
 * start on demand and retire after 30 s without work, so a slow DNS server
 * never blocks a scheduler worker. A lookup's task completes with a handle
 * that rtResolveTake turns into the result. slang__net_stop (shutdown)
 * drops queued lookups and makes running ones discard their result. */

typedef struct resolve_req {
    int kind;               /* 0 = forward, 1 = reverse */
    int flags;
    char *host;
    slang_task *task;       /* owned until completed */
    char **addrs;
    int n;
    const char *errmsg;
    bool notfound;
    uint64_t gen;
    struct resolve_req *next;
} resolve_req;

static slang__plat_mutex rs_mu = SLANG_PLAT_MUTEX_INIT;
static slang__plat_cond rs_cv = SLANG_PLAT_COND_INIT;
static resolve_req *rs_head, *rs_tail;
static int rs_threads, rs_idle;
static uint64_t rs_gen;

static void req_free(resolve_req *r) {
    for (int i = 0; i < r->n; i++) free(r->addrs[i]);
    free(r->addrs);
    free(r->host);
    free(r);
}

static void req_run(resolve_req *r) {
    if (r->kind == 0) r->errmsg = slang__plat_resolve(r->host, r->flags, &r->addrs, &r->n, &r->notfound);
    else r->errmsg = slang__plat_reverse(r->host, &r->addrs, &r->n, &r->notfound);
}

static void *resolver_main(void *arg) {
    uint64_t gen = (uint64_t)(uintptr_t)arg;
    slang__plat_mutex_lock(&rs_mu);
    for (;;) {
        if (rs_gen != gen) break;
        resolve_req *r = rs_head;
        if (!r) {
            rs_idle++;
            bool woke = slang__plat_cond_wait_ns(&rs_cv, &rs_mu, (int64_t)30 * 1000000000);
            if (rs_gen != gen) break;
            rs_idle--;
            if (!woke && !rs_head) {
                rs_threads--;
                break;
            }
            continue;
        }
        rs_head = r->next;
        if (!rs_head) rs_tail = NULL;
        slang__plat_mutex_unlock(&rs_mu);
        req_run(r);
        slang__plat_mutex_lock(&rs_mu);
        if (r->gen != rs_gen) {
            /* shut down meanwhile: nobody runs continuations any more */
            slang_release(r->task);
            req_free(r);
            continue;
        }
        slang__plat_mutex_unlock(&rs_mu);
        int64_t hnd = (int64_t)(intptr_t)r;
        slang_task *t = r->task;
        r->task = NULL;
        slang_task_complete(t, &hnd);
        slang_release(t);
        slang__plat_mutex_lock(&rs_mu);
    }
    slang__plat_mutex_unlock(&rs_mu);
    return NULL;
}

static int resolver_max(void) {
    const char *env = getenv("SLANG_RESOLVERS");
    long n = env ? strtol(env, NULL, 10) : 0;
    if (n <= 0) n = 4;
    if (n > 64) n = 64;
    return (int)n;
}

static slang_task *submit(int kind, const slang_string *host, int64_t flags) {
    slang_task *t = slang_task_new_plain(8, NULL);
    resolve_req *r = calloc(1, sizeof *r);
    if (!r) slang_panic("out of memory (resolve)");
    r->kind = kind;
    r->flags = (int)flags;
    r->host = strdup(slang_string_cstr(host));
    if (!r->host) slang_panic("out of memory (resolve)");
    if (kind == 0) {
        unsigned char b[16];
        char ip[96];
        snprintf(ip, sizeof ip, "%s", r->host);
        char *pct = strchr(ip, '%');
        if (pct) *pct = 0;
        if (slang__plat_parse_ip(ip, b)) {  /* a literal resolves to itself, at once */
            r->addrs = calloc(2, sizeof *r->addrs);
            if (!r->addrs) slang_panic("out of memory (resolve)");
            r->addrs[0] = strdup(r->host);
            r->n = 1;
            int64_t h = (int64_t)(intptr_t)r;
            slang_task_complete(t, &h);
            return t;
        }
    }
    slang_retain(t);
    r->task = t;
    slang__plat_mutex_lock(&rs_mu);
    r->gen = rs_gen;
    if (rs_tail) rs_tail->next = r; else rs_head = r;
    rs_tail = r;
    if (rs_idle == 0 && rs_threads < resolver_max()) {
        slang__net_stop_fn = slang__net_stop;  /* shutdown stops the pool */
        if (slang__plat_thread_start_detached(resolver_main, (void *)(uintptr_t)rs_gen)) rs_threads++;
        else if (rs_threads == 0) {
            /* no thread at all: fail the lookup now */
            rs_head = rs_tail = NULL;
            slang__plat_mutex_unlock(&rs_mu);
            r->errmsg = "cannot start a resolver thread";
            r->task = NULL;
            int64_t h = (int64_t)(intptr_t)r;
            slang_task_complete(t, &h);
            slang_release(t);
            return t;
        }
    }
    slang__plat_cond_broadcast(&rs_cv);
    slang__plat_mutex_unlock(&rs_mu);
    return t;
}

slang_task *slang_net_rt_resolve(const slang_string *host, int64_t flags) { return submit(0, host, flags); }
slang_task *slang_net_rt_reverse(const slang_string *ip) { return submit(1, ip, 0); }

static void take_result(resolve_req *r, const char *op, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (r->errmsg) {
        char msg[512];
        snprintf(msg, sizeof msg, "%s %s", op, r->host);
        slang_string *ctx = slang_string_from_cstr(msg);
        slang_error *inner = r->notfound ? &not_found_err : slang_error_new_cstr(r->errmsg);
        *err = slang_error_wrap(inner, ctx);
        slang_release(inner);
        slang_release(ctx);
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
    req_free(r);
}

void slang_net_rt_resolve_take(int64_t handle, slang_slice *out, slang_error **err) {
    resolve_req *r = (resolve_req *)(intptr_t)handle;
    if (!r) slang_panic_nil();
    take_result(r, "lookup", out, err);
}

/* Blocking lookup on the calling thread (Listen with a host name). */
void slang_net_rt_resolve_sync(const slang_string *host, int64_t flags, slang_slice *out, slang_error **err) {
    resolve_req *r = calloc(1, sizeof *r);
    if (!r) slang_panic("out of memory (resolve)");
    r->host = strdup(slang_string_cstr(host));
    if (!r->host) slang_panic("out of memory (resolve)");
    r->flags = (int)flags;
    req_run(r);
    take_result(r, "lookup", out, err);
}

void slang__net_stop(void) {
    slang__plat_mutex_lock(&rs_mu);
    rs_gen++;
    resolve_req *r = rs_head;
    rs_head = rs_tail = NULL;
    rs_threads = 0;
    rs_idle = 0;
    /* Idle threads are not woken: they stay parked until their idle
     * timeout (then see the new generation and exit), so a process that
     * exits right after shutdown does not race glibc's per-thread
     * resolver state teardown. submit() broadcasts, so an old thread
     * taking a wake-up meant for a new one cannot lose it. */
    slang__plat_mutex_unlock(&rs_mu);
    while (r) {
        resolve_req *n = r->next;
        slang_release(r->task);
        req_free(r);
        r = n;
    }
}

/* ---- dialing helpers ---- */

/* Task whose int64 result is the index of the first handle to become
 * writable (or whose wait timed out after ms > 0, or was cancelled); the
 * caller then checks every attempt with rtConnectCheck. */
slang_task *slang_net_rt_wait_any_writable(const slang_slice *hs, int64_t ms) {
    int64_t n = hs->len;
    if (n <= 0) slang_panic("std.net: wait on no sockets");
    slang_task **ws = malloc(sizeof *ws * (size_t)n);
    if (!ws) slang_panic("out of memory (dial)");
    const int64_t *h = hs->data;
    for (int64_t i = 0; i < n; i++) ws[i] = wait_on(h[i], true, ms);
    slang_task *any = slang_task_any(ws, n);
    for (int64_t i = 0; i < n; i++) slang_release(ws[i]);
    free(ws);
    return any;
}

/* 1 connected, 0 still connecting, or the connect error. */
int64_t slang_net_rt_connect_check(int64_t h, slang_error **err) {
    ACQUIRE_OR(s, h, err, 0);
    int e = slang__plat_sock_error(s->fd);
    slang__plat_addr a;
    bool connected = !e && slang__plat_sock_name(s->fd, true, &a);
    /* A TCP self-connect (the kernel picked the dialled loopback port as
     * the ephemeral source port: simultaneous open with itself) succeeds
     * although nothing listens there. Like Go, treat it as "connection
     * refused" (a listener's port is never chosen as a source port). */
    bool self = false;
    slang__plat_addr la;
    if (connected && slang__plat_addr_family(&a) != SLANG_PLAT_AF_UNIX && slang__plat_sock_name(s->fd, false, &la)) {
        unsigned char ip1[16], ip2[16];
        int n1 = 0, n2 = 0;
        int64_t p1 = 0, p2 = 0;
        uint32_t z1 = 0, z2 = 0;
        slang__plat_addr_parts(&a, ip1, &n1, &p1, &z1);
        slang__plat_addr_parts(&la, ip2, &n2, &p2, &z2);
        self = n1 > 0 && n1 == n2 && p1 == p2 && memcmp(ip1, ip2, (size_t)n1) == 0;
    }
    release_op(s);
    if (e) { set_code(err, e); return 0; }
    if (self) { if (err) *err = &refused_err; return 0; }
    return connected ? 1 : 0;
}

/* ---- byte helpers for net.sl ---- */

void slang_net_rt_bytes(int64_t n, slang_slice *out) {
    memset(out, 0, sizeof *out);
    if (n > 0) slang_slice_new(out, 1, n, n);
}

/* Copies min(n, len(dst) - doff, len(src) - soff) bytes; returns the count. */
int64_t slang_net_rt_copy_bytes(const slang_slice *dst, int64_t doff, const slang_slice *src, int64_t soff, int64_t n) {
    if (doff < 0 || soff < 0 || doff > dst->len || soff > src->len) slang_panic("net: copy out of range");
    int64_t k = n;
    if (k > dst->len - doff) k = dst->len - doff;
    if (k > src->len - soff) k = src->len - soff;
    if (k <= 0) return 0;
    memmove((char *)dst->data + doff, (const char *)src->data + soff, (size_t)k);
    return k;
}

/* Index of byte c in b at or after `from`, or -1. */
int64_t slang_net_rt_index_byte(const slang_slice *b, int64_t from, int64_t c) {
    if (from < 0) from = 0;
    if (from >= b->len) return -1;
    const char *p = memchr((const char *)b->data + from, (int)(c & 0xff), (size_t)(b->len - from));
    return p ? (int64_t)(p - (const char *)b->data) : -1;
}

/* a ++ b[..bn] as a new slice. */
void slang_net_rt_join(const slang_slice *a, const slang_slice *b, int64_t bn, slang_slice *out) {
    if (bn > b->len) bn = b->len;
    if (bn < 0) bn = 0;
    int64_t n = a->len + bn;
    memset(out, 0, sizeof *out);
    if (n == 0) return;
    slang_slice_new(out, 1, n, n + n / 2);
    out->len = n;
    if (a->len) memcpy(out->data, a->data, (size_t)a->len);
    if (bn) memcpy((char *)out->data + a->len, b->data, (size_t)bn);
}
