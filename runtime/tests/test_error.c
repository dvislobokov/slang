#include "test.h"

typedef struct { zell_object hdr; zell_string *path; } not_found;
static void nf_drop(void *o) { zell_release(((not_found *)o)->path); }
static const zell_type_info nf_type = { "NotFound", sizeof(not_found), nf_drop };
static zell_string *nf_message(void *payload) {
    zell_string *pre = S("not found: ");
    zell_string *r = zell_string_concat(pre, ((not_found *)payload)->path);
    zell_release(pre);
    return r;
}

TEST(basic) {
    zell_string *msg = S("boom");
    zell_error *e = zell_error_new(msg);
    zell_string *m = zell_error_message(e);
    assert(m == msg && zell_refcount(msg) == 3);
    zell_release(m);
    zell_release(e);
    assert(zell_refcount(msg) == 1);
    zell_release(msg);
    zell_string *nil = zell_error_message(NULL);
    assert(STR_IS(nil, "<nil>"));
    zell_release(nil);
}

TEST(wrap_chain) {
    zell_string *ctx1 = S("loading config"), *ctx2 = S("startup");
    zell_error *base = zell_error_new_cstr("file missing");
    zell_error *w1 = zell_error_wrap(base, ctx1);
    zell_error *w2 = zell_error_wrap(w1, ctx2);
    zell_string *m = zell_error_message(w2);
    assert(STR_IS(m, "startup: loading config: file missing"));
    zell_release(m);
    zell_error *u = zell_error_unwrap(w2);
    assert(u == w1);
    zell_release(u);
    assert(zell_error_unwrap(base) == NULL);
    assert(zell_error_wrap(NULL, ctx1) == NULL);
    assert(zell_error_is(w2, base) && zell_error_is(w2, w1) && zell_error_is(w2, w2));
    zell_error *other = zell_error_new_cstr("file missing");
    assert(!zell_error_is(w2, other));  /* identity, not text */
    assert(zell_error_is(NULL, NULL) && !zell_error_is(NULL, base) && !zell_error_is(base, NULL));
    zell_release(other);
    zell_release(base);
    zell_release(w1);
    zell_release(w2);   /* frees the whole chain */
    zell_release(ctx1);
    zell_release(ctx2);
}

TEST(sentinel) {
    zell_string *msg = S("not found");
    zell_error *ErrNotFound = zell_error_sentinel(msg);
    zell_release(msg);
    zell_string *c = S("open a.txt");
    zell_error *w = zell_error_wrap(ErrNotFound, c);
    assert(zell_error_is(w, ErrNotFound));
    zell_release(w);
    zell_release(ErrNotFound);  /* no-op */
    assert(zell_refcount(ErrNotFound) == ZELL_RC_IMMORTAL);
    zell_release(c);
    zell_string *eof = zell_error_message(zell_err_eof);
    assert(STR_IS(eof, "EOF"));
    zell_release(eof);
}

TEST(typed) {
    not_found *nf = zell_object_new(&nf_type, sizeof(not_found));
    nf->path = S("a.txt");
    zell_error *e = zell_error_new_typed(&nf_type, nf, nf_message);
    zell_release(nf);  /* error holds the payload now */
    zell_string *c = S("open");
    zell_error *w = zell_error_wrap(e, c);
    zell_string *m = zell_error_message(w);
    assert(STR_IS(m, "open: not found: a.txt"));
    not_found *got = zell_error_as(w, &nf_type);
    assert(got && STR_IS(got->path, "a.txt"));
    assert(zell_error_as(w, &zell_type_string) == NULL);
    zell_release(m); zell_release(c); zell_release(e); zell_release(w);
}

static const zell_error_site site_open = { "app/fs.zl", "Open", 42 };
static const zell_error_site site_cfg = { "app/config.zl", "ReadConfig", 17 };
static const zell_error_site site_main = { "app/main.zl", "main", 9 };

TEST(trace) {
    zell_error *e = zell_error_created(zell_error_new_cstr("not found"), &site_open);
    assert(e->created == &site_open);
    assert(zell_error_created(e, &site_main) == e && e->created == &site_open); /* first site wins */
    zell_string *ctx = S("config");
    e = zell_error_returned(e, &site_cfg, ctx);
    e = zell_error_returned(e, &site_main, NULL);
    zell_string *m = zell_error_message(e);
    assert(STR_IS(m, "config: not found"));
    zell_string *t = zell_error_trace(e);
    assert(STR_IS(t, "  created at   app/fs.zl:42      Open\n"
                     "  returned via app/config.zl:17  ReadConfig  ?(\"config\")\n"
                     "  returned via app/main.zl:9     main"));
    zell_slice fr;
    zell_error_frames(e, &fr);
    assert(fr.len == 3);
    struct { zell_string *file; int64_t line; zell_string *func; zell_string *ctx; } *f = fr.data;
    assert(STR_IS(f[1].func, "ReadConfig") && f[1].line == 17 && STR_IS(f[1].ctx, "config") && f[2].ctx == NULL);
    zell_slice_release(&fr);
    zell_release(t); zell_release(m); zell_release(ctx); zell_release(e);
    zell_string *empty = zell_error_trace(NULL);
    assert(zell_string_len(empty) == 0);
    zell_release(empty);
}

TEST(trace_cow_and_overflow) {
    zell_error *e = zell_error_new_cstr("x");
    zell_retain(e);                      /* shared: the trace must not change for the other owner */
    zell_error *c = zell_error_returned(e, &site_main, NULL);
    assert(c != e && e->nframes == 0 && c->nframes == 1 && c->origin == e);
    assert(zell_error_is(c, e));         /* a copy keeps the identity of its origin */
    for (int i = 0; i < 20; i++) c = zell_error_returned(c, &site_cfg, NULL);
    assert(c->nframes == 21);
    zell_string *t = zell_error_trace(c);
    assert(strstr(zell_string_data(t), "  returned via app/main.zl:9     main\n"));
    zell_release(t);
    zell_release(c);
    zell_release(e);
    /* immortal sentinels are copied too */
    zell_error *s = zell_error_returned(zell_err_eof, &site_main, NULL);
    assert(s != zell_err_eof && zell_error_is(s, zell_err_eof));
    zell_release(s);
}

TEST(errno_sentinel) {
    zell_error *e = zell_error_errno("open", "/x/y", 2 /* ENOENT */);
    assert(zell_error_is(e, zell_fs_err_not_found()));
    zell_string *m = zell_error_message(e);
    assert(STR_IS(m, "open /x/y: No such file or directory"));
    zell_release(m);
    zell_release(e);
    zell_error *p = zell_error_errno("open", "/x", 13 /* EACCES */);
    assert(!zell_error_is(p, zell_fs_err_not_found()));
    zell_release(p);
}

int main(void) {
    printf("test_error\n");
    int64_t live = zell_live_objects();
    RUN(basic);
    RUN(wrap_chain);
    RUN(typed);
    RUN(trace);
    RUN(trace_cow_and_overflow);
    RUN(errno_sentinel);
    int64_t live2 = zell_live_objects();
    RUN(sentinel);
    assert(live2 == live);
    assert(zell_live_objects() == live);  /* immortal sentinels and their messages do not count */
    return 0;
}
