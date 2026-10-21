/* net_win32.c - Winsock2 for the platform layer (platform.h): non-blocking
 * TCP, UDP and AF_UNIX sockets (Windows 10 1803+), options, getaddrinfo
 * name resolution, GetAdaptersAddresses interface enumeration, files for
 * sendfile, and process setup (WSAStartup). Socket descriptors are SOCKET
 * values in int64_t; errno-style codes of socket calls are WSA error
 * numbers (WSAECONNREFUSED ...), which slang__plat_err_class and
 * slang__plat_strerror understand next to C runtime errno values (< 10000).
 *
 * Sockets are created with WSA_FLAG_OVERLAPPED (ready for the IOCP step,
 * docs/PLATFORMS.md) and WSA_FLAG_NO_HANDLE_INHERIT, and switched to
 * non-blocking mode with FIONBIO. sendfile is a read + send loop for now:
 * TransmitFile on a non-blocking socket needs an OVERLAPPED completion,
 * which comes with the IOCP poller. */
#include "platform.h"

#if defined(_WIN32)

#include "win32_util.h"

#include <afunix.h>
#include <errno.h>
#include <iphlpapi.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WSA_FLAG_NO_HANDLE_INHERIT
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#endif

_Static_assert(sizeof(SOCKADDR_STORAGE) <= sizeof(((slang__plat_addr *)0)->u.bytes), "sockaddr_storage fits");

static slang__plat_once g_wsa_once = SLANG_PLAT_ONCE_INIT;
static void wsa_start(void) {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
}
void slang__w32_wsa_init(void) { slang__plat_once_run(&g_wsa_once, wsa_start); }

static int64_t neg(int e) {
    if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY) return -SLANG_PLAT_EAGAIN;
    return -(int64_t)(e ? e : WSAEINVAL);
}
static int64_t neg_last(void) { return neg(WSAGetLastError()); }

/* ---- process setup ----------------------------------------------------------- */

void slang__plat_process_init(void) {
    slang__w32_wsa_init();
    _setmaxstdio(8192);  /* C runtime descriptors (files); there is no SIGPIPE */
}

/* ---- errors --------------------------------------------------------------------- */

int slang__plat_err_class(int code) {
    switch (code) {
    case WSAECONNREFUSED: return SLANG_PLAT_E_REFUSED;
    case WSAECONNRESET: case WSAECONNABORTED: case WSAESHUTDOWN: case EPIPE: return SLANG_PLAT_E_RESET;
    case WSAENETUNREACH: case WSAEHOSTUNREACH: case WSAENETDOWN: case WSAEHOSTDOWN: return SLANG_PLAT_E_UNREACHABLE;
    case WSAEADDRINUSE: return SLANG_PLAT_E_ADDRINUSE;
    case WSAETIMEDOUT: return SLANG_PLAT_E_TIMEOUT;
    case WSAENOTSOCK: case EBADF: return SLANG_PLAT_E_CLOSED;
    case WSAEINVAL: case EINVAL: return SLANG_PLAT_E_INVAL;
    default: return SLANG_PLAT_E_OTHER;
    }
}

/* Messages as the POSIX build prints them, so programs print the same. */
static const char *wsa_text(int code) {
    switch (code) {
    case WSAECONNREFUSED: return "connection refused";
    case WSAECONNRESET: return "connection reset by peer";
    case WSAECONNABORTED: return "software caused connection abort";
    case WSAESHUTDOWN: return "broken pipe";
    case WSAENETUNREACH: return "network is unreachable";
    case WSAEHOSTUNREACH: return "no route to host";
    case WSAENETDOWN: return "network is down";
    case WSAEHOSTDOWN: return "host is down";
    case WSAEADDRINUSE: return "address already in use";
    case WSAEADDRNOTAVAIL: return "cannot assign requested address";
    case WSAETIMEDOUT: return "connection timed out";
    case WSAENOTSOCK: return "bad file descriptor";
    case WSAEINVAL: return "invalid argument";
    case WSAEWOULDBLOCK: return "resource temporarily unavailable";
    case WSAEINPROGRESS: return "operation now in progress";
    case WSAEALREADY: return "operation already in progress";
    case WSAEISCONN: return "transport endpoint is already connected";
    case WSAENOTCONN: return "transport endpoint is not connected";
    case WSAEMSGSIZE: return "message too long";
    case WSAENOPROTOOPT: return "protocol not available";
    case WSAEAFNOSUPPORT: return "address family not supported by protocol";
    case WSAEPROTONOSUPPORT: return "protocol not supported";
    case WSAEOPNOTSUPP: return "operation not supported";
    case WSAEACCES: return "permission denied";
    case WSAEMFILE: return "too many open files";
    case WSAENOBUFS: return "no buffer space available";
    case WSAEFAULT: return "bad address";
    case WSAEINTR: return "interrupted system call";
    case WSANOTINITIALISED: return "winsock not initialised";
    default: return NULL;
    }
}

const char *slang__plat_strerror(int code, char *buf, size_t cap) {
    if (code < 0) code = -code;
    if (code == SLANG_PLAT_EAGAIN) code = WSAEWOULDBLOCK;
    const char *m = wsa_text(code);
    if (m) {
        snprintf(buf, cap, "%s", m);
        return buf;
    }
    if (code < 10000) {
        snprintf(buf, cap, "%s", strerror(code));
    } else {
        DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)code, 0, buf,
                                 (DWORD)cap, NULL);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == '.')) buf[--n] = 0;
        if (n == 0) snprintf(buf, cap, "error %d", code);
    }
    if (buf[0] >= 'A' && buf[0] <= 'Z' && !(buf[1] >= 'A' && buf[1] <= 'Z')) buf[0] = (char)(buf[0] - 'A' + 'a');
    return buf;
}

/* ---- addresses ---------------------------------------------------------------------- */

bool slang__plat_addr_ip(slang__plat_addr *a, const unsigned char *ip, int iplen, int64_t port, uint32_t scope) {
    memset(a, 0, sizeof *a);
    if (port < 0 || port > 65535) return false;
    if (iplen == 4) {
        struct sockaddr_in *s = (struct sockaddr_in *)a->u.bytes;
        s->sin_family = AF_INET;
        s->sin_port = htons((uint16_t)port);
        memcpy(&s->sin_addr, ip, 4);
        a->len = sizeof *s;
        return true;
    }
    if (iplen == 16) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)a->u.bytes;
        s->sin6_family = AF_INET6;
        s->sin6_port = htons((uint16_t)port);
        memcpy(&s->sin6_addr, ip, 16);
        s->sin6_scope_id = scope;
        a->len = sizeof *s;
        return true;
    }
    return false;
}

bool slang__plat_addr_unix(slang__plat_addr *a, const char *path) {
    memset(a, 0, sizeof *a);
    struct sockaddr_un *s = (struct sockaddr_un *)a->u.bytes;
    size_t n = strlen(path);
    if (path[0] == '@' || n == 0 || n >= sizeof s->sun_path) return false;  /* no abstract namespace */
    s->sun_family = AF_UNIX;
    memcpy(s->sun_path, path, n + 1);
    a->len = (uint32_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
    return true;
}

int slang__plat_addr_family(const slang__plat_addr *a) {
    switch (((const struct sockaddr *)a->u.bytes)->sa_family) {
    case AF_INET: return SLANG_PLAT_AF_INET;
    case AF_INET6: return SLANG_PLAT_AF_INET6;
    case AF_UNIX: return SLANG_PLAT_AF_UNIX;
    default: return 0;
    }
}

void slang__plat_addr_parts(const slang__plat_addr *a, unsigned char ip[16], int *iplen, int64_t *port, uint32_t *scope) {
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

void slang__plat_addr_format(const slang__plat_addr *a, char *buf, size_t cap) {
    buf[0] = 0;
    int fam = slang__plat_addr_family(a);
    if (fam == SLANG_PLAT_AF_UNIX) {
        const struct sockaddr_un *s = (const struct sockaddr_un *)a->u.bytes;
        size_t off = offsetof(struct sockaddr_un, sun_path);
        if (a->len <= off) return;
        size_t n = a->len - off;
        snprintf(buf, cap, "%.*s", (int)strnlen(s->sun_path, n), s->sun_path);
        return;
    }
    unsigned char ip[16];
    int len;
    int64_t port;
    uint32_t scope;
    slang__plat_addr_parts(a, ip, &len, &port, &scope);
    char t[64];
    if (!slang__plat_format_ip(ip, len, t, sizeof t)) return;
    if (len == 16) {
        char z[64] = "";
        if (scope && !slang__plat_if_name(scope, z, sizeof z)) snprintf(z, sizeof z, "%u", scope);
        snprintf(buf, cap, "[%s%s%s]:%lld", t, scope ? "%" : "", z, (long long)port);
    } else {
        snprintf(buf, cap, "%s:%lld", t, (long long)port);
    }
}

int slang__plat_parse_ip(const char *s, unsigned char out[16]) {
    slang__w32_wsa_init();
    if (inet_pton(AF_INET, s, out) == 1) return 4;
    if (inet_pton(AF_INET6, s, out) == 1) return 16;
    return 0;
}

bool slang__plat_format_ip(const unsigned char *ip, int len, char *buf, size_t cap) {
    slang__w32_wsa_init();
    if (len == 4) return inet_ntop(AF_INET, ip, buf, cap) != NULL;
    if (len == 16) return inet_ntop(AF_INET6, ip, buf, cap) != NULL;
    return false;
}

uint32_t slang__plat_if_index(const char *name) { return (uint32_t)if_nametoindex(name); }

bool slang__plat_if_name(uint32_t index, char *buf, size_t cap) {
    char n[IF_MAX_STRING_SIZE + 1];
    if (!if_indextoname(index, n)) return false;
    snprintf(buf, cap, "%s", n);
    return true;
}

bool slang__plat_is_ip(const char *s) {
    unsigned char b[16];
    return slang__plat_parse_ip(s, b) != 0;
}

/* ---- sockets ------------------------------------------------------------------------- */

static int nonblock(SOCKET s) {
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0 ? 0 : WSAGetLastError();
}

int64_t slang__plat_sock_open(int family, int type) {
    slang__w32_wsa_init();
    int af = family == SLANG_PLAT_AF_INET ? AF_INET : family == SLANG_PLAT_AF_INET6 ? AF_INET6 : family == SLANG_PLAT_AF_UNIX ? AF_UNIX : -1;
    int st = type == SLANG_PLAT_STREAM ? SOCK_STREAM : type == SLANG_PLAT_DGRAM ? SOCK_DGRAM : -1;
    if (af < 0 || st < 0) return -WSAEINVAL;
    SOCKET s = WSASocketW(af, st, 0, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return neg_last();
    int e = nonblock(s);
    if (e) {
        closesocket(s);
        return neg(e);
    }
    if (st == SOCK_DGRAM) {
        /* an ICMP port-unreachable must not make the next recvfrom fail (POSIX semantics) */
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, NULL, 0, &ret, NULL, NULL);
    }
    return (int64_t)s;
}

int64_t slang__plat_sock_connect(int64_t fd, const slang__plat_addr *a) {
    if (connect((SOCKET)fd, (const struct sockaddr *)a->u.bytes, (int)a->len) == 0) return 0;
    return neg_last();
}

int slang__plat_sock_error(int64_t fd) {
    int err = 0;
    int len = sizeof err;
    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0) return WSAGetLastError();
    return err;
}

int64_t slang__plat_sock_bind(int64_t fd, const slang__plat_addr *a) {
    if (slang__plat_addr_family(a) != SLANG_PLAT_AF_UNIX) {
        /* POSIX SO_REUSEADDR semantics are the Windows default; exclusive
         * use keeps another process from stealing the port */
        BOOL one = TRUE;
        setsockopt((SOCKET)fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
    }
    if (bind((SOCKET)fd, (const struct sockaddr *)a->u.bytes, (int)a->len) != 0) return neg_last();
    return 0;
}

int64_t slang__plat_sock_listen(int64_t fd, int backlog) {
    if (listen((SOCKET)fd, backlog) != 0) return neg_last();
    return 0;
}

int64_t slang__plat_sock_accept(int64_t lfd, slang__plat_addr *from) {
    for (;;) {
        SOCKADDR_STORAGE ss;
        int len = sizeof ss;
        SOCKET s = accept((SOCKET)lfd, (struct sockaddr *)&ss, &len);
        if (s == INVALID_SOCKET) {
            int e = WSAGetLastError();
            if (e == WSAECONNRESET || e == WSAEINTR) continue;  /* died in the queue */
            return neg(e);
        }
        SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
        int e = nonblock(s);
        if (e) {
            closesocket(s);
            return neg(e);
        }
        if (from) {
            memset(from, 0, sizeof *from);
            memcpy(from->u.bytes, &ss, (size_t)len < sizeof ss ? (size_t)len : sizeof ss);
            from->len = (uint32_t)len;
        }
        return (int64_t)s;
    }
}

static int clampi(int64_t n) { return n > 0x40000000 ? 0x40000000 : (int)n; }

int64_t slang__plat_sock_recv(int64_t fd, void *buf, int64_t n) {
    int r = recv((SOCKET)fd, buf, clampi(n), 0);
    if (r >= 0) return r;
    int e = WSAGetLastError();
    if (e == WSAESHUTDOWN) return 0;
    return neg(e);
}

int64_t slang__plat_sock_send(int64_t fd, const void *buf, int64_t n) {
    int r = send((SOCKET)fd, buf, clampi(n), 0);
    return r >= 0 ? r : neg_last();
}

int64_t slang__plat_sock_sendv(int64_t fd, const slang__plat_iovec *iov, int n) {
    WSABUF v[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) {
        v[i].buf = (char *)iov[i].base;
        v[i].len = (ULONG)(iov[i].len > 0x40000000 ? 0x40000000 : iov[i].len);
    }
    DWORD sent = 0;
    if (WSASend((SOCKET)fd, v, (DWORD)n, &sent, 0, NULL, NULL) != 0) return neg_last();
    return sent;
}

int64_t slang__plat_sock_recvfrom(int64_t fd, void *buf, int64_t n, slang__plat_addr *from, bool *truncated) {
    SOCKADDR_STORAGE ss;
    int len = sizeof ss;
    int r = recvfrom((SOCKET)fd, buf, clampi(n), 0, (struct sockaddr *)&ss, &len);
    bool trunc = false;
    if (r < 0) {
        int e = WSAGetLastError();
        if (e != WSAEMSGSIZE) return neg(e);
        trunc = true;  /* the buffer is full; the rest of the datagram is gone */
        r = clampi(n);
    }
    if (truncated) *truncated = trunc;
    if (from) {
        memset(from, 0, sizeof *from);
        memcpy(from->u.bytes, &ss, (size_t)len < sizeof ss ? (size_t)len : sizeof ss);
        from->len = (uint32_t)len;
    }
    return r;
}

int64_t slang__plat_sock_sendto(int64_t fd, const void *buf, int64_t n, const slang__plat_addr *to) {
    int r = to ? sendto((SOCKET)fd, buf, clampi(n), 0, (const struct sockaddr *)to->u.bytes, (int)to->len)
               : send((SOCKET)fd, buf, clampi(n), 0);
    return r >= 0 ? r : neg_last();
}

int slang__plat_sock_shutdown(int64_t fd, int how) {
    int h = how == (SLANG_PLAT_READ | SLANG_PLAT_WRITE) ? SD_BOTH : how == SLANG_PLAT_READ ? SD_RECEIVE : SD_SEND;
    return shutdown((SOCKET)fd, h) == 0 ? 0 : WSAGetLastError();
}

void slang__plat_sock_close(int64_t fd) {
    if (fd >= 0) closesocket((SOCKET)fd);
}

bool slang__plat_sock_name(int64_t fd, bool peer, slang__plat_addr *a) {
    memset(a, 0, sizeof *a);
    int len = sizeof a->u.bytes;
    int rc = peer ? getpeername((SOCKET)fd, (struct sockaddr *)a->u.bytes, &len) : getsockname((SOCKET)fd, (struct sockaddr *)a->u.bytes, &len);
    if (rc != 0) return false;
    a->len = (uint32_t)len;
    return true;
}

int slang__w32_socketpair(SOCKET sv[2]) {
    slang__w32_wsa_init();
    SOCKET l = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (l == INVALID_SOCKET) return WSAGetLastError();
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof a;
    SOCKET c = INVALID_SOCKET, s = INVALID_SOCKET;
    int e = 0;
    if (bind(l, (struct sockaddr *)&a, sizeof a) != 0 || getsockname(l, (struct sockaddr *)&a, &len) != 0 || listen(l, 1) != 0) {
        e = WSAGetLastError();
    } else if ((c = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT)) == INVALID_SOCKET ||
               connect(c, (struct sockaddr *)&a, sizeof a) != 0 || (s = accept(l, NULL, NULL)) == INVALID_SOCKET) {
        e = WSAGetLastError();
    }
    closesocket(l);
    if (e) {
        if (c != INVALID_SOCKET) closesocket(c);
        return e;
    }
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    BOOL one = TRUE;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    sv[0] = s;
    sv[1] = c;
    return 0;
}

/* ---- options ------------------------------------------------------------------------------ */

static int sock_family(SOCKET fd) {
    SOCKADDR_STORAGE ss;
    int len = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) return -1;
    return ss.ss_family;
}

#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE 3
#endif
#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT 16
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL 17
#endif
#ifndef TCP_FASTOPEN
#define TCP_FASTOPEN 15
#endif

static bool opt_of(SOCKET fd, int opt, int *level, int *name) {
    switch (opt) {
    case SLANG_PLAT_OPT_NODELAY: *level = IPPROTO_TCP; *name = TCP_NODELAY; return true;
    case SLANG_PLAT_OPT_KEEPALIVE: *level = SOL_SOCKET; *name = SO_KEEPALIVE; return true;
    case SLANG_PLAT_OPT_KEEPIDLE: *level = IPPROTO_TCP; *name = TCP_KEEPIDLE; return true;    /* Windows 10 1709+ */
    case SLANG_PLAT_OPT_KEEPINTVL: *level = IPPROTO_TCP; *name = TCP_KEEPINTVL; return true;
    case SLANG_PLAT_OPT_KEEPCNT: *level = IPPROTO_TCP; *name = TCP_KEEPCNT; return true;
    case SLANG_PLAT_OPT_RCVBUF: *level = SOL_SOCKET; *name = SO_RCVBUF; return true;
    case SLANG_PLAT_OPT_SNDBUF: *level = SOL_SOCKET; *name = SO_SNDBUF; return true;
    case SLANG_PLAT_OPT_REUSEADDR: return false;  /* see sock_setopt */
    case SLANG_PLAT_OPT_BROADCAST: *level = SOL_SOCKET; *name = SO_BROADCAST; return true;
    case SLANG_PLAT_OPT_V6ONLY: *level = IPPROTO_IPV6; *name = IPV6_V6ONLY; return true;
    case SLANG_PLAT_OPT_FASTOPEN: *level = IPPROTO_TCP; *name = TCP_FASTOPEN; return true;
    case SLANG_PLAT_OPT_MCAST_LOOP:
        if (sock_family(fd) == AF_INET6) { *level = IPPROTO_IPV6; *name = IPV6_MULTICAST_LOOP; }
        else { *level = IPPROTO_IP; *name = IP_MULTICAST_LOOP; }
        return true;
    case SLANG_PLAT_OPT_MCAST_TTL:
        if (sock_family(fd) == AF_INET6) { *level = IPPROTO_IPV6; *name = IPV6_MULTICAST_HOPS; }
        else { *level = IPPROTO_IP; *name = IP_MULTICAST_TTL; }
        return true;
    default: return false;  /* REUSEPORT, CORK: not on Windows */
    }
}

int slang__plat_sock_setopt(int64_t fd, int opt, int64_t value) {
    SOCKET s = (SOCKET)fd;
    if (opt == SLANG_PLAT_OPT_REUSEADDR) {
        /* POSIX SO_REUSEADDR (rebind while TIME_WAIT) is Windows' default
         * behaviour; Windows SO_REUSEADDR would allow port hijacking. */
        (void)value;
        return 0;
    }
    if (opt == SLANG_PLAT_OPT_MCAST_IF) {
        if (sock_family(s) == AF_INET6) {
            DWORD idx = (DWORD)value;
            return setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, (const char *)&idx, sizeof idx) == 0 ? 0 : WSAGetLastError();
        }
        DWORD idx = htonl((u_long)value);  /* IP_MULTICAST_IF takes an index in 0.0.0.x form */
        return setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&idx, sizeof idx) == 0 ? 0 : WSAGetLastError();
    }
    if (opt == SLANG_PLAT_OPT_LINGER) {
        struct linger l = { (u_short)(value >= 0), (u_short)(value >= 0 ? value : 0) };
        return setsockopt(s, SOL_SOCKET, SO_LINGER, (const char *)&l, sizeof l) == 0 ? 0 : WSAGetLastError();
    }
    int level, name;
    if (!opt_of(s, opt, &level, &name)) return WSAENOPROTOOPT;
    DWORD v = value > 0x7fffffff ? 0x7fffffff : value < 0 ? 0 : (DWORD)value;
    return setsockopt(s, level, name, (const char *)&v, sizeof v) == 0 ? 0 : WSAGetLastError();
}

int64_t slang__plat_sock_getopt(int64_t fd, int opt) {
    SOCKET s = (SOCKET)fd;
    if (opt == SLANG_PLAT_OPT_LINGER) {
        struct linger l;
        int len = sizeof l;
        if (getsockopt(s, SOL_SOCKET, SO_LINGER, (char *)&l, &len) != 0) return -(int64_t)WSAGetLastError();
        return l.l_onoff ? l.l_linger : -1;
    }
    if (opt == SLANG_PLAT_OPT_REUSEADDR) return 1;
    int level, name;
    if (!opt_of(s, opt, &level, &name)) return -(int64_t)WSAENOPROTOOPT;
    DWORD v = 0;
    int len = sizeof v;
    if (getsockopt(s, level, name, (char *)&v, &len) != 0) return -(int64_t)WSAGetLastError();
    if (len == 1) v = *(unsigned char *)&v;
    return (int64_t)v;
}

int slang__plat_sock_mcast(int64_t fd, bool join, const unsigned char *group, int len, uint32_t ifindex) {
    SOCKET s = (SOCKET)fd;
    if (len == 4) {
        struct ip_mreq m;
        memset(&m, 0, sizeof m);
        memcpy(&m.imr_multiaddr, group, 4);
        m.imr_interface.s_addr = htonl(ifindex);  /* 0.0.0.x: interface index */
        int rc = setsockopt(s, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, (const char *)&m, sizeof m);
        return rc == 0 ? 0 : WSAGetLastError();
    }
    if (len == 16) {
        struct ipv6_mreq m;
        memset(&m, 0, sizeof m);
        memcpy(&m.ipv6mr_multiaddr, group, 16);
        m.ipv6mr_interface = ifindex;
        int rc = setsockopt(s, IPPROTO_IPV6, join ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP, (const char *)&m, sizeof m);
        return rc == 0 ? 0 : WSAGetLastError();
    }
    return WSAEINVAL;
}

/* ---- files and sendfile ------------------------------------------------------------------- */

int64_t slang__plat_file_open(const char *path) { return slang__plat_open(path, SLANG_PLAT_O_RDONLY, 0); }

int64_t slang__plat_file_size(int64_t fd) {
    slang__plat_stat st;
    int e = slang__plat_fstat(fd, &st);
    return e ? -(int64_t)e : st.size;
}

void slang__plat_file_close(int64_t fd) {
    if (fd >= 0) slang__plat_close(fd);
}

int64_t slang__plat_sendfile(int64_t sock, int64_t fd, int64_t off, int64_t n) {
    if (n <= 0) return 0;
    char buf[65536];
    int64_t want = n > (int64_t)sizeof buf ? (int64_t)sizeof buf : n;
    int64_t got = slang__plat_pread(fd, buf, want, off);
    if (got <= 0) return got;
    return slang__plat_sock_send(sock, buf, got);
}

void slang__plat_unlink(const char *path) {
    if (path && path[0] && path[0] != '@') slang__plat_unlink_file(path);
}

/* ---- name resolution ------------------------------------------------------------------------ */

static bool addr_text(const struct sockaddr *sa, char *ip, size_t cap) {
    slang__plat_addr a;
    memset(&a, 0, sizeof a);
    size_t len = sa->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    memcpy(a.u.bytes, sa, len);
    a.len = (uint32_t)len;
    unsigned char b[16];
    int bl;
    int64_t port;
    uint32_t scope;
    slang__plat_addr_parts(&a, b, &bl, &port, &scope);
    char t[64];
    if (!slang__plat_format_ip(b, bl, t, sizeof t)) return false;
    if (bl == 16 && scope) {
        char z[64];
        if (!slang__plat_if_name(scope, z, sizeof z)) snprintf(z, sizeof z, "%u", scope);
        snprintf(ip, cap, "%s%%%s", t, z);
    } else {
        snprintf(ip, cap, "%s", t);
    }
    return true;
}

static bool gai_notfound(int rc) { return rc == WSAHOST_NOT_FOUND || rc == WSANO_DATA || rc == EAI_NONAME; }

const char *slang__plat_resolve(const char *host, int flags, char ***out, int *n, bool *notfound) {
    slang__w32_wsa_init();
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (flags & SLANG_PLAT_RESOLVE_PASSIVE) hints.ai_flags |= AI_PASSIVE;
    *out = NULL;
    *n = 0;
    *notfound = false;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        *notfound = gai_notfound(rc);
        if (*notfound) return "no such host";
        return "name resolution failed";
    }
    int cap = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) cap++;
    char **v = calloc((size_t)cap + 1, sizeof *v);
    int k = 0;
    int passes = (flags & SLANG_PLAT_RESOLVE_V4FIRST) ? 2 : 1;
    for (int pass = 0; pass < passes && v; pass++) {
        for (struct addrinfo *a = res; a; a = a->ai_next) {
            if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
            if (passes == 2 && a->ai_family != (pass == 0 ? AF_INET : AF_INET6)) continue;
            char ip[128];
            if (!addr_text(a->ai_addr, ip, sizeof ip)) continue;
            bool dup = false;
            for (int i = 0; i < k; i++) if (strcmp(v[i], ip) == 0) dup = true;
            if (dup) continue;
            v[k] = _strdup(ip);
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

const char *slang__plat_reverse(const char *ip, char ***out, int *n, bool *notfound) {
    *out = NULL;
    *n = 0;
    *notfound = false;
    unsigned char b[16];
    int len = slang__plat_parse_ip(ip, b);
    if (!len) return "not an IP address";
    slang__plat_addr a;
    slang__plat_addr_ip(&a, b, len, 0, 0);
    char host[NI_MAXHOST];
    int rc = getnameinfo((const struct sockaddr *)a.u.bytes, (socklen_t)a.len, host, sizeof host, NULL, 0, NI_NAMEREQD);
    if (rc != 0) {
        *notfound = gai_notfound(rc);
        return *notfound ? "no such host" : "name resolution failed";
    }
    char **v = calloc(2, sizeof *v);
    if (!v) return "out of memory";
    v[0] = _strdup(host);
    if (!v[0]) { free(v); return "out of memory"; }
    *out = v;
    *n = 1;
    return NULL;
}

int64_t slang__plat_lookup_port(const char *network, const char *service) {
    char *end;
    long p = strtol(service, &end, 10);
    if (*service && !*end) return p >= 0 && p <= 65535 ? p : -1;
    slang__w32_wsa_init();
    const char *proto = strncmp(network, "udp", 3) == 0 ? "udp" : "tcp";
    struct servent *se = getservbyname(service, proto);
    if (se) return ntohs((u_short)se->s_port);
    /* Wine and minimal installs may lack the services file: the common ones */
    static const struct { const char *name; int port; } known[] = {
        {"http", 80}, {"https", 443}, {"ftp", 21}, {"ssh", 22}, {"telnet", 23}, {"smtp", 25}, {"domain", 53},
        {"pop3", 110}, {"imap", 143}, {"ldap", 389}, {"imaps", 993}, {"pop3s", 995}, {"submission", 587},
    };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (strcmp(service, known[i].name) == 0) return known[i].port;
    return -1;
}

int slang__plat_interfaces(char ***out, int *n) {
    *out = NULL;
    *n = 0;
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *aa = NULL;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; tries++) {
        free(aa);
        aa = malloc(size);
        if (!aa) return ENOMEM;
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &size);
    }
    if (rc != NO_ERROR) {
        free(aa);
        return rc == ERROR_NO_DATA ? 0 : slang__w32_errno(rc);
    }
    int cap = 0, k = 0;
    char **v = NULL;
    for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next) {
        if (k == cap) {
            cap = cap ? cap * 2 : 8;
            char **nv = realloc(v, sizeof *v * (size_t)cap);
            if (!nv) break;
            v = nv;
        }
        char *fname = a->FriendlyName ? slang__w32_utf8(a->FriendlyName, -1) : NULL;
        char flags[8];
        int f = 0;
        if (a->OperStatus == IfOperStatusUp) flags[f++] = 'u';
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) flags[f++] = 'l';
        if (!(a->Flags & IP_ADAPTER_NO_MULTICAST)) flags[f++] = 'm';
        if (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211) flags[f++] = 'b';
        if (a->IfType == IF_TYPE_PPP || a->IfType == IF_TYPE_TUNNEL) flags[f++] = 'p';
        flags[f] = 0;
        size_t lcap = 512;
        char *line = malloc(lcap);
        if (!line) { free(fname); break; }
        int len = snprintf(line, lcap, "%s\t%u\t%u\t%s\t", fname ? fname : a->AdapterName,
                           (unsigned)(a->IfIndex ? a->IfIndex : a->Ipv6IfIndex), (unsigned)a->Mtu, flags);
        free(fname);
        bool first = true;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            char ip[128];
            if (!addr_text(u->Address.lpSockaddr, ip, sizeof ip)) continue;
            char *z = strchr(ip, '%');
            if (z) *z = 0;
            if ((size_t)len + strlen(ip) + 8 >= lcap) {
                lcap *= 2;
                char *nl = realloc(line, lcap);
                if (!nl) break;
                line = nl;
            }
            len += snprintf(line + len, lcap - (size_t)len, "%s%s/%u", first ? "" : ",", ip, (unsigned)u->OnLinkPrefixLength);
            first = false;
        }
        v[k++] = line;
    }
    free(aa);
    *out = v;
    *n = k;
    return 0;
}

#else
typedef int slang__net_win32_unused;
#endif
