/* certroots.c - where the system keeps its trusted root certificates
 * (std.crypto.x509.SystemRoots). $SSL_CERT_FILE always wins. Linux
 * distributions and the BSDs ship a PEM bundle; macOS ships one too
 * (/etc/ssl/cert.pem, maintained with the system's roots; the keychain's
 * user-added roots are not included). Windows keeps roots in the
 * certificate store: the "ROOT" system store (CertOpenSystemStoreW,
 * crypt32.dll loaded on demand) is exported once per process to a PEM
 * file under %LOCALAPPDATA%\zell (else %TEMP%), whose path is returned,
 * so x509.zl reads and caches it like any bundle. Windows fetches missing
 * roots on demand (Automatic Root Certificates Update): a root that was
 * never used on the machine may be absent from the store. */
#include "platform.h"

#if defined(_WIN32)

#include "win32_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *(WINAPI *open_store_fn)(ULONG_PTR, LPCWSTR);
typedef const void *(WINAPI *enum_cert_fn)(void *, const void *);
typedef BOOL(WINAPI *close_store_fn)(void *, DWORD);

/* CERT_CONTEXT (wincrypt.h), declared here to keep wincrypt out. */
typedef struct cert_ctx {
    DWORD dwCertEncodingType;
    BYTE *pbCertEncoded;
    DWORD cbCertEncoded;
    void *pCertInfo;
    void *hCertStore;
} cert_ctx;

static zell__plat_once g_roots_once = ZELL_PLAT_ONCE_INIT;
static char g_roots_path[1024];
static bool g_roots_ok;

static void b64_pem(FILE *f, const unsigned char *p, size_t n) {
    static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    fputs("-----BEGIN CERTIFICATE-----\n", f);
    int col = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        char q[4] = { tb[(v >> 18) & 63], tb[(v >> 12) & 63], i + 1 < n ? tb[(v >> 6) & 63] : '=', i + 2 < n ? tb[v & 63] : '=' };
        fwrite(q, 1, 4, f);
        col += 4;
        if (col == 64) {
            fputc('\n', f);
            col = 0;
        }
    }
    if (col) fputc('\n', f);
    fputs("-----END CERTIFICATE-----\n", f);
}

static void export_roots(void) {
    HMODULE c = LoadLibraryW(L"crypt32.dll");
    if (!c) return;
    open_store_fn open = (open_store_fn)(void *)GetProcAddress(c, "CertOpenSystemStoreW");
    enum_cert_fn next = (enum_cert_fn)(void *)GetProcAddress(c, "CertEnumCertificatesInStore");
    close_store_fn close = (close_store_fn)(void *)GetProcAddress(c, "CertCloseStore");
    if (!open || !next || !close) return;
    void *store = open(0, L"ROOT");
    if (!store) return;
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 40) n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH - 40) {
        close(store, 0);
        return;
    }
    while (n > 0 && (dir[n - 1] == L'\\' || dir[n - 1] == L'/')) dir[--n] = 0;
    wcscat(dir, L"\\zell");
    CreateDirectoryW(dir, NULL);
    wchar_t final[MAX_PATH + 40], tmp[MAX_PATH + 60];
    _snwprintf(final, MAX_PATH + 39, L"%ls\\roots.pem", dir);
    final[MAX_PATH + 39] = 0;
    _snwprintf(tmp, MAX_PATH + 59, L"%ls\\roots.%lu.tmp", dir, (unsigned long)GetCurrentProcessId());
    tmp[MAX_PATH + 59] = 0;
    FILE *f = _wfopen(tmp, L"wb");
    int count = 0;
    if (f) {
        fputs("# Trusted roots exported from the Windows ROOT certificate store by the Zell runtime\n", f);
        for (const cert_ctx *ctx = next(store, NULL); ctx; ctx = next(store, ctx)) {
            if ((ctx->dwCertEncodingType & 1 /* X509_ASN_ENCODING */) && ctx->cbCertEncoded) {
                b64_pem(f, ctx->pbCertEncoded, ctx->cbCertEncoded);
                count++;
            }
        }
        bool wrote = fclose(f) == 0;
        /* replace atomically: a concurrent process may be reading the old file */
        if (wrote && count > 0 && MoveFileExW(tmp, final, MOVEFILE_REPLACE_EXISTING)) {
            char *u = zell__w32_utf8(final, -1);
            if (u) {
                snprintf(g_roots_path, sizeof g_roots_path, "%s", u);
                g_roots_ok = true;
                free(u);
            }
        } else {
            DeleteFileW(tmp);
        }
    }
    close(store, 0);
}

const char *zell__plat_cert_bundle_path(void) {
    const char *env = getenv("SSL_CERT_FILE");
    if (env && env[0] && GetFileAttributesA(env) != INVALID_FILE_ATTRIBUTES) return env;
    zell__plat_once_run(&g_roots_once, export_roots);
    return g_roots_ok ? g_roots_path : NULL;
}

#else

#include <stdlib.h>
#include <unistd.h>

const char *zell__plat_cert_bundle_path(void) {
    const char *env = getenv("SSL_CERT_FILE");
    if (env && env[0] && access(env, R_OK) == 0) return env;
    static const char *const paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",                /* Debian, Ubuntu, Gentoo, Arch */
        "/etc/pki/tls/certs/ca-bundle.crt",                  /* Fedora, RHEL */
        "/etc/ssl/ca-bundle.pem",                            /* openSUSE */
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* CentOS, RHEL 7 */
        "/etc/ssl/cert.pem",                                 /* Alpine, OpenBSD, macOS */
        "/usr/local/share/certs/ca-root-nss.crt",            /* FreeBSD */
        "/usr/local/etc/ssl/cert.pem",                       /* FreeBSD */
        "/opt/homebrew/etc/openssl@3/cert.pem",              /* macOS, Homebrew */
        "/usr/local/etc/openssl@3/cert.pem",                 /* macOS, Homebrew (Intel) */
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++)
        if (access(paths[i], R_OK) == 0) return paths[i];
    return NULL;
}

#endif
