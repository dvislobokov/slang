/* error.c - boxed error values with wrapping, typed payloads and return
 * traces (ABI.md section 7). */
#include "rt_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void frames_release(slang_error *e) {
    int64_t n = e->nframes;
    for (int64_t i = 0; i < n; i++) {
        slang_error_frame *f = i < SLANG_ERROR_INLINE_FRAMES ? &e->frames[i] : &e->overflow[i - SLANG_ERROR_INLINE_FRAMES];
        slang_release(f->ctx);
    }
    slang_free(e->overflow);
}

static void error_drop(void *obj) {
    slang_error *e = obj;
    slang_release(e->message);
    slang_release(e->inner);
    slang_release(e->payload);
    slang_release(e->origin);
    frames_release(e);
}

const slang_type_info slang_type_error = { "error", (int64_t)sizeof(slang_error), error_drop };

static SLANG_STRING_STORAGE(4) eof_msg = SLANG_STRING_INIT(eof_msg, "EOF");

static slang_error eof_error = {
    .hdr = { SLANG_RC_IMMORTAL, &slang_type_error }, .message = (slang_string *)&eof_msg,
};
slang_error *const slang_err_eof = &eof_error;

static slang_error *error_alloc(void) {
    return slang_object_new(&slang_type_error, (int64_t)sizeof(slang_error));
}

slang_error *slang_error_new(const slang_string *msg) {
    slang_error *e = error_alloc();
    slang_retain((void *)msg);
    e->message = (slang_string *)msg;
    return e;
}

slang_error *slang_error_new_cstr(const char *msg) {
    slang_error *e = error_alloc();
    e->message = slang_string_from_cstr(msg);
    return e;
}

slang_error *slang_error_sentinel(const slang_string *msg) {
    /* The sentinel and its message (an immortal copy) are never freed and
     * do not count as live objects. */
    slang_string *m = (msg && msg->hdr.refcount != SLANG_RC_IMMORTAL)
                          ? slang_string_literal(slang_string_data(msg), slang_string_len(msg))
                          : (slang_string *)msg;
    slang_error *e = slang_error_new(m);
    slang__object_immortalize(&e->hdr);
    return e;
}

slang_error *slang_error_new_typed(const slang_type_info *type, void *payload,
                                   slang_message_fn message_fn) {
    slang_error *e = error_alloc();
    e->user_type = type;
    slang_retain(payload);
    e->payload = payload;
    e->message_fn = message_fn;
    return e;
}

slang_error *slang_error_wrap(slang_error *inner, const slang_string *ctx) {
    if (!inner) return NULL;
    slang_error *e = error_alloc();
    slang_retain((void *)ctx);
    e->message = (slang_string *)ctx;
    slang_retain(inner);
    e->inner = inner;
    return e;
}

slang_error *slang_error_unwrap(const slang_error *err) {
    if (!err) return NULL;
    slang_retain(err->inner);
    return err->inner;
}

/* Own text of a single link (not including inner). Owned result. */
static slang_string *own_text(const slang_error *e) {
    if (e->message_fn) return e->message_fn(e->payload);
    slang_retain(e->message);
    return e->message;
}

slang_string *slang_error_message(const slang_error *err) {
    if (!err) return slang_string_new("<nil>", 5);
    slang_string *acc = own_text(err);
    static SLANG_STRING_STORAGE(3) sep = SLANG_STRING_INIT(sep, ": ");
    for (const slang_error *e = err->inner; e; e = e->inner) {
        slang_string *t = own_text(e);
        slang_string *a = slang_string_concat(acc, (slang_string *)&sep);
        slang_string *b = slang_string_concat(a, t);
        slang_release(a);
        slang_release(t);
        slang_release(acc);
        acc = b;
    }
    return acc;
}

/* The error a copy-on-write copy was made from (identity for errors.Is). */
static const slang_error *root_of(const slang_error *e) {
    return e->origin ? e->origin : e;
}

bool slang_error_is(const slang_error *err, const slang_error *target) {
    if (!target) return err == NULL;
    const slang_error *t = root_of(target);
    for (const slang_error *e = err; e; e = e->inner)
        if (root_of(e) == t) return true;
    return false;
}

void *slang_error_as(const slang_error *err, const slang_type_info *type) {
    for (const slang_error *e = err; e; e = e->inner)
        if (e->user_type && e->user_type == type) return e->payload;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Error traces (ABI.md 7.4)
 * ------------------------------------------------------------------------- */

static slang_error_frame *frame_at(const slang_error *e, int64_t i) {
    return i < SLANG_ERROR_INLINE_FRAMES ? (slang_error_frame *)&e->frames[i]
                                         : &e->overflow[i - SLANG_ERROR_INLINE_FRAMES];
}

static void frame_push(slang_error *e, const slang_error_site *site, const slang_string *ctx) {
    int64_t i = e->nframes;
    if (i >= SLANG_ERROR_INLINE_FRAMES) {
        int64_t k = i - SLANG_ERROR_INLINE_FRAMES;
        if (k >= e->overflow_cap) {
            int64_t cap = e->overflow_cap ? e->overflow_cap * 2 : 8;
            e->overflow = slang_realloc(e->overflow, cap * (int64_t)sizeof(slang_error_frame));
            e->overflow_cap = cap;
        }
    }
    slang_error_frame *f = frame_at(e, i);
    f->site = site;
    slang_retain((void *)ctx);
    f->ctx = (slang_string *)ctx;
    e->nframes = i + 1;
}

/* Copies the trace of src into dst (which has none). */
static void trace_copy(slang_error *dst, const slang_error *src) {
    dst->created = src->created;
    for (int64_t i = 0; i < src->nframes; i++) {
        const slang_error_frame *f = frame_at(src, i);
        frame_push(dst, f->site, f->ctx);
    }
}

/* Copy-on-write: an error that is referenced elsewhere (or immortal) is
 * copied before its trace changes. Consumes err, returns an owned error
 * that is uniquely referenced. */
static slang_error *unshare(slang_error *err) {
    if (slang_refcount(err) == 1) return err;
    slang_error *c = slang_object_new(&slang_type_error, (int64_t)sizeof(slang_error));
    slang_retain(err->message);
    c->message = err->message;
    slang_retain(err->inner);
    c->inner = err->inner;
    c->user_type = err->user_type;
    slang_retain(err->payload);
    c->payload = err->payload;
    c->message_fn = err->message_fn;
    slang_error *root = (slang_error *)root_of(err);
    slang_retain(root);
    c->origin = root;
    trace_copy(c, err);
    slang_release(err);
    return c;
}

slang_error *slang_error_created(slang_error *err, const slang_error_site *site) {
    if (!err || err->created || !site) return err;
    err = unshare(err);
    err->created = site;
    if (__atomic_load_n(&slang_dbg_on, __ATOMIC_RELAXED) && slang__dbg_error_fn) slang__dbg_error_fn(err);
    return err;
}

slang_error *slang_error_returned(slang_error *err, const slang_error_site *site, const slang_string *ctx) {
    if (!err) return NULL;
    if (ctx) {
        slang_error *w = slang_error_wrap(err, ctx);
        trace_copy(w, err);
        slang_release(err);
        err = w;
    } else {
        err = unshare(err);
    }
    frame_push(err, site, ctx);
    return err;
}

/* A growable byte buffer for building the trace text. */
typedef struct { char *p; size_t len, cap; } tbuf;

static void tb_put(tbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n + 1) cap *= 2;
        b->p = slang_realloc(b->p, (int64_t)cap);
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void tb_pad(tbuf *b, size_t n) {
    while (n--) tb_put(b, " ", 1);
}

static int loc_len(const slang_error_site *s) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, ":%lld", (long long)s->line);
    return (int)strlen(s->file ? s->file : "?") + n;
}

slang_string *slang_error_trace(const slang_error *err) {
    if (!err) return slang_string_new("", 0);
    int64_t n = err->nframes + (err->created ? 1 : 0);
    size_t maxloc = 0, maxfn = 0;
    for (int64_t i = 0; i < n; i++) {
        const slang_error_site *s = err->created ? (i == 0 ? err->created : frame_at(err, i - 1)->site)
                                                 : frame_at(err, i)->site;
        size_t l = (size_t)loc_len(s), f = strlen(s->func ? s->func : "");
        if (l > maxloc) maxloc = l;
        if (f > maxfn) maxfn = f;
    }
    tbuf b = { 0 };
    for (int64_t i = 0; i < n; i++) {
        bool created = err->created && i == 0;
        const slang_error_frame *fr = created ? NULL : frame_at(err, err->created ? i - 1 : i);
        const slang_error_site *s = created ? err->created : fr->site;
        if (i > 0) tb_put(&b, "\n", 1);
        tb_put(&b, created ? "  created at   " : "  returned via ", 15);
        char loc[4096];
        int ll = snprintf(loc, sizeof loc, "%s:%lld", s->file ? s->file : "?", (long long)s->line);
        if (ll < 0) ll = 0;
        if ((size_t)ll >= sizeof loc) ll = (int)sizeof loc - 1;
        tb_put(&b, loc, (size_t)ll);
        tb_pad(&b, maxloc - (size_t)ll + 2);
        const char *fn = s->func ? s->func : "";
        tb_put(&b, fn, strlen(fn));
        if (fr && fr->ctx) {
            tb_pad(&b, maxfn - strlen(fn) + 2);
            tb_put(&b, "?(\"", 3);
            tb_put(&b, slang_string_data(fr->ctx), (size_t)slang_string_len(fr->ctx));
            tb_put(&b, "\")", 2);
        }
        while (b.len > 0 && b.p[b.len - 1] == ' ') b.p[--b.len] = 0;
    }
    slang_string *r = slang_string_new(b.p ? b.p : "", (int64_t)b.len);
    slang_free(b.p);
    return r;
}

/* errors.Frame {File string, Line int, Func string, Context string}. */
typedef struct { slang_string *file; int64_t line; slang_string *func; slang_string *ctx; } frame_rec;

static void frame_rec_retain(void *slot) {
    frame_rec *f = slot;
    slang_retain(f->file);
    slang_retain(f->func);
    slang_retain(f->ctx);
}

static void frame_rec_release(void *slot) {
    frame_rec *f = slot;
    slang_release(f->file);
    slang_release(f->func);
    slang_release(f->ctx);
}

const slang_value_ops slang_ops_error_frame = { frame_rec_retain, frame_rec_release };

void slang_error_frames(const slang_error *err, slang_slice *out) {
    int64_t n = err ? err->nframes + (err->created ? 1 : 0) : 0;
    slang_slice_new_ops(out, (int64_t)sizeof(frame_rec), n, n, &slang_ops_error_frame);
    for (int64_t i = 0; i < n; i++) {
        bool created = err->created && i == 0;
        const slang_error_frame *fr = created ? NULL : frame_at(err, err->created ? i - 1 : i);
        const slang_error_site *s = created ? err->created : fr->site;
        frame_rec *r = (frame_rec *)out->data + i;
        r->file = slang_string_from_cstr(s->file ? s->file : "");
        r->line = s->line;
        r->func = slang_string_from_cstr(s->func ? s->func : "");
        if (fr && fr->ctx) {
            slang_retain(fr->ctx);
            r->ctx = fr->ctx;
        }
    }
}

void slang_error_report(const slang_error *err) {
    slang_string *m = slang_error_message(err);
    slang_string *t = slang_error_trace(err);
    fprintf(stderr, "error: %s\n", slang_string_cstr(m));
    if (slang_string_len(t) > 0) fprintf(stderr, "%s\n", slang_string_cstr(t));
    fflush(stderr);
    slang_release(t);
    slang_release(m);
}

/* std.fs.ErrNotFound: an immortal sentinel; ENOENT errors wrap it. */
static SLANG_STRING_STORAGE(26) not_found_msg = SLANG_STRING_INIT(not_found_msg, "No such file or directory");

static slang_error not_found_error = {
    .hdr = { SLANG_RC_IMMORTAL, &slang_type_error }, .message = (slang_string *)&not_found_msg,
};

slang_error *slang_fs_err_not_found(void) {
    return &not_found_error;
}

slang_error *slang_error_errno(const char *op, const char *path, int e) {
    char buf[1024];
    if (e == ENOENT) {
        snprintf(buf, sizeof buf, "%s %.900s", op, path ? path : "");
        slang_string *ctx = slang_string_from_cstr(buf);
        slang_error *w = slang_error_wrap(&not_found_error, ctx);
        slang_release(ctx);
        return w;
    }
    snprintf(buf, sizeof buf, "%s %.900s: %s", op, path ? path : "", strerror(e));
    return slang_error_new_cstr(buf);
}
