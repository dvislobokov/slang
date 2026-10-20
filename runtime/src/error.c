/* error.c - boxed error values with wrapping and typed payloads. */
#include "rt_internal.h"

#include <string.h>

static void error_drop(void *obj) {
    slang_error *e = obj;
    slang_release(e->message);
    slang_release(e->inner);
    slang_release(e->payload);
}

const slang_type_info slang_type_error = { "error", (int64_t)sizeof(slang_error), error_drop };

static struct {
    slang_object hdr; int64_t len; char data[4];
} eof_msg = { { SLANG_RC_IMMORTAL, &slang_type_string }, 3, "EOF" };

static slang_error eof_error = {
    { SLANG_RC_IMMORTAL, &slang_type_error }, (slang_string *)&eof_msg, NULL, NULL, NULL, NULL
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
    slang_string *m = (msg && msg->hdr.refcount >= 0)
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
    static struct { slang_object hdr; int64_t len; char data[3]; } sep =
        { { SLANG_RC_IMMORTAL, &slang_type_string }, 2, ": " };
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

bool slang_error_is(const slang_error *err, const slang_error *target) {
    if (!target) return err == NULL;
    for (const slang_error *e = err; e; e = e->inner)
        if (e == target) return true;
    return false;
}

void *slang_error_as(const slang_error *err, const slang_type_info *type) {
    for (const slang_error *e = err; e; e = e->inner)
        if (e->user_type && e->user_type == type) return e->payload;
    return NULL;
}
