/* std_mime.c - the extension -> media type table of mime
 * (runtime/ABI.md 19.5): a built-in table (Go's built-in types plus common
 * web, document, archive, font, audio and video types) and the entries
 * added at run time with AddExtensionType, under a lock. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const builtin[][2] = {
    {".7z", "application/x-7z-compressed"}, {".aac", "audio/aac"}, {".avif", "image/avif"}, {".bin", "application/octet-stream"},
    {".bmp", "image/bmp"}, {".bz2", "application/x-bzip2"}, {".css", "text/css; charset=utf-8"}, {".csv", "text/csv; charset=utf-8"},
    {".doc", "application/msword"}, {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".eot", "application/vnd.ms-fontobject"}, {".epub", "application/epub+zip"}, {".flac", "audio/flac"}, {".gif", "image/gif"},
    {".gz", "application/gzip"}, {".htm", "text/html; charset=utf-8"}, {".html", "text/html; charset=utf-8"}, {".ico", "image/vnd.microsoft.icon"},
    {".ics", "text/calendar; charset=utf-8"}, {".jar", "application/java-archive"}, {".jpeg", "image/jpeg"}, {".jpg", "image/jpeg"},
    {".js", "text/javascript; charset=utf-8"}, {".json", "application/json"}, {".jsonld", "application/ld+json"}, {".m4a", "audio/mp4"},
    {".md", "text/markdown; charset=utf-8"}, {".mid", "audio/midi"}, {".mjs", "text/javascript; charset=utf-8"}, {".mp3", "audio/mpeg"},
    {".mp4", "video/mp4"}, {".mpeg", "video/mpeg"}, {".odp", "application/vnd.oasis.opendocument.presentation"},
    {".ods", "application/vnd.oasis.opendocument.spreadsheet"}, {".odt", "application/vnd.oasis.opendocument.text"}, {".oga", "audio/ogg"},
    {".ogg", "audio/ogg"}, {".ogv", "video/ogg"}, {".otf", "font/otf"}, {".pdf", "application/pdf"}, {".png", "image/png"},
    {".ppt", "application/vnd.ms-powerpoint"}, {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".rar", "application/vnd.rar"}, {".rtf", "application/rtf"}, {".sh", "application/x-sh"}, {".zl", "text/x-zell; charset=utf-8"},
    {".svg", "image/svg+xml"}, {".tar", "application/x-tar"}, {".tif", "image/tiff"}, {".tiff", "image/tiff"}, {".toml", "application/toml"},
    {".ts", "video/mp2t"}, {".ttf", "font/ttf"}, {".txt", "text/plain; charset=utf-8"}, {".wasm", "application/wasm"}, {".wav", "audio/wav"},
    {".weba", "audio/webm"}, {".webm", "video/webm"}, {".webmanifest", "application/manifest+json"}, {".webp", "image/webp"},
    {".woff", "font/woff"}, {".woff2", "font/woff2"}, {".xhtml", "application/xhtml+xml"}, {".xls", "application/vnd.ms-excel"},
    {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"}, {".xml", "text/xml; charset=utf-8"},
    {".yaml", "application/yaml"}, {".yml", "application/yaml"}, {".zip", "application/zip"}, {".zst", "application/zstd"},
};

typedef struct {
    char *ext;
    char *type;
} added_t;

static zell__plat_mutex mu = ZELL_PLAT_MUTEX_INIT;
static added_t *added;
static size_t nadded, capadded;

static void lower(char *d, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = (char)tolower((unsigned char)s[i]);
    d[n] = 0;
}

/* The media type of an extension like ".html" ("" if unknown; the exact
 * spelling is tried first, then lower case). */
zell_string *zell_mime_rt_type(const zell_string *ext) {
    const char *e = zell_string_cstr(ext);
    size_t n = (size_t)zell_string_len(ext);
    char low[64];
    if (n == 0 || n >= sizeof low) return zell__string_alloc(0);
    lower(low, e, n);
    const char *found = NULL;
    zell__plat_mutex_lock(&mu);
    for (size_t i = nadded; i-- > 0;)
        if (strcmp(added[i].ext, e) == 0) { found = added[i].type; break; }
    if (!found)
        for (size_t i = nadded; i-- > 0;)
            if (strcmp(added[i].ext, low) == 0) { found = added[i].type; break; }
    zell_string *r = found ? zell_string_from_cstr(found) : NULL;
    zell__plat_mutex_unlock(&mu);
    if (r) return r;
    for (size_t i = 0; i < sizeof builtin / sizeof builtin[0]; i++)
        if (strcmp(builtin[i][0], low) == 0) return zell_string_from_cstr(builtin[i][1]);
    return zell__string_alloc(0);
}

/* Records ext -> type (copies; kept for the life of the process). */
void zell_mime_rt_add(const zell_string *ext, const zell_string *type) {
    zell__plat_mutex_lock(&mu);
    if (nadded == capadded) {
        capadded = capadded ? capadded * 2 : 8;
        added = realloc(added, capadded * sizeof *added);
        if (!added) zell_panic("out of memory");
    }
    size_t n = (size_t)zell_string_len(ext);
    added[nadded].ext = malloc(n + 1);
    lower(added[nadded].ext, zell_string_cstr(ext), n);
    added[nadded].type = strdup(zell_string_cstr(type));
    nadded++;
    zell__plat_mutex_unlock(&mu);
}

static int same_type(const char *t, const char *want, size_t wn) {
    /* compares the media type part (before ';'), case-insensitively */
    size_t tn = strcspn(t, ";");
    while (tn > 0 && t[tn - 1] == ' ') tn--;
    return tn == wn && strncasecmp(t, want, wn) == 0;
}

/* The extensions known for a media type (no parameters), sorted. */
void zell_mime_rt_exts(const zell_string *type, zell_slice *out) {
    const char *w = zell_string_cstr(type);
    size_t wn = strcspn(w, ";");
    while (wn > 0 && w[wn - 1] == ' ') wn--;
    zell_slice_new_ops(out, 8, 0, 4, &zell_ops_ref);
    const char *list[128];
    size_t k = 0;
    for (size_t i = 0; i < sizeof builtin / sizeof builtin[0] && k < 128; i++)
        if (same_type(builtin[i][1], w, wn)) list[k++] = builtin[i][0];
    zell__plat_mutex_lock(&mu);
    for (size_t i = 0; i < nadded && k < 128; i++)
        if (same_type(added[i].type, w, wn)) {
            int dup = 0;
            for (size_t j = 0; j < k; j++) dup |= strcmp(list[j], added[i].ext) == 0;
            if (!dup) list[k++] = added[i].ext;
        }
    /* sort the names (insertion sort) and copy while locked */
    for (size_t i = 1; i < k; i++)
        for (size_t j = i; j > 0 && strcmp(list[j - 1], list[j]) > 0; j--) {
            const char *t = list[j];
            list[j] = list[j - 1];
            list[j - 1] = t;
        }
    for (size_t i = 0; i < k; i++) {
        zell_string *s = zell_string_from_cstr(list[i]);
        zell_slice_append(out, 8, &zell_ops_ref, &s);
        zell_release(s);
    }
    zell__plat_mutex_unlock(&mu);
}
