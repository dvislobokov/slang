/* profile.c - std.diag CPU and heap profiles in pprof format.
 *
 * CPU: the platform layer's SIGPROF sampler (platform/posix.c) hands every
 * sample (PC + frame-pointer return addresses) to on_sample, which copies it
 * into a preallocated buffer with one atomic reservation (async-signal-safe,
 * no locks, no allocation). Stopping symbolizes the distinct addresses with
 * the executable's symbol table and writes a `profile.proto` message
 * (github.com/google/pprof/proto/profile.proto), uncompressed, which
 * `go tool pprof` reads directly. Heap: live objects by type name from the
 * leak tracker (needs `--leak-check` without `--release`), written as a
 * pprof profile with one pseudo-frame per type. ABI.md section 18. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- protobuf encoding ------------------------------------------------ */

typedef struct { unsigned char *p; size_t len, cap; int oom; } pbuf;

static void pb_reserve(pbuf *b, size_t n) {
    if (b->oom || b->len + n <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->len + n) cap *= 2;
    unsigned char *q = realloc(b->p, cap);
    if (!q) { b->oom = 1; return; }
    b->p = q;
    b->cap = cap;
}

static void pb_bytes(pbuf *b, const void *p, size_t n) {
    pb_reserve(b, n);
    if (b->oom) return;
    memcpy(b->p + b->len, p, n);
    b->len += n;
}

static void pb_varint(pbuf *b, uint64_t v) {
    unsigned char tmp[10];
    int n = 0;
    do {
        unsigned char c = v & 0x7f;
        v >>= 7;
        tmp[n++] = c | (v ? 0x80 : 0);
    } while (v);
    pb_bytes(b, tmp, (size_t)n);
}

static void pb_key(pbuf *b, int field, int wire) { pb_varint(b, ((uint64_t)field << 3) | (uint64_t)wire); }
static void pb_uint(pbuf *b, int field, uint64_t v) {
    if (!v) return;
    pb_key(b, field, 0);
    pb_varint(b, v);
}
static void pb_sub(pbuf *b, int field, const pbuf *sub) {
    pb_key(b, field, 2);
    pb_varint(b, sub->len);
    pb_bytes(b, sub->p, sub->len);
}
static void pb_str(pbuf *b, int field, const char *s) {
    size_t n = strlen(s);
    pb_key(b, field, 2);
    pb_varint(b, n);
    pb_bytes(b, s, n);
}

/* ---- string table ------------------------------------------------------ */

typedef struct { char **v; int64_t n, cap; } strtab;

static int64_t st_add(strtab *t, const char *s) {
    for (int64_t i = 0; i < t->n; i++)
        if (strcmp(t->v[i], s) == 0) return i;
    if (t->n == t->cap) {
        int64_t cap = t->cap ? t->cap * 2 : 64;
        char **nv = realloc(t->v, (size_t)cap * sizeof *nv);
        if (!nv) return 0;
        t->v = nv;
        t->cap = cap;
    }
    char *c = strdup(s);
    if (!c) return 0;
    t->v[t->n] = c;
    return t->n++;
}

static void st_free(strtab *t) {
    for (int64_t i = 0; i < t->n; i++) free(t->v[i]);
    free(t->v);
}

/* `zell_app__main__run` -> `app.main.run` (Zell symbol convention). */
static void demangle(const char *sym, char *out, size_t cap) {
    if (strncmp(sym, "zell_", 5) == 0 && strstr(sym + 5, "__")) {
        const char *s = sym + 5;
        size_t k = 0;
        while (*s && k + 1 < cap) {
            if (s[0] == '_' && s[1] == '_') {
                out[k++] = '.';
                s += 2;
            } else {
                out[k++] = *s++;
            }
        }
        out[k] = 0;
        return;
    }
    snprintf(out, cap, "%s", sym);
}

/* ---- CPU samples -------------------------------------------------------- */

#define PROF_BUF_WORDS (1 << 20)   /* 8 MB: [n, pc0..pcn-1] records */

static uintptr_t *g_buf;
static int64_t g_used;      /* words reserved (may exceed the size: dropped) */
static int64_t g_samples, g_dropped;
static int g_running;
static int64_t g_hz, g_start_ns, g_start_wall;
static zell_string *g_path;

static void on_sample(const uintptr_t *pcs, int n) {
    uintptr_t *buf = __atomic_load_n(&g_buf, __ATOMIC_ACQUIRE);
    if (!buf) return;
    int64_t at = __atomic_fetch_add(&g_used, (int64_t)n + 1, __ATOMIC_RELAXED);
    if (at + n + 1 > PROF_BUF_WORDS) {
        __atomic_fetch_add(&g_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    for (int i = 0; i < n; i++) buf[at + 1 + i] = pcs[i];
    __atomic_store_n(&buf[at], (uintptr_t)n, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_samples, 1, __ATOMIC_RELAXED);
}

void zell_diag_rt_start_cpu_profile(const zell_string *path, int64_t hz, zell_error **err) {
    if (err) *err = NULL;
    if (g_running) {
        if (err) *err = zell_error_new_cstr("diag.StartCPUProfile: profiling already enabled");
        return;
    }
    FILE *probe = fopen(zell_string_cstr(path), "wb");
    if (!probe) {
        if (err) {
            char m[600];
            snprintf(m, sizeof m, "diag.StartCPUProfile: create %.400s: %s", zell_string_cstr(path), strerror(errno));
            *err = zell_error_new_cstr(m);
        }
        return;
    }
    fclose(probe);
    uintptr_t *buf = calloc(PROF_BUF_WORDS, sizeof *buf);
    if (!buf) {
        if (err) *err = zell_error_new_cstr("diag.StartCPUProfile: out of memory");
        return;
    }
    g_used = g_samples = g_dropped = 0;
    __atomic_store_n(&g_buf, buf, __ATOMIC_RELEASE);
    g_hz = hz > 0 ? hz : 100;
    g_start_ns = zell__plat_mono_ns();
    g_start_wall = zell__plat_wall_ns();
    zell_retain((void *)path);
    g_path = (zell_string *)path;
    if (!zell__plat_prof_start((int)g_hz, on_sample)) {
        __atomic_store_n(&g_buf, (uintptr_t *)NULL, __ATOMIC_RELEASE);
        free(buf);
        zell_release(g_path);
        g_path = NULL;
        if (err) *err = zell_error_new_cstr("diag.StartCPUProfile: CPU profiling is not supported on this platform");
        return;
    }
    g_running = 1;
}

/* Distinct addresses -> location ids (open addressing). */
typedef struct { uintptr_t addr; uint64_t id; } loc_slot;
typedef struct { loc_slot *s; size_t cap, n; } loc_map;

static uint64_t loc_id(loc_map *m, uintptr_t addr) {
    if (m->n * 2 >= m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 1024;
        loc_slot *ns = calloc(cap, sizeof *ns);
        if (!ns) return 0;
        for (size_t i = 0; i < m->cap; i++)
            if (m->s[i].id) {
                size_t h = (m->s[i].addr * 0x9E3779B97F4A7C15ull) & (cap - 1);
                while (ns[h].id) h = (h + 1) & (cap - 1);
                ns[h] = m->s[i];
            }
        free(m->s);
        m->s = ns;
        m->cap = cap;
    }
    size_t h = (addr * 0x9E3779B97F4A7C15ull) & (m->cap - 1);
    while (m->s[h].id) {
        if (m->s[h].addr == addr) return m->s[h].id;
        h = (h + 1) & (m->cap - 1);
    }
    m->s[h].addr = addr;
    m->s[h].id = ++m->n;
    return m->s[h].id;
}

/* One function per distinct symbol name. */
typedef struct { int64_t name; } fn_ent;

static bool write_file(const zell_string *path, const pbuf *b, zell_error **err, const char *what) {
    FILE *f = fopen(zell_string_cstr(path), "wb");
    if (!f || b->oom || fwrite(b->p, 1, b->len, f) != b->len) {
        if (f) fclose(f);
        if (err) {
            char m[600];
            snprintf(m, sizeof m, "%s: write %.400s: %s", what, zell_string_cstr(path), b->oom ? "out of memory" : strerror(errno));
            *err = zell_error_new_cstr(m);
        }
        return false;
    }
    fclose(f);
    return true;
}

/* Appends the shared tail of a profile: mapping, locations, functions. */
static void emit_locations(pbuf *out, strtab *st, const loc_map *lm, bool symbolize) {
    uintptr_t lo = 0, hi = 0;
    char path[512] = "";
    bool have_map = symbolize && zell__plat_exe_range(&lo, &hi, path, sizeof path);
    if (have_map) {
        pbuf m = {0};
        pb_uint(&m, 1, 1);
        pb_uint(&m, 2, lo);
        pb_uint(&m, 3, hi);
        pb_uint(&m, 5, (uint64_t)st_add(st, path));
        pb_uint(&m, 7, 1); /* has_functions */
        pb_sub(out, 3, &m);
        free(m.p);
    }
    /* Functions are keyed by name index: function id = name index. */
    int64_t *fn_seen = NULL;
    size_t fn_cap = 0;
    for (size_t i = 0; i < lm->cap; i++) {
        const loc_slot *s = &lm->s[i];
        if (!s->id) continue;
        char sym[512], name[512];
        uintptr_t start = 0;
        if (symbolize) {
            if (!zell__plat_symbolize(s->addr, sym, sizeof sym, &start))
                snprintf(sym, sizeof sym, "0x%llx", (unsigned long long)s->addr);
            demangle(sym, name, sizeof name);
        } else {
            /* heap pseudo-frames: the address is a string table index */
            snprintf(name, sizeof name, "%s", st->v[s->addr]);
        }
        int64_t ni = st_add(st, name);
        pbuf l = {0}, ln = {0};
        pb_uint(&l, 1, s->id);
        if (have_map && s->addr >= lo && s->addr < hi) pb_uint(&l, 2, 1);
        if (symbolize) pb_uint(&l, 3, s->addr);
        pb_uint(&ln, 1, (uint64_t)ni);
        pb_sub(&l, 4, &ln);
        pb_sub(out, 4, &l);
        free(l.p);
        free(ln.p);
        if ((size_t)ni >= fn_cap) {
            size_t nc = fn_cap ? fn_cap : 64;
            while (nc <= (size_t)ni) nc *= 2;
            int64_t *nf = realloc(fn_seen, nc * sizeof *nf);
            if (!nf) continue;
            memset(nf + fn_cap, 0, (nc - fn_cap) * sizeof *nf);
            fn_seen = nf;
            fn_cap = nc;
        }
        if (!fn_seen[ni]) {
            fn_seen[ni] = 1;
            pbuf f = {0};
            pb_uint(&f, 1, (uint64_t)ni);
            pb_uint(&f, 2, (uint64_t)ni);
            pb_uint(&f, 3, (uint64_t)ni);
            pb_sub(out, 5, &f);
            free(f.p);
        }
    }
    free(fn_seen);
}

static void emit_value_type(pbuf *out, int field, strtab *st, const char *type, const char *unit) {
    pbuf v = {0};
    pb_uint(&v, 1, (uint64_t)st_add(st, type));
    pb_uint(&v, 2, (uint64_t)st_add(st, unit));
    pb_sub(out, field, &v);
    free(v.p);
}

static void emit_strings(pbuf *out, const strtab *st) {
    for (int64_t i = 0; i < st->n; i++) pb_str(out, 6, st->v[i]);
}

int64_t zell_diag_rt_stop_cpu_profile(zell_error **err) {
    if (err) *err = NULL;
    if (!g_running) {
        if (err) *err = zell_error_new_cstr("diag.StopCPUProfile: profiling is not enabled");
        return 0;
    }
    zell__plat_prof_stop();
    g_running = 0;
    uintptr_t *buf = __atomic_exchange_n(&g_buf, (uintptr_t *)NULL, __ATOMIC_ACQ_REL);
    int64_t used = g_used < PROF_BUF_WORDS ? g_used : PROF_BUF_WORDS;
    int64_t dur = zell__plat_mono_ns() - g_start_ns;
    int64_t period = 1000000000 / g_hz;

    strtab st = {0};
    st_add(&st, "");
    pbuf out = {0}, samples = {0};
    loc_map lm = {0};
    emit_value_type(&out, 1, &st, "samples", "count");
    emit_value_type(&out, 1, &st, "cpu", "nanoseconds");
    int64_t count = 0;
    for (int64_t at = 0; at < used;) {
        int64_t n = (int64_t)__atomic_load_n(&buf[at], __ATOMIC_ACQUIRE);
        if (n <= 0 || at + 1 + n > used) break; /* torn record at the end */
        pbuf s = {0}, ids = {0}, vals = {0};
        for (int64_t i = 0; i < n; i++) {
            uintptr_t a = buf[at + 1 + i];
            if (i > 0) a -= 1; /* return address -> call instruction */
            pb_varint(&ids, loc_id(&lm, a));
        }
        pb_varint(&vals, 1);
        pb_varint(&vals, (uint64_t)period);
        pb_sub(&s, 1, &ids);
        pb_sub(&s, 2, &vals);
        pb_sub(&samples, 2, &s);
        free(s.p);
        free(ids.p);
        free(vals.p);
        count++;
        at += n + 1;
    }
    pb_bytes(&out, samples.p, samples.len);
    free(samples.p);
    emit_locations(&out, &st, &lm, true);
    zell__plat_symbolize_done();
    pb_uint(&out, 9, (uint64_t)g_start_wall);
    pb_uint(&out, 10, (uint64_t)dur);
    emit_value_type(&out, 11, &st, "cpu", "nanoseconds");
    pb_uint(&out, 12, (uint64_t)period);
    pbuf final = {0};
    /* The string table must be complete: emit it last (field order does
     * not matter in protobuf). */
    pb_bytes(&final, out.p, out.len);
    emit_strings(&final, &st);
    write_file(g_path, &final, err, "diag.StopCPUProfile");
    free(out.p);
    free(final.p);
    free(lm.s);
    st_free(&st);
    free(buf);
    zell_release(g_path);
    g_path = NULL;
    return count;
}

/* ---- heap ------------------------------------------------------------ */

typedef struct { const char *name; int64_t n, bytes; } heap_ent;
typedef struct { heap_ent *v; int64_t n, cap; } heap_tab;

static void heap_cb(const char *type, int64_t bytes, void *arg) {
    heap_tab *t = arg;
    for (int64_t i = 0; i < t->n; i++)
        if (strcmp(t->v[i].name, type) == 0) {
            t->v[i].n++;
            t->v[i].bytes += bytes;
            return;
        }
    if (t->n == t->cap) {
        int64_t cap = t->cap ? t->cap * 2 : 32;
        heap_ent *nv = realloc(t->v, (size_t)cap * sizeof *nv);
        if (!nv) return;
        t->v = nv;
        t->cap = cap;
    }
    t->v[t->n++] = (heap_ent){ type, 1, bytes };
}

static int heap_cmp(const void *a, const void *b) {
    const heap_ent *x = a, *y = b;
    if (x->n != y->n) return x->n < y->n ? 1 : -1;
    return strcmp(x->name, y->name);
}

static bool heap_collect(heap_tab *t) {
    if (!zell__heap_each(heap_cb, t)) return false;
    if (t->n) qsort(t->v, (size_t)t->n, sizeof *t->v, heap_cmp);
    return true;
}

/* "N TYPE BYTES" lines, most frequent first; "" without type tracking. */
zell_string *zell_diag_rt_heap_text(void) {
    heap_tab t = {0};
    if (!heap_collect(&t)) return zell__string_alloc(0);
    size_t cap = 64 + (size_t)t.n * 96, len = 0;
    char *s = malloc(cap);
    if (!s) {
        free(t.v);
        return zell__string_alloc(0);
    }
    for (int64_t i = 0; i < t.n; i++) {
        int w = snprintf(s + len, cap - len, "%lld %s %lld\n", (long long)t.v[i].n, t.v[i].name, (long long)t.v[i].bytes);
        if (w < 0 || (size_t)w >= cap - len) break;
        len += (size_t)w;
    }
    zell_string *r = zell_string_new(s, (int64_t)len);
    free(s);
    free(t.v);
    return r;
}

void zell_diag_rt_write_heap_profile(const zell_string *path, zell_error **err) {
    if (err) *err = NULL;
    heap_tab t = {0};
    if (!heap_collect(&t)) {
        if (err) *err = zell_error_new_cstr("diag.WriteHeapProfile: needs object tracking (run with --leak-check, without --release)");
        return;
    }
    strtab st = {0};
    st_add(&st, "");
    pbuf out = {0};
    loc_map lm = {0};
    emit_value_type(&out, 1, &st, "inuse_objects", "count");
    emit_value_type(&out, 1, &st, "inuse_space", "bytes");
    for (int64_t i = 0; i < t.n; i++) {
        int64_t ni = st_add(&st, t.v[i].name);
        pbuf s = {0}, ids = {0}, vals = {0};
        pb_varint(&ids, loc_id(&lm, (uintptr_t)ni));
        pb_varint(&vals, (uint64_t)t.v[i].n);
        pb_varint(&vals, (uint64_t)t.v[i].bytes);
        pb_sub(&s, 1, &ids);
        pb_sub(&s, 2, &vals);
        pb_sub(&out, 2, &s);
        free(s.p);
        free(ids.p);
        free(vals.p);
    }
    emit_locations(&out, &st, &lm, false);
    pb_uint(&out, 9, (uint64_t)zell__plat_wall_ns());
    emit_value_type(&out, 11, &st, "space", "bytes");
    pbuf final = {0};
    pb_bytes(&final, out.p, out.len);
    emit_strings(&final, &st);
    write_file(path, &final, err, "diag.WriteHeapProfile");
    free(out.p);
    free(final.p);
    free(lm.s);
    free(t.v);
    st_free(&st);
}
