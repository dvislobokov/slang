#include "test.h"

typedef struct { slang_object hdr; slang_string *path; } not_found;
static void nf_drop(void *o) { slang_release(((not_found *)o)->path); }
static const slang_type_info nf_type = { "NotFound", sizeof(not_found), nf_drop };
static slang_string *nf_message(void *payload) {
    slang_string *pre = S("not found: ");
    slang_string *r = slang_string_concat(pre, ((not_found *)payload)->path);
    slang_release(pre);
    return r;
}

TEST(basic) {
    slang_string *msg = S("boom");
    slang_error *e = slang_error_new(msg);
    slang_string *m = slang_error_message(e);
    assert(m == msg && slang_refcount(msg) == 3);
    slang_release(m);
    slang_release(e);
    assert(slang_refcount(msg) == 1);
    slang_release(msg);
    slang_string *nil = slang_error_message(NULL);
    assert(STR_IS(nil, "<nil>"));
    slang_release(nil);
}

TEST(wrap_chain) {
    slang_string *ctx1 = S("loading config"), *ctx2 = S("startup");
    slang_error *base = slang_error_new_cstr("file missing");
    slang_error *w1 = slang_error_wrap(base, ctx1);
    slang_error *w2 = slang_error_wrap(w1, ctx2);
    slang_string *m = slang_error_message(w2);
    assert(STR_IS(m, "startup: loading config: file missing"));
    slang_release(m);
    slang_error *u = slang_error_unwrap(w2);
    assert(u == w1);
    slang_release(u);
    assert(slang_error_unwrap(base) == NULL);
    assert(slang_error_wrap(NULL, ctx1) == NULL);
    assert(slang_error_is(w2, base) && slang_error_is(w2, w1) && slang_error_is(w2, w2));
    slang_error *other = slang_error_new_cstr("file missing");
    assert(!slang_error_is(w2, other));  /* identity, not text */
    assert(slang_error_is(NULL, NULL) && !slang_error_is(NULL, base) && !slang_error_is(base, NULL));
    slang_release(other);
    slang_release(base);
    slang_release(w1);
    slang_release(w2);   /* frees the whole chain */
    slang_release(ctx1);
    slang_release(ctx2);
}

TEST(sentinel) {
    slang_string *msg = S("not found");
    slang_error *ErrNotFound = slang_error_sentinel(msg);
    slang_release(msg);
    slang_string *c = S("open a.txt");
    slang_error *w = slang_error_wrap(ErrNotFound, c);
    assert(slang_error_is(w, ErrNotFound));
    slang_release(w);
    slang_release(ErrNotFound);  /* no-op */
    assert(slang_refcount(ErrNotFound) == SLANG_RC_IMMORTAL);
    slang_release(c);
    slang_string *eof = slang_error_message(slang_err_eof);
    assert(STR_IS(eof, "EOF"));
    slang_release(eof);
}

TEST(typed) {
    not_found *nf = slang_object_new(&nf_type, sizeof(not_found));
    nf->path = S("a.txt");
    slang_error *e = slang_error_new_typed(&nf_type, nf, nf_message);
    slang_release(nf);  /* error holds the payload now */
    slang_string *c = S("open");
    slang_error *w = slang_error_wrap(e, c);
    slang_string *m = slang_error_message(w);
    assert(STR_IS(m, "open: not found: a.txt"));
    not_found *got = slang_error_as(w, &nf_type);
    assert(got && STR_IS(got->path, "a.txt"));
    assert(slang_error_as(w, &slang_type_string) == NULL);
    slang_release(m); slang_release(c); slang_release(e); slang_release(w);
}

static const slang_error_site site_open = { "app/fs.sl", "Open", 42 };
static const slang_error_site site_cfg = { "app/config.sl", "ReadConfig", 17 };
static const slang_error_site site_main = { "app/main.sl", "main", 9 };

TEST(trace) {
    slang_error *e = slang_error_created(slang_error_new_cstr("not found"), &site_open);
    assert(e->created == &site_open);
    assert(slang_error_created(e, &site_main) == e && e->created == &site_open); /* first site wins */
    slang_string *ctx = S("config");
    e = slang_error_returned(e, &site_cfg, ctx);
    e = slang_error_returned(e, &site_main, NULL);
    slang_string *m = slang_error_message(e);
    assert(STR_IS(m, "config: not found"));
    slang_string *t = slang_error_trace(e);
    assert(STR_IS(t, "  created at   app/fs.sl:42      Open\n"
                     "  returned via app/config.sl:17  ReadConfig  ?(\"config\")\n"
                     "  returned via app/main.sl:9     main"));
    slang_slice fr;
    slang_error_frames(e, &fr);
    assert(fr.len == 3);
    struct { slang_string *file; int64_t line; slang_string *func; slang_string *ctx; } *f = fr.data;
    assert(STR_IS(f[1].func, "ReadConfig") && f[1].line == 17 && STR_IS(f[1].ctx, "config") && f[2].ctx == NULL);
    slang_slice_release(&fr);
    slang_release(t); slang_release(m); slang_release(ctx); slang_release(e);
    slang_string *empty = slang_error_trace(NULL);
    assert(slang_string_len(empty) == 0);
    slang_release(empty);
}

TEST(trace_cow_and_overflow) {
    slang_error *e = slang_error_new_cstr("x");
    slang_retain(e);                      /* shared: the trace must not change for the other owner */
    slang_error *c = slang_error_returned(e, &site_main, NULL);
    assert(c != e && e->nframes == 0 && c->nframes == 1 && c->origin == e);
    assert(slang_error_is(c, e));         /* a copy keeps the identity of its origin */
    for (int i = 0; i < 20; i++) c = slang_error_returned(c, &site_cfg, NULL);
    assert(c->nframes == 21);
    slang_string *t = slang_error_trace(c);
    assert(strstr(slang_string_data(t), "  returned via app/main.sl:9     main\n"));
    slang_release(t);
    slang_release(c);
    slang_release(e);
    /* immortal sentinels are copied too */
    slang_error *s = slang_error_returned(slang_err_eof, &site_main, NULL);
    assert(s != slang_err_eof && slang_error_is(s, slang_err_eof));
    slang_release(s);
}

TEST(errno_sentinel) {
    slang_error *e = slang_error_errno("open", "/x/y", 2 /* ENOENT */);
    assert(slang_error_is(e, slang_fs_err_not_found()));
    slang_string *m = slang_error_message(e);
    assert(STR_IS(m, "open /x/y: No such file or directory"));
    slang_release(m);
    slang_release(e);
    slang_error *p = slang_error_errno("open", "/x", 13 /* EACCES */);
    assert(!slang_error_is(p, slang_fs_err_not_found()));
    slang_release(p);
}

int main(void) {
    printf("test_error\n");
    int64_t live = slang_live_objects();
    RUN(basic);
    RUN(wrap_chain);
    RUN(typed);
    RUN(trace);
    RUN(trace_cow_and_overflow);
    RUN(errno_sentinel);
    int64_t live2 = slang_live_objects();
    RUN(sentinel);
    assert(live2 == live);
    assert(slang_live_objects() == live);  /* immortal sentinels and their messages do not count */
    return 0;
}
