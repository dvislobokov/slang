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

int main(void) {
    printf("test_error\n");
    int64_t live = slang_live_objects();
    RUN(basic);
    RUN(wrap_chain);
    RUN(typed);
    int64_t live2 = slang_live_objects();
    RUN(sentinel);
    assert(live2 == live);
    assert(slang_live_objects() == live);  /* immortal sentinels and their messages do not count */
    return 0;
}
