/* net_posix.c - BSD sockets for the platform layer (platform.h): non-blocking
 * TCP and UDP sockets and getaddrinfo name resolution. */
#include "platform.h"

#if defined(__unix__) || defined(__APPLE__)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static int64_t neg(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS ? -SLANG_PLAT_EAGAIN : -(int64_t)(e ? e : EIO); }

/* Fills a sockaddr from a numeric ip; returns its length or 0. */
static socklen_t make_addr(const char *ip, int64_t port, struct sockaddr_storage *ss) {
    memset(ss, 0, sizeof *ss);
    struct sockaddr_in *a4 = (struct sockaddr_in *)ss;
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)ss;
    if (port < 0 || port > 65535) return 0;
    if (inet_pton(AF_INET, ip, &a4->sin_addr) == 1) {
        a4->sin_family = AF_INET;
        a4->sin_port = htons((uint16_t)port);
        return sizeof *a4;
    }
    if (inet_pton(AF_INET6, ip, &a6->sin6_addr) == 1) {
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((uint16_t)port);
        return sizeof *a6;
    }
    return 0;
}

static int nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) return errno;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return 0;
}

static int64_t new_socket(int family, int type) {
    int fd = socket(family, type, 0);
    if (fd < 0) return neg(errno);
    int e = nonblock(fd);
    if (e) { close(fd); return neg(e); }
    return fd;
}

int64_t slang__plat_tcp_connect(const char *ip, int64_t port) {
    struct sockaddr_storage ss;
    socklen_t len = make_addr(ip, port, &ss);
    if (!len) return -EINVAL;
    int64_t fd = new_socket(ss.ss_family, SOCK_STREAM);
    if (fd < 0) return fd;
    int one = 1;
    setsockopt((int)fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect((int)fd, (struct sockaddr *)&ss, len) != 0 && errno != EINPROGRESS && errno != EINTR) {
        int e = errno;
        close((int)fd);
        return -(int64_t)e;
    }
    return fd;
}

int slang__plat_tcp_connect_result(int64_t fd) {
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt((int)fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return errno;
    return err;
}

int64_t slang__plat_tcp_listen(const char *ip, int64_t port, int backlog) {
    struct sockaddr_storage ss;
    socklen_t len = make_addr(ip, port, &ss);
    if (!len) return -EINVAL;
    int64_t fd = new_socket(ss.ss_family, SOCK_STREAM);
    if (fd < 0) return fd;
    int one = 1;
    setsockopt((int)fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind((int)fd, (struct sockaddr *)&ss, len) != 0 || listen((int)fd, backlog) != 0) {
        int e = errno;
        close((int)fd);
        return -(int64_t)e;
    }
    return fd;
}

int64_t slang__plat_tcp_accept(int64_t lfd) {
    for (;;) {
        int fd = accept((int)lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return neg(errno);
        }
        int e = nonblock(fd);
        if (e) { close(fd); return neg(e); }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        return fd;
    }
}

int64_t slang__plat_udp_bind(const char *ip, int64_t port) {
    struct sockaddr_storage ss;
    socklen_t len = make_addr(ip, port, &ss);
    if (!len) return -EINVAL;
    int64_t fd = new_socket(ss.ss_family, SOCK_DGRAM);
    if (fd < 0) return fd;
    if (bind((int)fd, (struct sockaddr *)&ss, len) != 0) {
        int e = errno;
        close((int)fd);
        return -(int64_t)e;
    }
    return fd;
}

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

int64_t slang__plat_sock_recv(int64_t fd, void *buf, int64_t n) {
    for (;;) {
        ssize_t r = recv((int)fd, buf, (size_t)n, 0);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return neg(errno);
    }
}

int64_t slang__plat_sock_send(int64_t fd, const void *buf, int64_t n) {
    for (;;) {
        ssize_t r = send((int)fd, buf, (size_t)n, SEND_FLAGS);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return neg(errno);
    }
}

static void addr_string(const struct sockaddr_storage *ss, char *ip, size_t cap, int64_t *port) {
    ip[0] = 0;
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)ss;
        inet_ntop(AF_INET, &a->sin_addr, ip, (socklen_t)cap);
        *port = ntohs(a->sin_port);
    } else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)ss;
        inet_ntop(AF_INET6, &a->sin6_addr, ip, (socklen_t)cap);
        *port = ntohs(a->sin6_port);
    } else {
        *port = 0;
    }
}

int64_t slang__plat_udp_recvfrom(int64_t fd, void *buf, int64_t n, char *from, size_t cap, int64_t *port) {
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t len = sizeof ss;
        ssize_t r = recvfrom((int)fd, buf, (size_t)n, 0, (struct sockaddr *)&ss, &len);
        if (r >= 0) {
            addr_string(&ss, from, cap, port);
            return r;
        }
        if (errno == EINTR) continue;
        return neg(errno);
    }
}

int64_t slang__plat_udp_sendto(int64_t fd, const void *buf, int64_t n, const char *ip, int64_t port) {
    struct sockaddr_storage ss;
    socklen_t len = make_addr(ip, port, &ss);
    if (!len) return -EINVAL;
    for (;;) {
        ssize_t r = sendto((int)fd, buf, (size_t)n, SEND_FLAGS, (struct sockaddr *)&ss, len);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        return neg(errno);
    }
}

void slang__plat_sock_close(int64_t fd) {
    if (fd >= 0) close((int)fd);
}

bool slang__plat_sock_addr(int64_t fd, bool peer, char *buf, size_t cap) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    int rc = peer ? getpeername((int)fd, (struct sockaddr *)&ss, &len) : getsockname((int)fd, (struct sockaddr *)&ss, &len);
    if (rc != 0) return false;
    char ip[INET6_ADDRSTRLEN];
    int64_t port;
    addr_string(&ss, ip, sizeof ip, &port);
    if (ss.ss_family == AF_INET6) snprintf(buf, cap, "[%s]:%lld", ip, (long long)port);
    else snprintf(buf, cap, "%s:%lld", ip, (long long)port);
    return true;
}

int64_t slang__plat_sock_port(int64_t fd) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getsockname((int)fd, (struct sockaddr *)&ss, &len) != 0) return -(int64_t)errno;
    char ip[INET6_ADDRSTRLEN];
    int64_t port;
    addr_string(&ss, ip, sizeof ip, &port);
    return port;
}

bool slang__plat_is_ip(const char *s) {
    unsigned char b[16];
    return inet_pton(AF_INET, s, b) == 1 || inet_pton(AF_INET6, s, b) == 1;
}

const char *slang__plat_resolve(const char *host, char ***out, int *n) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    *out = NULL;
    *n = 0;
    if (rc != 0) return gai_strerror(rc);
    int cap = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) cap++;
    char **v = calloc((size_t)cap + 1, sizeof *v);
    int k = 0;
    /* IPv4 first, then IPv6; skip duplicates. */
    for (int pass = 0; pass < 2 && v; pass++) {
        for (struct addrinfo *a = res; a; a = a->ai_next) {
            int fam = pass == 0 ? AF_INET : AF_INET6;
            if (a->ai_family != fam) continue;
            char ip[INET6_ADDRSTRLEN];
            int64_t port;
            struct sockaddr_storage ss;
            memset(&ss, 0, sizeof ss);
            memcpy(&ss, a->ai_addr, a->ai_addrlen);
            addr_string(&ss, ip, sizeof ip, &port);
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
        return "no addresses found";
    }
    *out = v;
    *n = k;
    return NULL;
}

const char *slang__plat_strerror(int code, char *buf, size_t cap) {
    if (code == SLANG_PLAT_EAGAIN) code = EAGAIN;
    snprintf(buf, cap, "%s", strerror(code));
    return buf;
}

#else
#error "platform/net_posix.c: unsupported platform"
#endif
