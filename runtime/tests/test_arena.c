#include "test.h"

static int drops;
static zell_string *held;
static void node_drop(void *obj) {
    drops++;
    zell_string **s = (zell_string **)((char *)obj + 16);
    zell_release(*s);
}
static const zell_type_info node_type = { "Node", 24, node_drop };

TEST(objects_freed_at_once) {
    int64_t live = zell_live_objects();
    int64_t a = zell_arena_create();
    held = S("held by an arena object");
    for (int i = 0; i < 5000; i++) {
        zell_object *o = zell_arena_object(a, &node_type, 24);
        assert(o->refcount == ZELL_RC_ARENA && o->type == &node_type);
        assert(*(zell_string **)((char *)o + 16) == NULL);
        zell_retain(o);   /* no-ops */
        zell_release(o);
        zell_retain(held);
        *(zell_string **)((char *)o + 16) = held;
    }
    /* A big object gets its own chunk. */
    unsigned char *big = zell_arena_object(a, NULL, 1 << 20);
    assert(big[100] == 0);
    assert(zell_arena_objects(a) == 5001);
    assert(zell_live_objects() == live + 1);  /* only `held` */
    zell_arena_free(a);
    assert(drops == 5000 && zell_arena_objects(a) == 0);
    assert(zell_refcount(held) == 1);
    /* Still usable after Free. */
    zell_object *o = zell_arena_object(a, NULL, 32);
    assert(o->refcount == ZELL_RC_ARENA);
    zell_arena_destroy(a);
    zell_release(held);
    assert(zell_live_objects() == live);
}

TEST(slices_and_strings) {
    int64_t a = zell_arena_create();
    zell_slice s;
    zell_slice_new_ops(&s, 8, 0, 0, NULL);
    zell_arena_slice(&s, a, 8, 10, &zell_ops_ref);
    assert(s.len == 10 && s.cap == 10 && s.backing->refcount == ZELL_RC_ARENA);
    zell_string *x = S("element");
    ((zell_string **)s.data)[3] = x;  /* the slice owns it now */
    zell_string *c = zell_arena_string(a, x);
    assert(c != x && STR_IS(c, "element") && zell_refcount(c) == ZELL_RC_ARENA);
    zell_slice_release(&s);  /* no-op on the backing */
    int64_t live = zell_live_objects();
    zell_arena_destroy(a);   /* releases x through the element ops */
    assert(zell_live_objects() == live - 1);
}

int main(void) {
    RUN(objects_freed_at_once);
    RUN(slices_and_strings);
    return 0;
}
