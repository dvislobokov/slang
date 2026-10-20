#include "test.h"

static int drops;
static void counting_drop(void *obj) { (void)obj; drops++; }
static const slang_type_info counted = { "Counted", 24, counting_drop };

TEST(alloc_zeroed) {
    unsigned char *p = slang_alloc(1000);
    for (int i = 0; i < 1000; i++) assert(p[i] == 0);
    p = slang_realloc(p, 5000);
    slang_free(p);
    slang_free(NULL);
}

TEST(refcount_basic) {
    int64_t live = slang_live_objects();
    slang_object *o = slang_object_new(&counted, 24);
    assert(o->refcount == 1 && o->type == &counted);
    assert(slang_live_objects() == live + 1);
    slang_retain(o);
    assert(slang_refcount(o) == 2);
    slang_release(o);
    assert(drops == 0);
    slang_release(o);
    assert(drops == 1);
    assert(slang_live_objects() == live);
    slang_retain(NULL);
    slang_release(NULL);
    assert(slang_refcount(NULL) == 0);
}

TEST(immortal) {
    static slang_object s = { SLANG_RC_IMMORTAL, &counted };
    slang_retain(&s);
    slang_release(&s);
    slang_release(&s);
    assert(s.refcount == SLANG_RC_IMMORTAL);
    assert(drops == 1);
}

#include <pthread.h>
static slang_object *shared;
static void *hammer(void *arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) { slang_retain(shared); slang_release(shared); }
    return NULL;
}

TEST(atomic_refcount) {
    shared = slang_object_new(&counted, 24);
    pthread_t th[8];
    for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, hammer, NULL);
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    assert(slang_refcount(shared) == 1);
    slang_release(shared);
    assert(drops == 2);
}

TEST(layout) {
    assert(sizeof(slang_object) == 16);
    assert(sizeof(slang_type_info) == 24);
    assert(sizeof(slang_value_ops) == 16);
    assert(sizeof(slang_slice) == 32);
    assert(sizeof(slang_map_desc) == 48);
    assert(sizeof(slang_map_iter) == 32);
    assert(sizeof(slang_error) == 56);
    assert(sizeof(slang_string) == 24);
    assert(offsetof(slang_string, len) == 16 && offsetof(slang_string, data) == 24);
    assert(offsetof(slang_slice, data) == 0 && offsetof(slang_slice, len) == 8);
    assert(offsetof(slang_slice, cap) == 16 && offsetof(slang_slice, backing) == 24);
    assert(offsetof(slang_error, message) == 16 && offsetof(slang_error, inner) == 24);
    assert(offsetof(slang_error, user_type) == 32 && offsetof(slang_error, payload) == 40);
    assert(offsetof(slang_error, message_fn) == 48);
}

int main(void) {
    printf("test_mem\n");
    RUN(alloc_zeroed);
    RUN(refcount_basic);
    RUN(immortal);
    RUN(atomic_refcount);
    RUN(layout);
    return 0;
}
