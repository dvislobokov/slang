/* certroots.c - where the system keeps its trusted root certificates
 * (std.crypto.x509.SystemRoots). Linux distributions and the BSDs ship a
 * PEM bundle; macOS and Windows keep roots in a keychain / system store
 * that is not wired up yet (stub: only $SSL_CERT_FILE is honoured). */
#include "platform.h"

#include <stdlib.h>
#include <unistd.h>

const char *slang__plat_cert_bundle_path(void) {
    const char *env = getenv("SSL_CERT_FILE");
    if (env && env[0] && access(env, R_OK) == 0) return env;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    static const char *const paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",                /* Debian, Ubuntu, Gentoo, Arch */
        "/etc/pki/tls/certs/ca-bundle.crt",                  /* Fedora, RHEL */
        "/etc/ssl/ca-bundle.pem",                            /* openSUSE */
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* CentOS, RHEL 7 */
        "/etc/ssl/cert.pem",                                 /* Alpine, OpenBSD */
        "/usr/local/share/certs/ca-root-nss.crt",            /* FreeBSD */
        "/usr/local/etc/ssl/cert.pem",                       /* FreeBSD */
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++)
        if (access(paths[i], R_OK) == 0) return paths[i];
#endif
    return NULL;
}
