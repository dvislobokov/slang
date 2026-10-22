/* std_net.c - runtime side of std.net (zell_net_*): platform-independent
 * wrappers over the socket layer (platform/net_posix.c), the readiness
 * waits (task_io.c) and a resolver thread pool, called by the Zell-source
 * std.net (compiler/src/sema/stdlib_src/net.zl). See ABI.md 16.
 *
 * Sockets are handles: a net_sock owns the descriptor, counts the
 * operations in flight and closes the descriptor only when it is closed
 * AND no operation is running, so a descriptor number is never reused
 * while some thread may still pass it to a system call (the close race of
 * raw fds). Every operation acquires the handle, so a closed socket fails
 * with net.ErrClosed instead of touching someone else's descriptor. The
 * Zell socket object frees the handle in its Drop.
 *
 * Every socket is non-blocking: operations that would block return -1 (or
 * an empty slice with zell_net_rt_again() == true for the []byte reads),
 * and the Zell code then awaits zell_net_rt_wait_readable/writable. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- sentinel errors (std.net.Err*) ---- */

#define SENTINEL(cname, text)                                                                 \
    static ZELL_STRING_STORAGE(sizeof text) cname##_msg = ZELL_STRING_INIT(cname##_msg, text); \
    static zell_error cname##_err = { .hdr = { ZELL_RC_IMMORTAL, &zell_type_error },       \
                                       .message = (zell_string *)&cname##_msg };             \
    zell_error *zell_net_err_##cname(void) { return &cname##_err; }

SENTINEL(closed, "use of closed network connection")
SENTINEL(timeout, "i/o timeout")
SENTINEL(refused, "connection refused")
SENTINEL(reset, "connection reset by peer")
SENTINEL(unreachable, "network is unreachable")
SENTINEL(addr_in_use, "address already in use")
SENTINEL(not_found, "no such host")

zell_error *zell_net_err_eof(void) { return zell_err_eof; }

/* The error for an errno-style code (negative or positive): a sentinel for
 * the classes std.net names, else a plain error with the system text. +1. */
static zell_error *code_err(int64_t code) {
    int c = (int)(code < 0 ? -code : code);
    switch (zell__plat_err_class(c)) {
    case ZELL_PLAT_E_REFUSED: return &refused_err;
    case ZELL_PLAT_E_RESET: return &reset_err;
    case ZELL_PLAT_E_UNREACHABLE: return &unreachable_err;
    case ZELL_PLAT_E_ADDRINUSE: return &addr_in_use_err;
    case ZELL_PLAT_E_TIMEOUT: return &timeout_err;
    case ZELL_PLAT_E_CLOSED: return &closed_err;
    default: break;
    }
    char eb[160];
    return zell_error_new_cstr(zell__plat_strerror(c, eb, sizeof eb));
}

static void set_code(zell_error **err, int64_t code) {
    if (err) *err = code_err(code);
}

static void set_msg(zell_error **err, const char *msg) {
    if (err) *err = zell_error_new_cstr(msg);
}

/* rtOpError: an error whose own text is `prefix` and whose inner error is
 * `inner` (so the message is "prefix: inner" and errors.Is sees through
 * it), carrying the boxed OpError value of `boxed` for errors.As. +1. */
zell_error *zell_net_rt_op_error(const zell_error *boxed, const zell_string *prefix, const zell_error *inner) {
    zell_error *e = zell_error_wrap((zell_error *)inner, prefix);
    if (!e) {
        e = zell_error_new(prefix);
    }
    if (boxed && boxed->user_type) {
        e->user_type = boxed->user_type;
        zell_retain(boxed->payload);
        e->payload = boxed->payload;
    }
    return e;
}

/* ---- socket handles ---- */

typedef struct net_sock {
    int64_t fd;
    int64_t state;          /* atomic: (operations in flight << 1) | closed */
    int32_t released;       /* atomic: the descriptor was closed */
    int32_t family;         /* ZELL_PLAT_AF_* */
    int32_t type;           /* ZELL_PLAT_STREAM / DGRAM */
    int32_t has_peer;
    int32_t listening;      /* a listener: shut down before close */
    zell__plat_addr peer;  /* remote address known at dial/accept */
    char *unlink_path;      /* Unix listener: socket file removed on close */
} net_sock;

static net_sock *sock_of(int64_t h) {
    net_sock *s = (net_sock *)(intptr_t)h;
    if (!s) zell_panic("std.net: invalid socket handle");
    return s;
}

static int64_t sock_new(int64_t fd, int family, int type) {
    net_sock *s = calloc(1, sizeof *s);
    if (!s) zell_panic("out of memory (socket)");
    s->fd = fd;
    s->family = family;
    s->type = type;
    zell_io_register(fd);  /* the socket owns its poller registration */
    return (int64_t)(intptr_t)s;
}

static void final_close(net_sock *s) {
    if (__atomic_exchange_n(&s->released, 1, __ATOMIC_ACQ_REL)) return;
    /* A wait registered while an operation was in flight is released too. */
    zell_io_forget(s->fd);
    /* A listener stops accepting now, even if the descriptor is still
     * referenced elsewhere (an io_uring request completed a moment ago
     * whose file reference the kernel drops lazily, a thread inside
     * accept): close(2) alone would leave it listening until the last
     * reference goes, and a dial could still be accepted, then reset. */
    if (s->listening) zell__plat_sock_shutdown(s->fd, ZELL_PLAT_READ);
    zell__plat_sock_close(s->fd);
    if (s->unlink_path) zell__plat_unlink(s->unlink_path);
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
        if (err) *(zell_error **)(err) = &closed_err; \
        return ret;                                    \
    }

void zell_net_rt_close(int64_t h) {
    if (!h) return;
    net_sock *s = sock_of(h);
    int64_t old = __atomic_fetch_or(&s->state, 1, __ATOMIC_ACQ_REL);
    if (old & 1) return;  /* double close: no-op */
    /* Wake every waiter now (they fail with ErrClosed); the descriptor is
     * closed once the operations in flight are done. */
    zell_io_forget(s->fd);
    if ((old >> 1) == 0) final_close(s);
}

void zell_net_rt_free(int64_t h) {
    if (!h) return;
    net_sock *s = sock_of(h);
    zell_net_rt_close(h);
    /* Only the Zell object's Drop frees: no operation can be in flight. */
    free(s->unlink_path);
    free(s);
}

bool zell_net_rt_is_closed(int64_t h) {
    return !h || (__atomic_load_n(&sock_of(h)->state, __ATOMIC_ACQUIRE) & 1);
}

int64_t zell_net_rt_fd(int64_t h) {
    net_sock *s = sock_of(h);
    return (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) & 1) ? -1 : s->fd;
}

/* ---- addresses ---- */

/* Parses "ip" or "ip%zone" into a socket address. */
static bool parse_ip_zone(const char *text, int64_t port, zell__plat_addr *a) {
    char ip[96];
    snprintf(ip, sizeof ip, "%s", text);
    uint32_t scope = 0;
    char *pct = strchr(ip, '%');
    if (pct) {
        *pct = 0;
        const char *z = pct + 1;
        char *end;
        unsigned long v = strtoul(z, &end, 10);
        scope = (*z && !*end) ? (uint32_t)v : zell__plat_if_index(z);
        if (!scope) return false;
    }
    unsigned char b[16];
    int len = zell__plat_parse_ip(ip, b);
    if (!len || (pct && len != 16)) return false;
    return zell__plat_addr_ip(a, b, len, port, scope);
}

void zell_net_rt_parse_ip(const zell_string *s, zell_slice *out) {
    memset(out, 0, sizeof *out);
    unsigned char b[16];
    int len = zell__plat_parse_ip(zell_string_cstr(s), b);
    if (!len) return;
    zell_slice_new(out, 1, len, len);
    memcpy(out->data, b, (size_t)len);
}

zell_string *zell_net_rt_format_ip(const zell_slice *b) {
    char t[64];
    if (!zell__plat_format_ip(b->data, (int)b->len, t, sizeof t)) return zell_string_from_cstr("<invalid IP>");
    return zell_string_from_cstr(t);
}

bool zell_net_rt_is_ip(const zell_string *s) { return zell__plat_is_ip(zell_string_cstr(s)); }

int64_t zell_net_rt_if_index(const zell_string *name) { return zell__plat_if_index(zell_string_cstr(name)); }

zell_string *zell_net_rt_if_name(int64_t index) {
    char b[64];
    if (index <= 0 || !zell__plat_if_name((uint32_t)index, b, sizeof b)) return zell_string_from_cstr("");
    return zell_string_from_cstr(b);
}

/* Formats a, showing IPv4-mapped IPv6 peers of dual-stack sockets as
 * plain IPv4 ("127.0.0.1:80", like Go). */
static zell_string *addr_string(const zell__plat_addr *a) {
    char b[160];
    unsigned char ip[16];
    int len;
    int64_t port;
    uint32_t scope;
    zell__plat_addr_parts(a, ip, &len, &port, &scope);
    static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (len == 16 && memcmp(ip, mapped, 12) == 0) {
        zell__plat_addr v4;
        zell__plat_addr_ip(&v4, ip + 12, 4, port, 0);
        zell__plat_addr_format(&v4, b, sizeof b);
        return zell_string_from_cstr(b);
    }
    zell__plat_addr_format(a, b, sizeof b);
    return zell_string_from_cstr(b);
}

zell_string *zell_net_rt_local_addr(int64_t h) {
    net_sock *s = sock_of(h);
    if (!acquire(s)) return zell_string_from_cstr("");
    zell__plat_addr a;
    zell_string *r = zell__plat_sock_name(s->fd, false, &a) ? addr_string(&a) : zell_string_from_cstr("");
    release_op(s);
    return r;
}

zell_string *zell_net_rt_remote_addr(int64_t h) {
    net_sock *s = sock_of(h);
    if (s->has_peer) return addr_string(&s->peer);  /* known at dial/accept: works after Close too */
    if (!acquire(s)) return zell_string_from_cstr("");
    zell__plat_addr a;
    zell_string *r = zell__plat_sock_name(s->fd, true, &a) ? addr_string(&a) : zell_string_from_cstr("");
    release_op(s);
    return r;
}

int64_t zell_net_rt_port(int64_t h) {
    net_sock *s = sock_of(h);
    if (!acquire(s)) return 0;
    zell__plat_addr a;
    int64_t port = 0;
    if (zell__plat_sock_name(s->fd, false, &a)) {
        unsigned char b[16];
        int len;
        uint32_t scope;
        zell__plat_addr_parts(&a, b, &len, &port, &scope);
    }
    release_op(s);
    return port;
}

/* ---- connect / listen / accept ---- */

static int family_of(const zell__plat_addr *a) { return zell__plat_addr_family(a); }

/* Optional local bind: lip == "" and lport == 0 mean none. */
static int64_t bind_local(int64_t fd, const zell_string *lip, int64_t lport, int fam) {
    const char *l = zell_string_cstr(lip);
    if (!*l && lport == 0) return 0;
    zell__plat_addr la;
    if (*l) {
        if (!parse_ip_zone(l, lport, &la)) return -22;
    } else {
        unsigned char zero[16] = { 0 };
        zell__plat_addr_ip(&la, zero, fam == ZELL_PLAT_AF_INET6 ? 16 : 4, lport, 0);
    }
    if (family_of(&la) != fam) return -22;
    return zell__plat_sock_bind(fd, &la);
}

int64_t zell_net_rt_dial(const zell_string *ip, int64_t port, const zell_string *lip, int64_t lport, zell_error **err) {
    zell__plat_addr a;
    if (!parse_ip_zone(zell_string_cstr(ip), port, &a)) {
        set_msg(err, "invalid address");
        return -1;
    }
    int fam = family_of(&a);
    int64_t fd = zell__plat_sock_open(fam, ZELL_PLAT_STREAM);
    if (fd < 0) { set_code(err, fd); return -1; }
    int64_t rc = bind_local(fd, lip, lport, fam);
    if (rc < 0) {
        zell__plat_sock_close(fd);
        if (rc == -22) set_msg(err, "local address does not match the remote address family");
        else set_code(err, rc);
        return -1;
    }
    zell__plat_sock_setopt(fd, ZELL_PLAT_OPT_NODELAY, 1);  /* Go's default */
    rc = zell__plat_sock_connect(fd, &a);
    if (rc < 0 && rc != -ZELL_PLAT_EAGAIN) {
        zell__plat_sock_close(fd);
        set_code(err, rc);
        return -1;
    }
    int64_t h = sock_new(fd, fam, ZELL_PLAT_STREAM);
    net_sock *s = sock_of(h);
    s->peer = a;
    s->has_peer = 1;
    return h;
}

int64_t zell_net_rt_dial_unix(const zell_string *path, zell_error **err) {
    zell__plat_addr a;
    if (!zell__plat_addr_unix(&a, zell_string_cstr(path))) {
        set_msg(err, "invalid unix socket path");
        return -1;
    }
    int64_t fd = zell__plat_sock_open(ZELL_PLAT_AF_UNIX, ZELL_PLAT_STREAM);
    if (fd < 0) { set_code(err, fd); return -1; }
    int64_t rc = zell__plat_sock_connect(fd, &a);
    if (rc < 0 && rc != -ZELL_PLAT_EAGAIN) {
        zell__plat_sock_close(fd);
        /* ENOENT: no such socket file; reported like a refused connection
         * by the text, with the system message */
        set_code(err, rc);
        return -1;
    }
    if (rc == -ZELL_PLAT_EAGAIN) {
        /* Unix stream sockets do not connect asynchronously: EAGAIN means
         * the listener's backlog is full. */
        zell__plat_sock_close(fd);
        set_msg(err, "resource temporarily unavailable (listen backlog full)");
        return -1;
    }
    int64_t h = sock_new(fd, ZELL_PLAT_AF_UNIX, ZELL_PLAT_STREAM);
    sock_of(h)->peer = a;
    sock_of(h)->has_peer = 1;
    return h;
}

void zell_net_rt_connect_result(int64_t h, zell_error **err) {
    ACQUIRE_OR(s, h, err, );
    int e = zell__plat_sock_error(s->fd);
    release_op(s);
    if (e) set_code(err, e);
}

/* Listen flags (rtListen / rtUdp): */
enum { LF_NO_REUSEADDR = 1, LF_REUSEPORT = 2, LF_V6ONLY = 4, LF_FASTOPEN = 8 };

static int64_t open_bound(const zell__plat_addr *a, int type, int64_t flags, bool dual, int64_t *err_out) {
    int fam = family_of(a);
    int64_t fd = zell__plat_sock_open(fam, type);
    if (fd < 0) { *err_out = fd; return -1; }
    if (!(flags & LF_NO_REUSEADDR) && type == ZELL_PLAT_STREAM) zell__plat_sock_setopt(fd, ZELL_PLAT_OPT_REUSEADDR, 1);
    if (flags & LF_REUSEPORT) {
        int e = zell__plat_sock_setopt(fd, ZELL_PLAT_OPT_REUSEPORT, 1);
        if (e) { zell__plat_sock_close(fd); *err_out = -e; return -1; }
    }
    if (fam == ZELL_PLAT_AF_INET6) zell__plat_sock_setopt(fd, ZELL_PLAT_OPT_V6ONLY, (flags & LF_V6ONLY) || !dual ? 1 : 0);
    int64_t rc = zell__plat_sock_bind(fd, a);
    if (rc < 0) { zell__plat_sock_close(fd); *err_out = rc; return -1; }
    return fd;
}

/* ip "" = every interface: "::" dual-stack, or 0.0.0.0 without IPv6. */
static bool listen_addr(const zell_string *ip, int64_t port, zell__plat_addr *a, bool *dual) {
    const char *s = zell_string_cstr(ip);
    *dual = false;
    if (!*s) {
        unsigned char zero[16] = { 0 };
        *dual = true;
        return zell__plat_addr_ip(a, zero, 16, port, 0);
    }
    return parse_ip_zone(s, port, a);
}

int64_t zell_net_rt_listen(const zell_string *ip, int64_t port, int64_t backlog, int64_t flags, zell_error **err) {
    zell__plat_addr a;
    bool dual;
    if (!listen_addr(ip, port, &a, &dual)) {
        set_msg(err, "invalid address");
        return -1;
    }
    int64_t code = 0;
    int64_t fd = open_bound(&a, ZELL_PLAT_STREAM, flags, dual, &code);
    if (fd < 0 && dual && zell__plat_err_class((int)-code) != ZELL_PLAT_E_ADDRINUSE) {  /* no IPv6 here */
        unsigned char zero[4] = { 0 };
        zell__plat_addr_ip(&a, zero, 4, port, 0);
        int64_t code4 = 0;
        fd = open_bound(&a, ZELL_PLAT_STREAM, flags, false, &code4);
        if (fd < 0) code = code4;
    }
    if (fd < 0) { set_code(err, code); return -1; }
    if (flags & LF_FASTOPEN) zell__plat_sock_setopt(fd, ZELL_PLAT_OPT_FASTOPEN, 256);
    int64_t rc = zell__plat_sock_listen(fd, backlog > 0 ? (int)backlog : 4096);
    if (rc < 0) { zell__plat_sock_close(fd); set_code(err, rc); return -1; }
    int64_t h = sock_new(fd, family_of(&a), ZELL_PLAT_STREAM);
    sock_of(h)->listening = 1;
    return h;
}

int64_t zell_net_rt_listen_unix(const zell_string *path, int64_t backlog, zell_error **err) {
    zell__plat_addr a;
    const char *p = zell_string_cstr(path);
    if (!zell__plat_addr_unix(&a, p)) {
        set_msg(err, "invalid unix socket path");
        return -1;
    }
    int64_t fd = zell__plat_sock_open(ZELL_PLAT_AF_UNIX, ZELL_PLAT_STREAM);
    if (fd < 0) { set_code(err, fd); return -1; }
    int64_t rc = zell__plat_sock_bind(fd, &a);
    if (rc >= 0) rc = zell__plat_sock_listen(fd, backlog > 0 ? (int)backlog : 4096);
    if (rc < 0) { zell__plat_sock_close(fd); set_code(err, rc); return -1; }
    int64_t h = sock_new(fd, ZELL_PLAT_AF_UNIX, ZELL_PLAT_STREAM);
    sock_of(h)->listening = 1;
    if (p[0] != '@') {
        sock_of(h)->unlink_path = strdup(p);
        if (!sock_of(h)->unlink_path) zell_panic("out of memory (socket)");
    }
    return h;
}

int64_t zell_net_rt_accept(int64_t h, zell_error **err) {
    ACQUIRE_OR(s, h, err, -1);
    zell__plat_addr from;
    int64_t c = zell__plat_sock_accept(s->fd, &from);
    int fam = s->family;
    release_op(s);
    if (c == -ZELL_PLAT_EAGAIN) return -1;
    if (c < 0) { set_code(err, c); return -1; }
    int64_t nh = sock_new(c, fam, ZELL_PLAT_STREAM);
    if (fam != ZELL_PLAT_AF_UNIX) {
        zell__plat_sock_setopt(c, ZELL_PLAT_OPT_NODELAY, 1);
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
static _Thread_local zell__plat_addr t_from;

bool zell_net_rt_again(void) { return t_again; }

/* Per-thread landing buffer of the allocating reads: recv into it, then
 * copy exactly the bytes received into a slice of that size (a 16 KB
 * Read(n) that gets 100 bytes costs 100 bytes, not a zeroed 16 KB). */
#define SCRATCH 65536
static _Thread_local unsigned char *t_scratch;
static zell__plat_tls_key scratch_key;  /* frees a thread's buffer when it exits */
static zell__plat_once scratch_once = ZELL_PLAT_ONCE_INIT;

static void scratch_key_init(void) { zell__plat_tls_new(&scratch_key, free); }

static unsigned char *scratch(void) {
    if (!t_scratch) {
        t_scratch = malloc(SCRATCH);
        if (!t_scratch) zell_panic("out of memory (net read)");
        zell__plat_once_run(&scratch_once, scratch_key_init);
        zell__plat_tls_set(scratch_key, t_scratch);
    }
    return t_scratch;
}

static void slice_of(zell_slice *out, const void *p, int64_t n) {
    memset(out, 0, sizeof *out);
    if (n <= 0) return;
    zell_slice_new(out, 1, n, n);
    memcpy(out->data, p, (size_t)n);
}

void zell_net_rt_read(int64_t h, int64_t n, zell_slice *out, zell_error **err) {
    memset(out, 0, sizeof *out);
    t_again = false;
    if (n <= 0) return;
    ACQUIRE_OR(s, h, err, );
    if (n <= SCRATCH) {
        unsigned char *b = scratch();
        int64_t r = zell__plat_sock_recv(s->fd, b, n);
        release_op(s);
        if (r == -ZELL_PLAT_EAGAIN) t_again = true;
        else if (r < 0) set_code(err, r);
        else slice_of(out, b, r);
        return;
    }
    zell_slice buf;
    zell_slice_new(&buf, 1, n, n);
    int64_t r = zell__plat_sock_recv(s->fd, buf.data, n);
    release_op(s);
    if (r <= 0) {
        zell_slice_release(&buf);
        if (r == -ZELL_PLAT_EAGAIN) t_again = true;
        else if (r < 0) set_code(err, r);
        return;
    }
    buf.len = r;
    *out = buf;
}

int64_t zell_net_rt_read_into(int64_t h, const zell_slice *buf, int64_t off, zell_error **err) {
    if (off < 0 || off > buf->len) zell_panic("net read: offset out of range");
    if (off == buf->len) return 0;
    ACQUIRE_OR(s, h, err, -1);
    int64_t r = zell__plat_sock_recv(s->fd, (char *)buf->data + off, buf->len - off);
    release_op(s);
    if (r == -ZELL_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    return r;
}

static int64_t write_bytes(int64_t h, const char *p, int64_t len, int64_t off, zell_error **err) {
    if (off < 0 || off > len) zell_panic("net write: offset out of range");
    if (off == len) return 0;
    ACQUIRE_OR(s, h, err, -1);
    int64_t r = zell__plat_sock_send(s->fd, p + off, len - off);
    release_op(s);
    if (r == -ZELL_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    return r;
}

int64_t zell_net_rt_write(int64_t h, const zell_slice *data, int64_t off, zell_error **err) {
    return write_bytes(h, data->data, data->len, off, err);
}

int64_t zell_net_rt_write_str(int64_t h, const zell_string *s, int64_t off, zell_error **err) {
    return write_bytes(h, zell_string_data(s), zell_string_len(s), off, err);
}

/* Gathered write of bufs ([][]byte), skipping the first `off` bytes. */
int64_t zell_net_rt_write_v(int64_t h, const zell_slice *bufs, int64_t off, zell_error **err) {
    zell__plat_iovec iov[64];
    int n = 0;
    int64_t skip = off;
    const zell_slice *v = bufs->data;
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
    int64_t r = zell__plat_sock_sendv(s->fd, iov, n);
    release_op(s);
    if (r == -ZELL_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    return r;
}

void zell_net_rt_shutdown(int64_t h, int64_t how, zell_error **err) {
    ACQUIRE_OR(s, h, err, );
    int e = zell__plat_sock_shutdown(s->fd, (int)how);
    release_op(s);
    if (e) set_code(err, e);
}

/* ---- files and sendfile ---- */

int64_t zell_net_rt_open_file(const zell_string *path, zell_error **err) {
    int64_t fd = zell__plat_file_open(zell_string_cstr(path));
    if (fd < 0) {
        if (err) *err = zell_error_errno("open", zell_string_cstr(path), (int)-fd);
        return -1;
    }
    return fd;
}

int64_t zell_net_rt_file_size(int64_t fd) {
    int64_t n = zell__plat_file_size(fd);
    return n < 0 ? 0 : n;
}

void zell_net_rt_close_file(int64_t fd) { zell__plat_file_close(fd); }

int64_t zell_net_rt_send_file(int64_t h, int64_t ffd, int64_t off, int64_t n, zell_error **err) {
    ACQUIRE_OR(s, h, err, -1);
    int64_t r = zell__plat_sendfile(s->fd, ffd, off, n);
    release_op(s);
    if (r == -ZELL_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    return r;
}

/* ---- waits ---- */

static zell_task *done_task(int64_t code) {
    zell_task *t = zell_task_new_plain(8, NULL);
    zell_task_complete(t, &code);
    return t;
}

static zell_task *wait_on(int64_t h, bool write, int64_t ms) {
    net_sock *s = sock_of(h);
    if (!acquire(s)) return done_task(2);
    zell_task *t = write ? zell_io_wait_writable(s->fd, ms > 0 ? ms : -1) : zell_io_wait_readable(s->fd, ms > 0 ? ms : -1);
    release_op(s);
    return t;
}

zell_task *zell_net_rt_wait_readable(int64_t h, int64_t ms) { return wait_on(h, false, ms); }
zell_task *zell_net_rt_wait_writable(int64_t h, int64_t ms) { return wait_on(h, true, ms); }

/* ---- options ---- */

void zell_net_rt_set_opt(int64_t h, int64_t opt, int64_t value, zell_error **err) {
    ACQUIRE_OR(s, h, err, );
    int e = zell__plat_sock_setopt(s->fd, (int)opt, value);
    release_op(s);
    if (e) set_code(err, e);
}

int64_t zell_net_rt_get_opt(int64_t h, int64_t opt, zell_error **err) {
    ACQUIRE_OR(s, h, err, 0);
    int64_t v = zell__plat_sock_getopt(s->fd, (int)opt);
    release_op(s);
    if (v < 0 && opt != ZELL_PLAT_OPT_LINGER) { set_code(err, v); return 0; }
    return v;
}

void zell_net_rt_mcast(int64_t h, bool join, const zell_string *group, const zell_string *ifname, zell_error **err) {
    unsigned char b[16];
    int len = zell__plat_parse_ip(zell_string_cstr(group), b);
    if (!len) { set_msg(err, "invalid multicast group"); return; }
    uint32_t idx = 0;
    if (zell_string_len(ifname) > 0) {
        idx = zell__plat_if_index(zell_string_cstr(ifname));
        if (!idx) { set_msg(err, "no such network interface"); return; }
    }
    ACQUIRE_OR(s, h, err, );
    int e = zell__plat_sock_mcast(s->fd, join, b, len, idx);
    release_op(s);
    if (e) set_code(err, e);
}

/* ---- UDP ---- */

int64_t zell_net_rt_udp(const zell_string *ip, int64_t port, int64_t flags, zell_error **err) {
    zell__plat_addr a;
    bool dual;
    if (!listen_addr(ip, port, &a, &dual)) {
        set_msg(err, "invalid address");
        return -1;
    }
    int64_t code = 0;
    /* UDP: no SO_REUSEADDR (on Linux it would let two sockets share a
     * unicast port without SO_REUSEPORT's load balancing) */
    int64_t fd = open_bound(&a, ZELL_PLAT_DGRAM, flags | LF_NO_REUSEADDR, dual, &code);
    if (fd < 0 && dual) {
        unsigned char zero[4] = { 0 };
        zell__plat_addr_ip(&a, zero, 4, port, 0);
        int64_t code4 = 0;
        fd = open_bound(&a, ZELL_PLAT_DGRAM, flags | LF_NO_REUSEADDR, false, &code4);
        if (fd < 0) code = code4;
    }
    if (fd < 0) { set_code(err, code); return -1; }
    return sock_new(fd, family_of(&a), ZELL_PLAT_DGRAM);
}

int64_t zell_net_rt_udp_dial(const zell_string *ip, int64_t port, const zell_string *lip, int64_t lport, zell_error **err) {
    zell__plat_addr a;
    if (!parse_ip_zone(zell_string_cstr(ip), port, &a)) {
        set_msg(err, "invalid address");
        return -1;
    }
    int fam = family_of(&a);
    int64_t fd = zell__plat_sock_open(fam, ZELL_PLAT_DGRAM);
    if (fd < 0) { set_code(err, fd); return -1; }
    int64_t rc = bind_local(fd, lip, lport, fam);
    if (rc >= 0) rc = zell__plat_sock_connect(fd, &a);
    if (rc < 0) {
        zell__plat_sock_close(fd);
        if (rc == -22) set_msg(err, "local address does not match the remote address family");
        else set_code(err, rc);
        return -1;
    }
    int64_t h = sock_new(fd, fam, ZELL_PLAT_DGRAM);
    sock_of(h)->peer = a;
    sock_of(h)->has_peer = 1;
    return h;
}

static void note_from(const zell__plat_addr *from, bool trunc) {
    t_from = *from;
    t_trunc = trunc;
}

void zell_net_rt_recv_from(int64_t h, int64_t n, zell_slice *out, zell_error **err) {
    memset(out, 0, sizeof *out);
    t_again = false;
    memset(&t_from, 0, sizeof t_from);
    t_trunc = false;
    if (n <= 0) return;
    ACQUIRE_OR(s, h, err, );
    bool small = n <= SCRATCH;
    zell_slice buf;
    memset(&buf, 0, sizeof buf);
    if (!small) zell_slice_new(&buf, 1, n, n);
    void *dst = small ? (void *)scratch() : buf.data;
    zell__plat_addr from;
    bool trunc = false;
    int64_t r = zell__plat_sock_recvfrom(s->fd, dst, n, &from, &trunc);
    release_op(s);
    if (r < 0) {
        zell_slice_release(&buf);
        if (r == -ZELL_PLAT_EAGAIN) t_again = true;
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

int64_t zell_net_rt_read_from(int64_t h, const zell_slice *buf, zell_error **err) {
    memset(&t_from, 0, sizeof t_from);
    t_trunc = false;
    ACQUIRE_OR(s, h, err, -1);
    zell__plat_addr from;
    bool trunc = false;
    int64_t r = zell__plat_sock_recvfrom(s->fd, buf->data, buf->len, &from, &trunc);
    release_op(s);
    if (r == -ZELL_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    note_from(&from, trunc);
    return r;
}

zell_string *zell_net_rt_last_from(void) { return addr_string(&t_from); }

int64_t zell_net_rt_last_from_port(void) {
    unsigned char b[16];
    int len;
    int64_t port;
    uint32_t scope;
    zell__plat_addr_parts(&t_from, b, &len, &port, &scope);
    return port;
}

bool zell_net_rt_last_trunc(void) { return t_trunc; }

int64_t zell_net_rt_send_to(int64_t h, const zell_slice *data, const zell_string *ip, int64_t port, zell_error **err) {
    zell__plat_addr a, *to = NULL;
    if (zell_string_len(ip) > 0) {
        if (!parse_ip_zone(zell_string_cstr(ip), port, &a)) {
            set_msg(err, "invalid address");
            return -1;
        }
        to = &a;
    }
    ACQUIRE_OR(s, h, err, -1);
    if (to && s->family == ZELL_PLAT_AF_INET6 && family_of(to) == ZELL_PLAT_AF_INET) {
        /* dual-stack socket: send to the IPv4-mapped address */
        unsigned char b[16] = { 0 }, v4[16];
        int len;
        int64_t p;
        uint32_t sc;
        zell__plat_addr_parts(to, v4, &len, &p, &sc);
        b[10] = b[11] = 0xff;
        memcpy(b + 12, v4, 4);
        zell__plat_addr_ip(&a, b, 16, p, 0);
    }
    int64_t r = zell__plat_sock_sendto(s->fd, data->data, data->len, to);
    release_op(s);
    if (r == -ZELL_PLAT_EAGAIN) return -1;
    if (r < 0) { set_code(err, r); return -1; }
    return r;
}

/* ---- interfaces ---- */

void zell_net_rt_interfaces(zell_slice *out, zell_error **err) {
    memset(out, 0, sizeof *out);
    char **lines;
    int n;
    int e = zell__plat_interfaces(&lines, &n);
    if (e) { set_code(err, e); return; }
    zell_slice res;
    zell_slice_new_ops(&res, 8, 0, n, &zell_ops_ref);
    for (int i = 0; i < n; i++) {
        zell_string *s = zell_string_from_cstr(lines[i]);
        zell_slice_append(&res, 8, &zell_ops_ref, &s);
        zell_release(s);
        free(lines[i]);
    }
    free(lines);
    *out = res;
}

int64_t zell_net_rt_lookup_port(const zell_string *network, const zell_string *service) {
    return zell__plat_lookup_port(zell_string_cstr(network), zell_string_cstr(service));
}

/* ---- name resolution: getaddrinfo on a small resolver pool ----
 * Lookups run on up to ZELL_RESOLVERS (default 4) detached threads that
 * start on demand and retire after 30 s without work, so a slow DNS server
 * never blocks a scheduler worker. A lookup's task completes with a handle
 * that rtResolveTake turns into the result. zell__net_stop (shutdown)
 * drops queued lookups and makes running ones discard their result. */

typedef struct resolve_req {
    int kind;               /* 0 = forward, 1 = reverse */
    int flags;
    char *host;
    zell_task *task;       /* owned until completed */
    char **addrs;
    int n;
    const char *errmsg;
    bool notfound;
    uint64_t gen;
    struct resolve_req *next;
} resolve_req;

static zell__plat_mutex rs_mu = ZELL_PLAT_MUTEX_INIT;
static zell__plat_cond rs_cv = ZELL_PLAT_COND_INIT;
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
    if (r->kind == 0) r->errmsg = zell__plat_resolve(r->host, r->flags, &r->addrs, &r->n, &r->notfound);
    else r->errmsg = zell__plat_reverse(r->host, &r->addrs, &r->n, &r->notfound);
}

static void *resolver_main(void *arg) {
    uint64_t gen = (uint64_t)(uintptr_t)arg;
    zell__plat_mutex_lock(&rs_mu);
    for (;;) {
        if (rs_gen != gen) break;
        resolve_req *r = rs_head;
        if (!r) {
            rs_idle++;
            bool woke = zell__plat_cond_wait_ns(&rs_cv, &rs_mu, (int64_t)30 * 1000000000);
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
        zell__plat_mutex_unlock(&rs_mu);
        req_run(r);
        zell__plat_mutex_lock(&rs_mu);
        if (r->gen != rs_gen) {
            /* shut down meanwhile: nobody runs continuations any more */
            zell_release(r->task);
            req_free(r);
            continue;
        }
        zell__plat_mutex_unlock(&rs_mu);
        int64_t hnd = (int64_t)(intptr_t)r;
        zell_task *t = r->task;
        r->task = NULL;
        zell_task_complete(t, &hnd);
        zell_release(t);
        zell__plat_mutex_lock(&rs_mu);
    }
    zell__plat_mutex_unlock(&rs_mu);
    return NULL;
}

static int resolver_max(void) {
    const char *env = getenv("ZELL_RESOLVERS");
    long n = env ? strtol(env, NULL, 10) : 0;
    if (n <= 0) n = 4;
    if (n > 64) n = 64;
    return (int)n;
}

static zell_task *submit(int kind, const zell_string *host, int64_t flags) {
    zell_task *t = zell_task_new_plain(8, NULL);
    resolve_req *r = calloc(1, sizeof *r);
    if (!r) zell_panic("out of memory (resolve)");
    r->kind = kind;
    r->flags = (int)flags;
    r->host = strdup(zell_string_cstr(host));
    if (!r->host) zell_panic("out of memory (resolve)");
    if (kind == 0) {
        unsigned char b[16];
        char ip[96];
        snprintf(ip, sizeof ip, "%s", r->host);
        char *pct = strchr(ip, '%');
        if (pct) *pct = 0;
        if (zell__plat_parse_ip(ip, b)) {  /* a literal resolves to itself, at once */
            r->addrs = calloc(2, sizeof *r->addrs);
            if (!r->addrs) zell_panic("out of memory (resolve)");
            r->addrs[0] = strdup(r->host);
            r->n = 1;
            int64_t h = (int64_t)(intptr_t)r;
            zell_task_complete(t, &h);
            return t;
        }
    }
    zell_retain(t);
    r->task = t;
    zell__plat_mutex_lock(&rs_mu);
    r->gen = rs_gen;
    if (rs_tail) rs_tail->next = r; else rs_head = r;
    rs_tail = r;
    if (rs_idle == 0 && rs_threads < resolver_max()) {
        zell__net_stop_fn = zell__net_stop;  /* shutdown stops the pool */
        if (zell__plat_thread_start_detached(resolver_main, (void *)(uintptr_t)rs_gen)) rs_threads++;
        else if (rs_threads == 0) {
            /* no thread at all: fail the lookup now */
            rs_head = rs_tail = NULL;
            zell__plat_mutex_unlock(&rs_mu);
            r->errmsg = "cannot start a resolver thread";
            r->task = NULL;
            int64_t h = (int64_t)(intptr_t)r;
            zell_task_complete(t, &h);
            zell_release(t);
            return t;
        }
    }
    zell__plat_cond_broadcast(&rs_cv);
    zell__plat_mutex_unlock(&rs_mu);
    return t;
}

zell_task *zell_net_rt_resolve(const zell_string *host, int64_t flags) { return submit(0, host, flags); }
zell_task *zell_net_rt_reverse(const zell_string *ip) { return submit(1, ip, 0); }

static void take_result(resolve_req *r, const char *op, zell_slice *out, zell_error **err) {
    memset(out, 0, sizeof *out);
    if (r->errmsg) {
        char msg[512];
        snprintf(msg, sizeof msg, "%s %s", op, r->host);
        zell_string *ctx = zell_string_from_cstr(msg);
        zell_error *inner = r->notfound ? &not_found_err : zell_error_new_cstr(r->errmsg);
        *err = zell_error_wrap(inner, ctx);
        zell_release(inner);
        zell_release(ctx);
    } else {
        zell_slice res;
        zell_slice_new_ops(&res, 8, 0, r->n, &zell_ops_ref);
        for (int i = 0; i < r->n; i++) {
            zell_string *s = zell_string_from_cstr(r->addrs[i]);
            zell_slice_append(&res, 8, &zell_ops_ref, &s);
            zell_release(s);
        }
        *out = res;
    }
    req_free(r);
}

void zell_net_rt_resolve_take(int64_t handle, zell_slice *out, zell_error **err) {
    resolve_req *r = (resolve_req *)(intptr_t)handle;
    if (!r) zell_panic_nil();
    take_result(r, "lookup", out, err);
}

/* Blocking lookup on the calling thread (Listen with a host name). */
void zell_net_rt_resolve_sync(const zell_string *host, int64_t flags, zell_slice *out, zell_error **err) {
    resolve_req *r = calloc(1, sizeof *r);
    if (!r) zell_panic("out of memory (resolve)");
    r->host = strdup(zell_string_cstr(host));
    if (!r->host) zell_panic("out of memory (resolve)");
    r->flags = (int)flags;
    req_run(r);
    take_result(r, "lookup", out, err);
}

void zell__net_stop(void) {
    zell__plat_mutex_lock(&rs_mu);
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
    zell__plat_mutex_unlock(&rs_mu);
    while (r) {
        resolve_req *n = r->next;
        zell_release(r->task);
        req_free(r);
        r = n;
    }
}

/* ---- dialing helpers ---- */

/* Task whose int64 result is the index of the first handle to become
 * writable (or whose wait timed out after ms > 0, or was cancelled); the
 * caller then checks every attempt with rtConnectCheck. */
zell_task *zell_net_rt_wait_any_writable(const zell_slice *hs, int64_t ms) {
    int64_t n = hs->len;
    if (n <= 0) zell_panic("std.net: wait on no sockets");
    zell_task **ws = malloc(sizeof *ws * (size_t)n);
    if (!ws) zell_panic("out of memory (dial)");
    const int64_t *h = hs->data;
    for (int64_t i = 0; i < n; i++) ws[i] = wait_on(h[i], true, ms);
    zell_task *any = zell_task_any(ws, n);
    for (int64_t i = 0; i < n; i++) zell_release(ws[i]);
    free(ws);
    return any;
}

/* 1 connected, 0 still connecting, or the connect error. */
int64_t zell_net_rt_connect_check(int64_t h, zell_error **err) {
    ACQUIRE_OR(s, h, err, 0);
    int e = zell__plat_sock_error(s->fd);
    zell__plat_addr a;
    bool connected = !e && zell__plat_sock_name(s->fd, true, &a);
    /* A TCP self-connect (the kernel picked the dialled loopback port as
     * the ephemeral source port: simultaneous open with itself) succeeds
     * although nothing listens there. Like Go, treat it as "connection
     * refused" (a listener's port is never chosen as a source port). */
    bool self = false;
    zell__plat_addr la;
    if (connected && zell__plat_addr_family(&a) != ZELL_PLAT_AF_UNIX && zell__plat_sock_name(s->fd, false, &la)) {
        unsigned char ip1[16], ip2[16];
        int n1 = 0, n2 = 0;
        int64_t p1 = 0, p2 = 0;
        uint32_t z1 = 0, z2 = 0;
        zell__plat_addr_parts(&a, ip1, &n1, &p1, &z1);
        zell__plat_addr_parts(&la, ip2, &n2, &p2, &z2);
        self = n1 > 0 && n1 == n2 && p1 == p2 && memcmp(ip1, ip2, (size_t)n1) == 0;
    }
    release_op(s);
    if (e) { set_code(err, e); return 0; }
    if (self) { if (err) *err = &refused_err; return 0; }
    return connected ? 1 : 0;
}

/* ---- byte helpers for net.zl ---- */

void zell_net_rt_bytes(int64_t n, zell_slice *out) {
    memset(out, 0, sizeof *out);
    if (n > 0) zell_slice_new(out, 1, n, n);
}

/* Copies min(n, len(dst) - doff, len(src) - soff) bytes; returns the count. */
int64_t zell_net_rt_copy_bytes(const zell_slice *dst, int64_t doff, const zell_slice *src, int64_t soff, int64_t n) {
    if (doff < 0 || soff < 0 || doff > dst->len || soff > src->len) zell_panic("net: copy out of range");
    int64_t k = n;
    if (k > dst->len - doff) k = dst->len - doff;
    if (k > src->len - soff) k = src->len - soff;
    if (k <= 0) return 0;
    memmove((char *)dst->data + doff, (const char *)src->data + soff, (size_t)k);
    return k;
}

/* Index of byte c in b at or after `from`, or -1. */
int64_t zell_net_rt_index_byte(const zell_slice *b, int64_t from, int64_t c) {
    if (from < 0) from = 0;
    if (from >= b->len) return -1;
    const char *p = memchr((const char *)b->data + from, (int)(c & 0xff), (size_t)(b->len - from));
    return p ? (int64_t)(p - (const char *)b->data) : -1;
}

/* a ++ b[..bn] as a new slice. */
void zell_net_rt_join(const zell_slice *a, const zell_slice *b, int64_t bn, zell_slice *out) {
    if (bn > b->len) bn = b->len;
    if (bn < 0) bn = 0;
    int64_t n = a->len + bn;
    memset(out, 0, sizeof *out);
    if (n == 0) return;
    zell_slice_new(out, 1, n, n + n / 2);
    out->len = n;
    if (a->len) memcpy(out->data, a->data, (size_t)a->len);
    if (bn) memcpy((char *)out->data + a->len, b->data, (size_t)bn);
}
