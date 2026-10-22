/* net_posix.c - BSD sockets for the platform layer (platform.h): non-blocking
 * TCP, UDP and Unix domain sockets, socket options, sendfile, getaddrinfo
 * name resolution and interface enumeration, plus process setup (SIGPIPE,
 * open-files limit). */
#include "platform.h"

#if defined(__unix__) || defined(__APPLE__)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/sendfile.h>
#endif

_Static_assert(sizeof(struct sockaddr_storage) <= sizeof(((zell__plat_addr *)0)->u.bytes), "sockaddr_storage fits");

static int64_t neg(int e) {
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS) return -ZELL_PLAT_EAGAIN;
    return -(int64_t)(e ? e : EIO);
}

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

/* ---- process setup ---------------------------------------------------- */

void zell__plat_process_init(void) {
    struct sigaction sa;
    if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) {
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPIPE, &sa, NULL);
    }
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rlim_t want = rl.rlim_max;
#if defined(__APPLE__)
        if (want > (rlim_t)10240) want = 10240;  /* OPEN_MAX: setrlimit fails above it */
#endif
        if (want > rl.rlim_cur) {
            rl.rlim_cur = want;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
    }
}

/* ---- errors ----------------------------------------------------------- */

int zell__plat_err_class(int code) {
    switch (code) {
    case ECONNREFUSED: return ZELL_PLAT_E_REFUSED;
    case ECONNRESET: case EPIPE: case ECONNABORTED: return ZELL_PLAT_E_RESET;
    case ENETUNREACH: case EHOSTUNREACH: case ENETDOWN:
#ifdef EHOSTDOWN
    case EHOSTDOWN:
#endif
        return ZELL_PLAT_E_UNREACHABLE;
    case EADDRINUSE: return ZELL_PLAT_E_ADDRINUSE;
    case ETIMEDOUT: return ZELL_PLAT_E_TIMEOUT;
    case EBADF: return ZELL_PLAT_E_CLOSED;
    case EINVAL: return ZELL_PLAT_E_INVAL;
    default: return ZELL_PLAT_E_OTHER;
    }
}

const char *zell__plat_strerror(int code, char *buf, size_t cap) {
    if (code == ZELL_PLAT_EAGAIN) code = EAGAIN;
    if (code < 0) code = -code;
    char tmp[256];
    const char *m = NULL;
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    m = strerror_r(code, tmp, sizeof tmp);
#else
    if (strerror_r(code, tmp, sizeof tmp) == 0) m = tmp;
#endif
    if (!m) m = "unknown error";
    snprintf(buf, cap, "%s", m);
    /* Go-style lower case ("connection refused"), but keep acronyms. */
    if (buf[0] >= 'A' && buf[0] <= 'Z' && !(buf[1] >= 'A' && buf[1] <= 'Z')) buf[0] = (char)(buf[0] - 'A' + 'a');
    return buf;
}

/* ---- addresses --------------------------------------------------------- */

bool zell__plat_addr_ip(zell__plat_addr *a, const unsigned char *ip, int iplen, int64_t port, uint32_t scope) {
    memset(a, 0, sizeof *a);
    if (port < 0 || port > 65535) return false;
    if (iplen == 4) {
        struct sockaddr_in *s = (struct sockaddr_in *)a->u.bytes;
        s->sin_family = AF_INET;
        s->sin_port = htons((uint16_t)port);
        memcpy(&s->sin_addr, ip, 4);
#if defined(SIN6_LEN)  /* BSD-derived stacks have sa_len fields */
        s->sin_len = sizeof *s;
#endif
        a->len = sizeof *s;
        return true;
    }
    if (iplen == 16) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)a->u.bytes;
        s->sin6_family = AF_INET6;
        s->sin6_port = htons((uint16_t)port);
        memcpy(&s->sin6_addr, ip, 16);
        s->sin6_scope_id = scope;
#if defined(SIN6_LEN)  /* BSD-derived stacks have sa_len fields */
        s->sin6_len = sizeof *s;
#endif
        a->len = sizeof *s;
        return true;
    }
    return false;
}

bool zell__plat_addr_unix(zell__plat_addr *a, const char *path) {
    memset(a, 0, sizeof *a);
    struct sockaddr_un *s = (struct sockaddr_un *)a->u.bytes;
    size_t n = strlen(path);
    s->sun_family = AF_UNIX;
    if (path[0] == '@') {
#if defined(__linux__)
        if (n > sizeof s->sun_path) return false;
        s->sun_path[0] = 0;  /* abstract namespace: no file, name without a NUL */
        memcpy(s->sun_path + 1, path + 1, n - 1);
        a->len = (uint32_t)(offsetof(struct sockaddr_un, sun_path) + n);
        return true;
#else
        return false;
#endif
    }
    if (n == 0 || n >= sizeof s->sun_path) return false;
    memcpy(s->sun_path, path, n + 1);
    a->len = (uint32_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
#if defined(SIN6_LEN)  /* BSD-derived stacks have sa_len fields */
    s->sun_len = (unsigned char)a->len;
#endif
    return true;
}

int zell__plat_addr_family(const zell__plat_addr *a) {
    switch (((const struct sockaddr *)a->u.bytes)->sa_family) {
    case AF_INET: return ZELL_PLAT_AF_INET;
    case AF_INET6: return ZELL_PLAT_AF_INET6;
    case AF_UNIX: return ZELL_PLAT_AF_UNIX;
    default: return 0;
    }
}

void zell__plat_addr_parts(const zell__plat_addr *a, unsigned char ip[16], int *iplen, int64_t *port, uint32_t *scope) {
    *iplen = 0;
    *port = 0;
    *scope = 0;
    const struct sockaddr *sa = (const struct sockaddr *)a->u.bytes;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)a->u.bytes;
        memcpy(ip, &s->sin_addr, 4);
        *iplen = 4;
        *port = ntohs(s->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)a->u.bytes;
        memcpy(ip, &s->sin6_addr, 16);
        *iplen = 16;
        *port = ntohs(s->sin6_port);
        *scope = s->sin6_scope_id;
    }
}

void zell__plat_addr_format(const zell__plat_addr *a, char *buf, size_t cap) {
    buf[0] = 0;
    int fam = zell__plat_addr_family(a);
    if (fam == ZELL_PLAT_AF_UNIX) {
        const struct sockaddr_un *s = (const struct sockaddr_un *)a->u.bytes;
        size_t off = offsetof(struct sockaddr_un, sun_path);
        if (a->len <= off) return;  /* unnamed */
        size_t n = a->len - off;
        if (s->sun_path[0] == 0) {  /* abstract */
            snprintf(buf, cap, "@%.*s", (int)(n - 1), s->sun_path + 1);
        } else {
            snprintf(buf, cap, "%.*s", (int)strnlen(s->sun_path, n), s->sun_path);
        }
        return;
    }
    unsigned char ip[16];
    int len;
    int64_t port;
    uint32_t scope;
    zell__plat_addr_parts(a, ip, &len, &port, &scope);
    char t[INET6_ADDRSTRLEN];
    if (!zell__plat_format_ip(ip, len, t, sizeof t)) return;
    if (len == 16) {
        char z[IF_NAMESIZE + 1] = "";
        if (scope && !zell__plat_if_name(scope, z, sizeof z)) snprintf(z, sizeof z, "%u", scope);
        snprintf(buf, cap, "[%s%s%s]:%lld", t, scope ? "%" : "", z, (long long)port);
    } else {
        snprintf(buf, cap, "%s:%lld", t, (long long)port);
    }
}

int zell__plat_parse_ip(const char *s, unsigned char out[16]) {
    if (inet_pton(AF_INET, s, out) == 1) return 4;
    if (inet_pton(AF_INET6, s, out) == 1) return 16;
    return 0;
}

bool zell__plat_format_ip(const unsigned char *ip, int len, char *buf, size_t cap) {
    if (len == 4) return inet_ntop(AF_INET, ip, buf, (socklen_t)cap) != NULL;
    if (len == 16) return inet_ntop(AF_INET6, ip, buf, (socklen_t)cap) != NULL;
    return false;
}

uint32_t zell__plat_if_index(const char *name) { return (uint32_t)if_nametoindex(name); }

bool zell__plat_if_name(uint32_t index, char *buf, size_t cap) {
    char n[IF_NAMESIZE + 1];
    if (!if_indextoname(index, n)) return false;
    snprintf(buf, cap, "%s", n);
    return true;
}

bool zell__plat_is_ip(const char *s) {
    unsigned char b[16];
    return zell__plat_parse_ip(s, b) != 0;
}

/* ---- sockets ----------------------------------------------------------- */

static int setup_fd(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) return errno;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return 0;
}

int64_t zell__plat_sock_open(int family, int type) {
    int af = family == ZELL_PLAT_AF_INET ? AF_INET : family == ZELL_PLAT_AF_INET6 ? AF_INET6 : family == ZELL_PLAT_AF_UNIX ? AF_UNIX : -1;
    int st = type == ZELL_PLAT_STREAM ? SOCK_STREAM : type == ZELL_PLAT_DGRAM ? SOCK_DGRAM : -1;
    if (af < 0 || st < 0) return -EINVAL;
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    int fd = socket(af, st | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return neg(errno);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#else
    int fd = socket(af, st, 0);
    if (fd < 0) return neg(errno);
    int e = setup_fd(fd);
    if (e) { close(fd); return neg(e); }
#endif
    (void)setup_fd;
    return fd;
}

int64_t zell__plat_sock_connect(int64_t fd, const zell__plat_addr *a) {
    if (connect((int)fd, (const struct sockaddr *)a->u.bytes, a->len) == 0) return 0;
    /* EINTR: the connect goes on asynchronously, like EINPROGRESS. */
    if (errno == EINTR) return -ZELL_PLAT_EAGAIN;
    return neg(errno);
}

int zell__plat_sock_error(int64_t fd) {
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt((int)fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return errno;
    return err;
}

int64_t zell__plat_sock_bind(int64_t fd, const zell__plat_addr *a) {
    if (bind((int)fd, (const struct sockaddr *)a->u.bytes, a->len) != 0) return neg(errno);
    return 0;
}

int64_t zell__plat_sock_listen(int64_t fd, int backlog) {
    if (listen((int)fd, backlog) != 0) return neg(errno);
    return 0;
}

int64_t zell__plat_sock_accept(int64_t lfd, zell__plat_addr *from) {
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t len = sizeof ss;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
        int fd = accept4((int)lfd, (struct sockaddr *)&ss, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) {
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        }
#else
        int fd = accept((int)lfd, (struct sockaddr *)&ss, &len);
        if (fd >= 0) {
            int e = setup_fd(fd);
            if (e) { close(fd); return neg(e); }
        }
#endif
        if (fd < 0) {
            /* A connection that died in the queue is not the listener's failure. */
            if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO) continue;
            return neg(errno);
        }
        if (from) {
            memset(from, 0, sizeof *from);
            memcpy(from->u.bytes, &ss, len < sizeof ss ? len : sizeof ss);
            from->len = len;
        }
        return fd;
    }
}

int64_t zell__plat_sock_recv(int64_t fd, void *buf, int64_t n) {
    for (;;) {
        ssize_t r = recv((int)fd, buf, (size_t)n, 0);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return neg(errno);
    }
}

int64_t zell__plat_sock_send(int64_t fd, const void *buf, int64_t n) {
    for (;;) {
        ssize_t r = send((int)fd, buf, (size_t)n, SEND_FLAGS);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return neg(errno);
    }
}

int64_t zell__plat_sock_sendv(int64_t fd, const zell__plat_iovec *iov, int n) {
    struct iovec v[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) {
        v[i].iov_base = (void *)iov[i].base;
        v[i].iov_len = iov[i].len;
    }
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = v;
    m.msg_iovlen = n;
    for (;;) {
        ssize_t r = sendmsg((int)fd, &m, SEND_FLAGS);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return neg(errno);
    }
}

int64_t zell__plat_sock_recvfrom(int64_t fd, void *buf, int64_t n, zell__plat_addr *from, bool *truncated) {
    struct sockaddr_storage ss;
    struct iovec v = { buf, (size_t)n };
    struct msghdr m;
    for (;;) {
        memset(&m, 0, sizeof m);
        m.msg_name = &ss;
        m.msg_namelen = sizeof ss;
        m.msg_iov = &v;
        m.msg_iovlen = 1;
        ssize_t r = recvmsg((int)fd, &m, 0);
        if (r >= 0) {
            if (truncated) *truncated = (m.msg_flags & MSG_TRUNC) != 0;
            if (from) {
                memset(from, 0, sizeof *from);
                memcpy(from->u.bytes, &ss, m.msg_namelen < sizeof ss ? m.msg_namelen : sizeof ss);
                from->len = m.msg_namelen;
            }
            return r;
        }
        if (errno == EINTR) continue;
        return neg(errno);
    }
}

int64_t zell__plat_sock_sendto(int64_t fd, const void *buf, int64_t n, const zell__plat_addr *to) {
    for (;;) {
        ssize_t r = to ? sendto((int)fd, buf, (size_t)n, SEND_FLAGS, (const struct sockaddr *)to->u.bytes, to->len)
                       : send((int)fd, buf, (size_t)n, SEND_FLAGS);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return neg(errno);
    }
}

int zell__plat_sock_shutdown(int64_t fd, int how) {
    int h = how == (ZELL_PLAT_READ | ZELL_PLAT_WRITE) ? SHUT_RDWR : how == ZELL_PLAT_READ ? SHUT_RD : SHUT_WR;
    return shutdown((int)fd, h) == 0 ? 0 : errno;
}

void zell__plat_sock_close(int64_t fd) {
    if (fd < 0) return;
    /* close(2) releases the descriptor even when it reports EINTR (Linux,
     * the BSDs): never retry, the number may already be reused. */
    close((int)fd);
}

bool zell__plat_sock_name(int64_t fd, bool peer, zell__plat_addr *a) {
    memset(a, 0, sizeof *a);
    socklen_t len = sizeof a->u.bytes;
    int rc = peer ? getpeername((int)fd, (struct sockaddr *)a->u.bytes, &len) : getsockname((int)fd, (struct sockaddr *)a->u.bytes, &len);
    if (rc != 0) return false;
    a->len = len;
    return true;
}

/* ---- options ------------------------------------------------------------ */

static int sock_family(int fd) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) return -1;
    return ss.ss_family;
}

static bool opt_of(int fd, int opt, int *level, int *name) {
    switch (opt) {
    case ZELL_PLAT_OPT_NODELAY: *level = IPPROTO_TCP; *name = TCP_NODELAY; return true;
    case ZELL_PLAT_OPT_KEEPALIVE: *level = SOL_SOCKET; *name = SO_KEEPALIVE; return true;
#if defined(TCP_KEEPIDLE)
    case ZELL_PLAT_OPT_KEEPIDLE: *level = IPPROTO_TCP; *name = TCP_KEEPIDLE; return true;
#elif defined(TCP_KEEPALIVE)
    case ZELL_PLAT_OPT_KEEPIDLE: *level = IPPROTO_TCP; *name = TCP_KEEPALIVE; return true;  /* macOS */
#endif
#if defined(TCP_KEEPINTVL)
    case ZELL_PLAT_OPT_KEEPINTVL: *level = IPPROTO_TCP; *name = TCP_KEEPINTVL; return true;
#endif
#if defined(TCP_KEEPCNT)
    case ZELL_PLAT_OPT_KEEPCNT: *level = IPPROTO_TCP; *name = TCP_KEEPCNT; return true;
#endif
    case ZELL_PLAT_OPT_RCVBUF: *level = SOL_SOCKET; *name = SO_RCVBUF; return true;
    case ZELL_PLAT_OPT_SNDBUF: *level = SOL_SOCKET; *name = SO_SNDBUF; return true;
    case ZELL_PLAT_OPT_REUSEADDR: *level = SOL_SOCKET; *name = SO_REUSEADDR; return true;
#if defined(SO_REUSEPORT)
    case ZELL_PLAT_OPT_REUSEPORT: *level = SOL_SOCKET; *name = SO_REUSEPORT; return true;
#endif
    case ZELL_PLAT_OPT_BROADCAST: *level = SOL_SOCKET; *name = SO_BROADCAST; return true;
#if defined(TCP_CORK)
    case ZELL_PLAT_OPT_CORK: *level = IPPROTO_TCP; *name = TCP_CORK; return true;
#elif defined(TCP_NOPUSH)
    case ZELL_PLAT_OPT_CORK: *level = IPPROTO_TCP; *name = TCP_NOPUSH; return true;
#endif
    case ZELL_PLAT_OPT_V6ONLY: *level = IPPROTO_IPV6; *name = IPV6_V6ONLY; return true;
#if defined(TCP_FASTOPEN)
    case ZELL_PLAT_OPT_FASTOPEN: *level = IPPROTO_TCP; *name = TCP_FASTOPEN; return true;
#endif
    case ZELL_PLAT_OPT_MCAST_LOOP:
        if (sock_family(fd) == AF_INET6) { *level = IPPROTO_IPV6; *name = IPV6_MULTICAST_LOOP; }
        else { *level = IPPROTO_IP; *name = IP_MULTICAST_LOOP; }
        return true;
    case ZELL_PLAT_OPT_MCAST_TTL:
        if (sock_family(fd) == AF_INET6) { *level = IPPROTO_IPV6; *name = IPV6_MULTICAST_HOPS; }
        else { *level = IPPROTO_IP; *name = IP_MULTICAST_TTL; }
        return true;
    default: return false;
    }
}

int zell__plat_sock_setopt(int64_t fd, int opt, int64_t value) {
    if (opt == ZELL_PLAT_OPT_MCAST_IF) {
        if (sock_family((int)fd) == AF_INET6) {
            unsigned idx = (unsigned)value;
            return setsockopt((int)fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof idx) == 0 ? 0 : errno;
        }
#if defined(__linux__)
        struct ip_mreqn m;
        memset(&m, 0, sizeof m);
        m.imr_ifindex = (int)value;
        return setsockopt((int)fd, IPPROTO_IP, IP_MULTICAST_IF, &m, sizeof m) == 0 ? 0 : errno;
#else
        /* BSD/macOS take the interface's IPv4 address */
        struct in_addr ia;
        ia.s_addr = htonl(INADDR_ANY);
        if (value > 0) {
            char name[IF_NAMESIZE + 1];
            struct ifaddrs *ifs = NULL, *i;
            if (!if_indextoname((unsigned)value, name) || getifaddrs(&ifs) != 0) return ENXIO;
            int found = 0;
            for (i = ifs; i; i = i->ifa_next)
                if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, name) == 0) {
                    ia = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
                    found = 1;
                    break;
                }
            freeifaddrs(ifs);
            if (!found) return EADDRNOTAVAIL;
        }
        return setsockopt((int)fd, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof ia) == 0 ? 0 : errno;
#endif
    }
    if (opt == ZELL_PLAT_OPT_LINGER) {
        struct linger l = { value >= 0, value >= 0 ? (int)value : 0 };
        return setsockopt((int)fd, SOL_SOCKET, SO_LINGER, &l, sizeof l) == 0 ? 0 : errno;
    }
    int level, name;
    if (!opt_of((int)fd, opt, &level, &name)) return ENOPROTOOPT;
    int v = value > 0x7fffffff ? 0x7fffffff : value < 0 ? 0 : (int)value;
#if !defined(__linux__)
    if (opt == ZELL_PLAT_OPT_MCAST_LOOP && level == IPPROTO_IP) {
        unsigned char c = (unsigned char)(v != 0);
        return setsockopt((int)fd, level, name, &c, sizeof c) == 0 ? 0 : errno;
    }
    if (opt == ZELL_PLAT_OPT_MCAST_TTL && level == IPPROTO_IP) {
        unsigned char c = (unsigned char)(v > 255 ? 255 : v);
        return setsockopt((int)fd, level, name, &c, sizeof c) == 0 ? 0 : errno;
    }
#endif
    return setsockopt((int)fd, level, name, &v, sizeof v) == 0 ? 0 : errno;
}

int64_t zell__plat_sock_getopt(int64_t fd, int opt) {
    if (opt == ZELL_PLAT_OPT_LINGER) {
        struct linger l;
        socklen_t len = sizeof l;
        if (getsockopt((int)fd, SOL_SOCKET, SO_LINGER, &l, &len) != 0) return -(int64_t)errno;
        return l.l_onoff ? l.l_linger : -1;
    }
    int level, name;
    if (!opt_of((int)fd, opt, &level, &name)) return -(int64_t)ENOPROTOOPT;
    int v = 0;
    socklen_t len = sizeof v;
    if (getsockopt((int)fd, level, name, &v, &len) != 0) return -(int64_t)errno;
    if (len == 1) v = *(unsigned char *)&v;
    return v;
}

int zell__plat_sock_mcast(int64_t fd, bool join, const unsigned char *group, int len, uint32_t ifindex) {
    if (len == 4) {
#if defined(__linux__)
        struct ip_mreqn m;
        memset(&m, 0, sizeof m);
        memcpy(&m.imr_multiaddr, group, 4);
        m.imr_ifindex = (int)ifindex;
#else
        struct ip_mreq m;
        memset(&m, 0, sizeof m);
        memcpy(&m.imr_multiaddr, group, 4);
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        (void)ifindex;
#endif
        int rc = setsockopt((int)fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &m, sizeof m);
        return rc == 0 ? 0 : errno;
    }
    if (len == 16) {
        struct ipv6_mreq m;
        memset(&m, 0, sizeof m);
        memcpy(&m.ipv6mr_multiaddr, group, 16);
        m.ipv6mr_interface = ifindex;
#if defined(IPV6_ADD_MEMBERSHIP)
        int name = join ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP;
#else
        int name = join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP;
#endif
        int rc = setsockopt((int)fd, IPPROTO_IPV6, name, &m, sizeof m);
        return rc == 0 ? 0 : errno;
    }
    return EINVAL;
}

/* ---- files and sendfile ------------------------------------------------- */

int64_t zell__plat_file_open(const char *path) {
    int fd;
    do fd = open(path, O_RDONLY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    return fd < 0 ? -(int64_t)errno : fd;
}

int64_t zell__plat_file_size(int64_t fd) {
    struct stat st;
    if (fstat((int)fd, &st) != 0) return -(int64_t)errno;
    return (int64_t)st.st_size;
}

void zell__plat_file_close(int64_t fd) {
    if (fd >= 0) close((int)fd);
}

int64_t zell__plat_sendfile(int64_t sock, int64_t fd, int64_t off, int64_t n) {
    if (n <= 0) return 0;
#if defined(__linux__)
    for (;;) {
        off_t o = (off_t)off;
        size_t cnt = n > 0x7ffff000 ? 0x7ffff000 : (size_t)n;
        ssize_t r = sendfile((int)sock, (int)fd, &o, cnt);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        if (errno == EINVAL || errno == ENOSYS) break;  /* not sendfile-able: copy */
        return neg(errno);
    }
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    for (;;) {
        off_t sent = 0;
        int r = sendfile((int)fd, (int)sock, (off_t)off, (size_t)n, NULL, &sent, 0);
        if (sent > 0) return (int64_t)sent;
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN) return -ZELL_PLAT_EAGAIN;
        if (errno == EINVAL || errno == ENOTSOCK || errno == EOPNOTSUPP) break;
        return neg(errno);
    }
#elif defined(__APPLE__)
    for (;;) {
        off_t len = (off_t)n;
        int r = sendfile((int)fd, (int)sock, (off_t)off, &len, NULL, 0);
        if (len > 0) return (int64_t)len;
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN) return -ZELL_PLAT_EAGAIN;
        if (errno == EINVAL || errno == ENOTSOCK || errno == EOPNOTSUPP) break;
        return neg(errno);
    }
#endif
    /* Portable fallback: pread a chunk and send what the socket takes. */
    char buf[65536];
    size_t want = n > (int64_t)sizeof buf ? sizeof buf : (size_t)n;
    ssize_t got;
    do got = pread((int)fd, buf, want, (off_t)off);
    while (got < 0 && errno == EINTR);
    if (got < 0) return neg(errno);
    if (got == 0) return 0;
    return zell__plat_sock_send(sock, buf, got);
}

void zell__plat_unlink(const char *path) {
    if (path && path[0] && path[0] != '@') unlink(path);
}

/* ---- name resolution ---------------------------------------------------- */

static bool addr_text(const struct sockaddr *sa, char *ip, size_t cap) {
    zell__plat_addr a;
    memset(&a, 0, sizeof a);
    size_t len = sa->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    memcpy(a.u.bytes, sa, len);
    a.len = (uint32_t)len;
    unsigned char b[16];
    int bl;
    int64_t port;
    uint32_t scope;
    zell__plat_addr_parts(&a, b, &bl, &port, &scope);
    char t[INET6_ADDRSTRLEN];
    if (!zell__plat_format_ip(b, bl, t, sizeof t)) return false;
    if (bl == 16 && scope) {
        char z[IF_NAMESIZE + 1];
        if (!zell__plat_if_name(scope, z, sizeof z)) snprintf(z, sizeof z, "%u", scope);
        snprintf(ip, cap, "%s%%%s", t, z);
    } else {
        snprintf(ip, cap, "%s", t);
    }
    return true;
}

static bool gai_notfound(int rc) {
    if (rc == EAI_NONAME) return true;
#ifdef EAI_NODATA
    if (rc == EAI_NODATA) return true;
#endif
#ifdef EAI_ADDRFAMILY
    if (rc == EAI_ADDRFAMILY) return true;
#endif
    return false;
}

const char *zell__plat_resolve(const char *host, int flags, char ***out, int *n, bool *notfound) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    if (flags & ZELL_PLAT_RESOLVE_PASSIVE) hints.ai_flags |= AI_PASSIVE;
    *out = NULL;
    *n = 0;
    *notfound = false;
    int rc;
    do rc = getaddrinfo(host, NULL, &hints, &res);
    while (rc == EAI_AGAIN && 0);
    if (rc == EAI_BADFLAGS || (rc != 0 && gai_notfound(rc))) {
        /* AI_ADDRCONFIG hides loopback-only hosts on machines without a
         * configured address: retry without it. */
        hints.ai_flags &= ~AI_ADDRCONFIG;
        if (res) freeaddrinfo(res);
        res = NULL;
        rc = getaddrinfo(host, NULL, &hints, &res);
    }
    if (rc != 0) {
        *notfound = gai_notfound(rc);
        if (*notfound) return "no such host";
#ifdef EAI_SYSTEM
        if (rc == EAI_SYSTEM) return "system error during lookup";
#endif
        return gai_strerror(rc);
    }
    int cap = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) cap++;
    char **v = calloc((size_t)cap + 1, sizeof *v);
    int k = 0;
    int passes = (flags & ZELL_PLAT_RESOLVE_V4FIRST) ? 2 : 1;
    for (int pass = 0; pass < passes && v; pass++) {
        for (struct addrinfo *a = res; a; a = a->ai_next) {
            if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
            if (passes == 2 && a->ai_family != (pass == 0 ? AF_INET : AF_INET6)) continue;
            char ip[INET6_ADDRSTRLEN + IF_NAMESIZE + 2];
            if (!addr_text(a->ai_addr, ip, sizeof ip)) continue;
            bool dup = false;
            for (int i = 0; i < k; i++) if (strcmp(v[i], ip) == 0) dup = true;
            if (dup) continue;
            v[k] = strdup(ip);
            if (v[k]) k++;
        }
    }
    freeaddrinfo(res);
    if (!v || k == 0) {
        free(v);
        *notfound = true;
        return "no such host";
    }
    *out = v;
    *n = k;
    return NULL;
}

const char *zell__plat_reverse(const char *ip, char ***out, int *n, bool *notfound) {
    *out = NULL;
    *n = 0;
    *notfound = false;
    unsigned char b[16];
    int len = zell__plat_parse_ip(ip, b);
    if (!len) return "not an IP address";
    zell__plat_addr a;
    zell__plat_addr_ip(&a, b, len, 0, 0);
    char host[NI_MAXHOST];
    int rc = getnameinfo((const struct sockaddr *)a.u.bytes, a.len, host, sizeof host, NULL, 0, NI_NAMEREQD);
    if (rc != 0) {
        *notfound = gai_notfound(rc);
        return *notfound ? "no such host" : gai_strerror(rc);
    }
    char **v = calloc(2, sizeof *v);
    if (!v) return "out of memory";
    v[0] = strdup(host);
    if (!v[0]) { free(v); return "out of memory"; }
    *out = v;
    *n = 1;
    return NULL;
}

int64_t zell__plat_lookup_port(const char *network, const char *service) {
    char *end;
    long p = strtol(service, &end, 10);
    if (*service && !*end) return p >= 0 && p <= 65535 ? p : -1;
    const char *proto = strncmp(network, "udp", 3) == 0 ? "udp" : "tcp";
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = proto[0] == 'u' ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    if (getaddrinfo(NULL, service, &hints, &res) != 0 || !res) return -1;
    int64_t port = ntohs(((struct sockaddr_in *)res->ai_addr)->sin_port);
    freeaddrinfo(res);
    return port;
}

static int prefix_len(const struct sockaddr *mask) {
    if (!mask) return -1;
    const unsigned char *b;
    int n;
    if (mask->sa_family == AF_INET) { b = (const unsigned char *)&((const struct sockaddr_in *)mask)->sin_addr; n = 4; }
    else if (mask->sa_family == AF_INET6) { b = (const unsigned char *)&((const struct sockaddr_in6 *)mask)->sin6_addr; n = 16; }
    else return -1;
    int bits = 0;
    for (int i = 0; i < n; i++)
        for (int k = 7; k >= 0; k--)
            if (b[i] & (1u << k)) bits++;
    return bits;
}

int zell__plat_interfaces(char ***out, int *n) {
    *out = NULL;
    *n = 0;
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return errno;
    int cap = 0, k = 0;
    char **v = NULL;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_name) continue;
        int found = -1;
        for (int j = 0; j < k; j++) {
            size_t nl = strlen(i->ifa_name);
            if (strncmp(v[j], i->ifa_name, nl) == 0 && v[j][nl] == '\t') found = j;
        }
        if (found < 0) {
            if (k == cap) {
                cap = cap ? cap * 2 : 8;
                char **nv = realloc(v, sizeof *v * (size_t)cap);
                if (!nv) break;
                v = nv;
            }
            char flags[8];
            int f = 0;
            if (i->ifa_flags & IFF_UP) flags[f++] = 'u';
            if (i->ifa_flags & IFF_LOOPBACK) flags[f++] = 'l';
            if (i->ifa_flags & IFF_MULTICAST) flags[f++] = 'm';
            if (i->ifa_flags & IFF_BROADCAST) flags[f++] = 'b';
            if (i->ifa_flags & IFF_POINTOPOINT) flags[f++] = 'p';
            flags[f] = 0;
            char line[256];
            snprintf(line, sizeof line, "%s\t%u\t0\t%s\t", i->ifa_name, (unsigned)if_nametoindex(i->ifa_name), flags);
            v[k] = strdup(line);
            if (!v[k]) break;
            found = k++;
        }
        if (i->ifa_addr && (i->ifa_addr->sa_family == AF_INET || i->ifa_addr->sa_family == AF_INET6)) {
            char ip[INET6_ADDRSTRLEN + IF_NAMESIZE + 2];
            if (!addr_text(i->ifa_addr, ip, sizeof ip)) continue;
            char *z = strchr(ip, '%');  /* the zone is the interface itself */
            if (z) *z = 0;
            size_t ol = strlen(v[found]);
            size_t add = strlen(ip) + 8;
            char *nl = realloc(v[found], ol + add);
            if (!nl) continue;
            v[found] = nl;
            snprintf(nl + ol, add, "%s%s/%d", nl[ol - 1] == '\t' ? "" : ",", ip, prefix_len(i->ifa_netmask));
        }
    }
    freeifaddrs(ifs);
    *out = v;
    *n = k;
    return 0;
}

#endif /* POSIX; Windows: net_win32.c */
