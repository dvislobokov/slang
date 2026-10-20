#include "test.h"

static int drops;
static slang_string *held;
static void node_drop(void *obj) {
    drops++;
    slang_string **s = (slang_string **)((char *)obj + 16);
    slang_release(*s);
}
static const slang_type_info node_type = { "Node", 24, node_drop };

TEST(objects_freed_at_once) {
    int64_t live = slang_live_objects();
    int64_t a = slang_arena_create();
    held = S("held by an arena object");
    for (int i = 0; i < 5000; i++) {
        slang_object *o = slang_arena_object(a, &node_type, 24);
        assert(o->refcount == SLANG_RC_ARENA && o->type == &node_type);
        assert(*(slang_string **)((char *)o + 16) == NULL);
        slang_retain(o);   /* no-ops */
        slang_release(o);
        slang_retain(held);
        *(slang_string **)((char *)o + 16) = held;
    }
    /* A big object gets its own chunk. */
    unsigned char *big = slang_arena_object(a, NULL, 1 << 20);
    assert(big[100] == 0);
    assert(slang_arena_objects(a) == 5001);
    assert(slang_live_objects() == live + 1);  /* only `held` */
    slang_arena_free(a);
    assert(drops == 5000 && slang_arena_objects(a) == 0);
    assert(slang_refcount(held) == 1);
    /* Still usable after Free. */
    slang_object *o = slang_arena_object(a, NULL, 32);
    assert(o->refcount == SLANG_RC_ARENA);
    slang_arena_destroy(a);
    slang_release(held);
    assert(slang_live_objects() == live);
}

TEST(slices_and_strings) {
    int64_t a = slang_arena_create();
    slang_slice s;
    slang_slice_new_ops(&s, 8, 0, 0, NULL);
    slang_arena_slice(&s, a, 8, 10, &slang_ops_ref);
    assert(s.len == 10 && s.cap == 10 && s.backing->refcount == SLANG_RC_ARENA);
    slang_string *x = S("element");
    ((slang_string **)s.data)[3] = x;  /* the slice owns it now */
    slang_string *c = slang_arena_string(a, x);
    assert(c != x && STR_IS(c, "element") && slang_refcount(c) == SLANG_RC_ARENA);
    slang_slice_release(&s);  /* no-op on the backing */
    int64_t live = slang_live_objects();
    slang_arena_destroy(a);   /* releases x through the element ops */
    assert(slang_live_objects() == live - 1);
}

int main(void) {
    RUN(objects_freed_at_once);
    RUN(slices_and_strings);
    return 0;
}
