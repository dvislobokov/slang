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
    assert(sizeof(slang_error) == 224 && sizeof(slang_error_site) == 24 && sizeof(slang_error_frame) == 16);
    assert(sizeof(slang_string) == 32);
    assert(offsetof(slang_string, len) == 16 && offsetof(slang_string, data) == 24);
    assert(offsetof(slang_slice, data) == 0 && offsetof(slang_slice, len) == 8);
    assert(offsetof(slang_slice, cap) == 16 && offsetof(slang_slice, backing) == 24);
    assert(offsetof(slang_error, message) == 16 && offsetof(slang_error, inner) == 24);
    assert(offsetof(slang_error, user_type) == 32 && offsetof(slang_error, payload) == 40);
    assert(offsetof(slang_error, message_fn) == 48 && offsetof(slang_error, origin) == 56);
    assert(offsetof(slang_error, created) == 64 && offsetof(slang_error, nframes) == 72);
    assert(offsetof(slang_error, frames) == 80 && offsetof(slang_error, overflow) == 208);
    assert(offsetof(slang_error, overflow_cap) == 216);
}

TEST(payload_pointers) {
    int64_t live = slang_live_objects();
    slang_object *o = slang_object_new(&counted, 32);
    void *payload = (char *)o + 16;
    slang_ptr_retain(payload);
    assert(slang_refcount(o) == 2);
    void *slot = payload;
    slang_ops_ptr.retain(&slot);
    assert(slang_refcount(o) == 3);
    slang_ops_ptr.release(&slot);
    slang_ptr_release(payload);
    slang_ptr_retain(NULL);
    slang_ptr_release(NULL);
    void *nil = NULL;
    slang_ops_ptr.release(&nil);
    int d = drops;
    slang_ptr_release(payload);
    assert(drops == d + 1);
    assert(slang_live_objects() == live);
}

static void *weak_seen;
static void weak_checking_drop(void *obj) {
    (void)obj;
    /* Weak refs are already cleared when the destructor runs. */
    assert(slang_weak_get(weak_seen) == NULL);
    drops++;
}
static const slang_type_info weak_counted = { "WeakCounted", 32, weak_checking_drop };

TEST(weak_refs) {
    int64_t live = slang_live_objects();
    assert(slang_weak_new(NULL) == NULL);
    assert(slang_weak_get(NULL) == NULL);
    slang_object *o = slang_object_new(&weak_counted, 32);
    void *payload = (char *)o + 16;
    void *w1 = slang_weak_new(payload);
    void *w2 = slang_weak_new(payload);
    assert(w1 && w1 == w2);              /* one shared box per object */
    assert(slang_refcount(o) == 1);      /* weak refs do not retain */
    assert(slang_refcount(w1) == 2);
    void *p = slang_weak_get(w1);
    assert(p == payload && slang_refcount(o) == 2);
    slang_ptr_release(p);
    weak_seen = w1;
    int d = drops;
    slang_release(o);                    /* last strong ref: target freed */
    assert(drops == d + 1);
    assert(slang_weak_get(w1) == NULL);
    slang_release(w1);
    slang_release(w2);
    /* Box freed first, object later: the flag is cleared again. */
    o = slang_object_new(&counted, 32);
    payload = (char *)o + 16;
    void *w = slang_weak_new(payload);
    slang_release(w);
    slang_retain(o);
    assert(slang_refcount(o) == 2);
    slang_release(o);
    slang_release(o);
    assert(slang_live_objects() == live);
}

#include <pthread.h>
static void *weak_box_shared;
static void *weak_hammer(void *arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) {
        void *p = slang_weak_get(weak_box_shared);
        if (!p) break;
        slang_ptr_release(p);
    }
    return NULL;
}

TEST(weak_threads) {
    int64_t live = slang_live_objects();
    slang_object *o = slang_object_new(&counted, 32);
    weak_box_shared = slang_weak_new((char *)o + 16);
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, weak_hammer, NULL);
    slang_release(o);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    assert(slang_weak_get(weak_box_shared) == NULL);
    slang_release(weak_box_shared);
    assert(slang_live_objects() == live);
}

static void leaky_child(void) {
    slang_leak_check_enable(2);
    slang_runtime_init(0, NULL);
    slang_object_new(&counted, 24);
    slang_object_new(&counted, 24);
    slang_string_literal("immortal", 8); /* does not count */
    slang_runtime_shutdown();            /* reports and exits */
    _exit(0);
}

static void clean_child(void) {
    slang_leak_check_enable(2);
    slang_runtime_init(0, NULL);
    slang_release(slang_object_new(&counted, 24));
    slang_runtime_shutdown();
}

TEST(leak_report) {
    char buf[4096];
    int st = run_child_fd(leaky_child, 2, buf, sizeof buf);
    assert(st == SLANG_LEAK_EXIT_CODE);
    assert(strstr(buf, "leak: 2 objects\n"));
    assert(strstr(buf, "  2 x Counted\n"));
    st = run_child_fd(clean_child, 2, buf, sizeof buf);
    assert(st == 0 && buf[0] == 0);
}

int main(void) {
    printf("test_mem\n");
    RUN(alloc_zeroed);
    RUN(refcount_basic);
    RUN(immortal);
    RUN(atomic_refcount);
    RUN(layout);
    RUN(payload_pointers);
    RUN(weak_refs);
    RUN(weak_threads);
    RUN(leak_report);
    return 0;
}
