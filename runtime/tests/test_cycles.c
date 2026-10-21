/* Cycle collector (cycles.c, ABI.md 3.8): trial deletion on constructed
 * graphs, destructor order, weak references, resurrection, zombies, and a
 * stress test of mutator threads racing a collector thread. */
#include "test.h"

#define MAGIC 0x5eed5eedLL

typedef struct node {
    slang_object hdr;
    slang_object *a, *b;     /* strong references (headers), visited */
    slang_object *hidden;    /* strong, but not visited: an "outside" reference */
    int64_t id, magic;
} node;

static int64_t g_drops, g_fins, g_allocs;
static int64_t g_order[64];
static int g_norder;

static void node_drop(void *obj) {
    node *n = obj;
    assert(n->magic == MAGIC);
    n->magic = 0;
    __atomic_add_fetch(&g_drops, 1, __ATOMIC_RELAXED);
    slang_release(__atomic_exchange_n(&n->a, NULL, __ATOMIC_ACQ_REL));
    slang_release(__atomic_exchange_n(&n->b, NULL, __ATOMIC_ACQ_REL));
    slang_release(__atomic_exchange_n(&n->hidden, NULL, __ATOMIC_ACQ_REL));
}

static void node_visit(void *obj, slang_cycle_cb cb, void *ctx) {
    node *n = obj;
    slang_object *a = __atomic_load_n(&n->a, __ATOMIC_ACQUIRE);
    slang_object *b = __atomic_load_n(&n->b, __ATOMIC_ACQUIRE);
    if (a) cb(ctx, a, NULL);
    if (b) cb(ctx, b, NULL);
}

static const slang_type_info node_ti = { "Node", (int64_t)sizeof(node), node_drop };

/* A type with a destructor: fin runs it, clear releases the fields. */
static void fnode_fin(void *obj) {
    node *n = obj;
    assert(n->magic == MAGIC);
    /* every member of the cycle is still intact while any fin runs */
    if (n->a) assert(((node *)n->a)->magic == MAGIC);
    if (g_norder < 64) g_order[g_norder++] = n->id;
    __atomic_add_fetch(&g_fins, 1, __ATOMIC_RELAXED);
}
static void fnode_clear(void *obj) {
    node *n = obj;
    n->magic = 0;
    slang_release(n->a);
    slang_release(n->b);
    slang_release(n->hidden);
    n->a = n->b = n->hidden = NULL;
}
static void fnode_drop(void *obj) {
    fnode_fin(obj);
    fnode_clear(obj);
}
static const slang_type_info fnode_ti = { "FNode", (int64_t)sizeof(node), fnode_drop };

/* A resurrecting destructor: stores its `a` in a global. */
static slang_object *g_saved;
static void rnode_fin(void *obj) {
    node *n = obj;
    __atomic_add_fetch(&g_fins, 1, __ATOMIC_RELAXED);
    if (n->id == 1 && !g_saved) {
        slang_retain(n->a);
        g_saved = n->a;
    }
}
static void rnode_drop(void *obj) {
    rnode_fin(obj);
    fnode_clear(obj);
}
static const slang_type_info rnode_ti = { "RNode", (int64_t)sizeof(node), rnode_drop };

static const slang_cycle_type types[] = {
    { &node_ti, node_visit, NULL, NULL, NULL, 0 },
    { &fnode_ti, node_visit, fnode_fin, fnode_clear, NULL, 0 },
    { &rnode_ti, node_visit, rnode_fin, fnode_clear, NULL, 0 },
};

static node *mk(const slang_type_info *ti, int64_t id) {
    node *n = slang_cycles_new(ti, (int64_t)sizeof(node));
    n->id = id;
    n->magic = MAGIC;
    __atomic_add_fetch(&g_allocs, 1, __ATOMIC_RELAXED);
    return n;
}

/* x.a = y (retains y). */
static void link_a(node *x, node *y) {
    slang_retain(y);
    slang_release(__atomic_exchange_n(&x->a, &y->hdr, __ATOMIC_ACQ_REL));
}
static void link_b(node *x, node *y) {
    slang_retain(y);
    slang_release(__atomic_exchange_n(&x->b, &y->hdr, __ATOMIC_ACQ_REL));
}

TEST(flags_set) {
    node *n = mk(&node_ti, 0);
    assert(n->hdr.refcount & SLANG_RC_CYCLIC);
    assert(slang_refcount(n) == 1);
    slang_release(n);
    assert(slang_cycles_collect() == 0);
}

TEST(self_loop) {
    int64_t live = slang_live_objects();
    node *n = mk(&node_ti, 1);
    link_a(n, n);
    slang_release(n);                     /* 2 -> 1: a possible root */
    assert(slang_live_objects() == live + 1);
    assert(slang_cycles_collect() == 1);
    assert(slang_live_objects() == live);
}

TEST(pair_and_tail) {
    int64_t live = slang_live_objects();
    node *x = mk(&node_ti, 1), *y = mk(&node_ti, 2), *t = mk(&node_ti, 3);
    link_a(x, y);
    link_a(y, x);
    link_b(x, t);                         /* acyclic tail hanging off the cycle */
    slang_release(t);
    slang_release(y);
    slang_release(x);
    assert(slang_live_objects() == live + 3);
    assert(slang_cycles_collect() == 3);
    assert(slang_live_objects() == live);
}

TEST(external_ref_keeps_alive) {
    int64_t live = slang_live_objects();
    node *x = mk(&node_ti, 1), *y = mk(&node_ti, 2);
    link_a(x, y);
    link_a(y, x);
    slang_release(y);                     /* x still held by us */
    assert(slang_cycles_collect() == 0);
    assert(x->magic == MAGIC && ((node *)x->a)->magic == MAGIC);
    /* an outside reference the collector does not see (not visited) */
    node *h = mk(&node_ti, 3);
    slang_retain(y);
    h->hidden = &y->hdr;
    slang_release(x);
    assert(slang_cycles_collect() == 0);  /* y is referenced from h */
    slang_release(h);                     /* h dies: y loses its outside ref */
    assert(slang_cycles_collect() == 2);
    assert(slang_live_objects() == live);
}

TEST(garbage_references_live) {
    int64_t live = slang_live_objects();
    node *keep = mk(&node_ti, 9);
    node *x = mk(&node_ti, 1), *y = mk(&node_ti, 2);
    link_a(x, y);
    link_a(y, x);
    link_b(y, keep);
    slang_release(x);
    slang_release(y);
    assert(slang_refcount(keep) == 2);
    assert(slang_cycles_collect() == 2);
    assert(slang_refcount(keep) == 1 && keep->magic == MAGIC);
    slang_release(keep);
    assert(slang_live_objects() == live);
}

TEST(big_ring) {
    int64_t live = slang_live_objects();
    enum { N = 20000 };
    node *first = mk(&node_ti, 0), *prev = first;
    slang_retain(first);                  /* ours, until the ring closes */
    for (int i = 1; i < N; i++) {
        node *n = mk(&node_ti, i);
        link_a(prev, n);
        slang_release(prev);
        prev = n;
    }
    link_a(prev, first);
    slang_release(prev);
    slang_release(first);
    int64_t got = slang_cycles_collect();
    assert(got == N);
    assert(slang_live_objects() == live);
}

TEST(two_rings_one_held) {
    int64_t live = slang_live_objects();
    node *a1 = mk(&node_ti, 1), *a2 = mk(&node_ti, 2), *b1 = mk(&node_ti, 3), *b2 = mk(&node_ti, 4);
    link_a(a1, a2);
    link_a(a2, a1);
    link_a(b1, b2);
    link_a(b2, b1);
    link_b(a2, b1);                       /* garbage ring a points at ring b */
    slang_release(a1);
    slang_release(a2);
    slang_release(b2);                    /* b1 still held */
    assert(slang_cycles_collect() == 2);  /* only ring a */
    assert(b1->magic == MAGIC && slang_refcount(b1) == 2);
    slang_release(b1);
    assert(slang_cycles_collect() == 2);
    assert(slang_live_objects() == live);
}

TEST(destructors_first) {
    int64_t live = slang_live_objects();
    g_norder = 0;
    g_fins = 0;
    node *x = mk(&fnode_ti, 1), *y = mk(&fnode_ti, 2), *z = mk(&fnode_ti, 3);
    link_a(x, y);
    link_a(y, z);
    link_a(z, x);
    slang_release(x);
    slang_release(y);
    slang_release(z);
    assert(slang_cycles_collect() == 3);
    assert(g_fins == 3 && g_norder == 3);
    /* recording order: the first possible root first */
    assert(g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3);
    assert(slang_live_objects() == live);
}

TEST(resurrection_keeps_and_never_redrops) {
    g_fins = 0;
    g_saved = NULL;
    node *x = mk(&rnode_ti, 1), *y = mk(&rnode_ti, 2);
    link_a(x, y);
    link_a(y, x);
    slang_release(x);
    slang_release(y);
    assert(slang_cycles_collect() == 0);  /* y was stored by x's destructor */
    assert(g_fins == 2);
    assert(g_saved == &y->hdr);
    assert(slang_cycles_collect() == 0);
    assert(g_fins == 2);                  /* no destructor runs twice */
}

TEST(weak_cleared) {
    int64_t live = slang_live_objects();
    node *x = mk(&node_ti, 1), *y = mk(&node_ti, 2);
    link_a(x, y);
    link_a(y, x);
    void *w = slang_weak_new((char *)x + sizeof(slang_object));
    void *p = slang_weak_get(w);
    assert(p == (char *)x + sizeof(slang_object));
    slang_ptr_release(p);
    slang_release(x);
    slang_release(y);
    /* a strong reference taken from the weak one keeps the cycle */
    p = slang_weak_get(w);
    assert(p);
    assert(slang_cycles_collect() == 0);
    slang_ptr_release(p);
    assert(slang_cycles_collect() == 2);
    assert(slang_weak_get(w) == NULL);
    slang_release(w);
    assert(slang_live_objects() == live);
}

TEST(buffered_death_same_thread) {
    int64_t live = slang_live_objects();
    for (int i = 0; i < 100000; i++) {
        node *n = mk(&node_ti, i);
        slang_retain(n);
        slang_release(n);                 /* recorded */
        slang_release(n);                 /* dies: leaves the buffer at once */
    }
    assert(slang_live_objects() == live);
    slang_cycle_stats st;
    slang_cycles_stats(&st);
    assert(st.candidates >= 100000 || st.mode != SLANG_CYCLES_AUTO);
    assert(slang_cycles_collect() == 0);
}

/* A pin (retain for a call, then slang_ptr_release_noroot with the epoch
 * read at the pin) records no possible root; with a collection in
 * between it does, so a cycle made garbage during the call is found. */
TEST(pin_release_noroot) {
    int64_t live = slang_live_objects();
    slang_cycle_stats a, b;
    node *x = mk(&node_ti, 1), *y = mk(&node_ti, 2);
    link_a(x, y);
    link_a(y, x);
    slang_release(y);                       /* y: recorded root */
    assert(slang_cycles_collect() == 0);    /* x is ours: both live, y unbuffered */
    slang_cycles_stats(&a);
    int64_t e = slang_cycles_epoch;
    slang_retain(x);                        /* pin x */
    slang_ptr_release_noroot((char *)x + sizeof(slang_object), e);
    slang_cycles_stats(&b);
    assert(b.candidates == a.candidates);   /* nothing recorded */
    /* Pin, then a collection, then our last outside reference is dropped
     * through the pin: the release must record. */
    e = slang_cycles_epoch;
    slang_retain(x);
    slang_release(x);                       /* our own reference: recorded (3 -> 2) */
    assert(slang_cycles_collect() == 0);    /* the pin keeps it: root dropped */
    slang_ptr_release_noroot((char *)x + sizeof(slang_object), e);  /* epoch moved: recorded */
    assert(slang_cycles_collect() == 2);
    assert(slang_live_objects() == live);
}

/* ---- threads ---- */

static node *g_mail;
static void *zombie_thread(void *arg) {
    (void)arg;
    node *n = __atomic_exchange_n(&g_mail, NULL, __ATOMIC_ACQ_REL);
    slang_release(n);                     /* dies on another thread: a zombie */
    return NULL;
}

TEST(zombie_other_thread) {
    int64_t live = slang_live_objects();
    node *n = mk(&node_ti, 7);
    slang_retain(n);
    slang_release(n);                     /* recorded by this thread */
    g_mail = n;
    pthread_t t;
    pthread_create(&t, NULL, zombie_thread, NULL);
    pthread_join(t, NULL);
    assert(slang_live_objects() == live); /* dropped (Drop ran), memory pending */
    assert(slang_cycles_collect() == 0);  /* the zombie's memory is freed */
    assert(slang_live_objects() == live);
}

/* Mutators build and rewire private graphs (making and breaking cycles)
 * and hand nodes to each other through a mailbox, while a collector thread
 * collects in a loop. Afterwards every node must have been dropped exactly
 * once and no live node may ever have been freed (magic checks, ASan). */
#define MUT 4
#define ROOTS 32
#define STEPS 40000
static int g_stop;
static node *g_box[MUT];

static uint64_t rnd(uint64_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

/* A node reachable from root r through a few `a`/`b` links, retained. */
static node *walk(node *r, uint64_t *s) {
    slang_retain(r);
    for (int k = (int)(rnd(s) % 4); k > 0; k--) {
        slang_object *nx = __atomic_load_n((rnd(s) & 1) ? &r->a : &r->b, __ATOMIC_ACQUIRE);
        if (!nx) break;
        /* r is held, so its field holds nx alive while we take a reference
         * (only this thread writes the fields of its own graph). */
        slang_retain(nx);
        slang_release(r);
        r = (node *)nx;
        assert(r->magic == MAGIC);
    }
    return r;
}

static void *mutator(void *arg) {
    int me = (int)(intptr_t)arg;
    uint64_t s = 0x9e3779b97f4a7c15ULL * (uint64_t)(me + 1);
    node *roots[ROOTS] = { 0 };
    node *held[8] = { 0 };
    for (int i = 0; i < ROOTS; i++) roots[i] = mk(&node_ti, i);
    for (int step = 0; step < STEPS; step++) {
        int i = (int)(rnd(&s) % ROOTS), j = (int)(rnd(&s) % ROOTS);
        switch (rnd(&s) % 6) {
        case 0: {                          /* a fresh node linked into a cycle */
            node *n = mk(&node_ti, step);
            node *x = walk(roots[i], &s);
            link_a(n, x);
            link_b(x, n);
            slang_release(x);
            slang_release(n);
            break;
        }
        case 1: {                          /* replace a root (drops a graph) */
            node *n = mk(&node_ti, step);
            if (rnd(&s) & 1) link_a(n, roots[j]);
            slang_release(roots[i]);
            roots[i] = n;
            break;
        }
        case 2: {                          /* rewire inside the graph */
            node *x = walk(roots[i], &s), *y = walk(roots[j], &s);
            if (rnd(&s) & 1) link_a(x, y); else link_b(x, y);
            slang_release(x);
            slang_release(y);
            break;
        }
        case 3: {                          /* cut a link */
            node *x = walk(roots[i], &s);
            slang_release(__atomic_exchange_n((rnd(&s) & 1) ? &x->a : &x->b, NULL, __ATOMIC_ACQ_REL));
            slang_release(x);
            break;
        }
        case 4: {                          /* hand a node to another thread */
            node *x = walk(roots[i], &s);
            node *old = __atomic_exchange_n(&g_box[(me + 1) % MUT], x, __ATOMIC_ACQ_REL);
            slang_release(old);
            break;
        }
        default: {                         /* adopt what another thread sent */
            node *x = __atomic_exchange_n(&g_box[me], NULL, __ATOMIC_ACQ_REL);
            if (x) {
                assert(x->magic == MAGIC);
                /* Hold it for a while from outside any graph (the sender
                 * may still rewire x, so this thread never walks into it),
                 * then drop it here: the last reference may die on this
                 * thread while x is buffered on another. */
                int k = (int)(rnd(&s) % 8);
                slang_release(held[k]);
                held[k] = x;
            }
            break;
        }
        }
    }
    for (int i = 0; i < ROOTS; i++) slang_release(roots[i]);
    for (int i = 0; i < 8; i++) slang_release(held[i]);
    return NULL;
}

static void *collector(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) slang_cycles_collect();
    return NULL;
}

TEST(stress_concurrent) {
    int64_t live = slang_live_objects();
    int64_t a0 = g_allocs, d0 = g_drops;
    pthread_t c, m[MUT];
    pthread_create(&c, NULL, collector, NULL);
    for (int i = 0; i < MUT; i++) pthread_create(&m[i], NULL, mutator, (void *)(intptr_t)i);
    for (int i = 0; i < MUT; i++) pthread_join(m[i], NULL);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    pthread_join(c, NULL);
    for (int i = 0; i < MUT; i++) slang_release(__atomic_exchange_n(&g_box[i], NULL, __ATOMIC_ACQ_REL));
    /* What is left is cyclic garbage: a couple of rounds free it all. */
    for (int k = 0; k < 4 && slang_live_objects() > live; k++) slang_cycles_collect();
    slang_cycle_stats st;
    slang_cycles_stats(&st);
    printf("       %lld nodes, %lld collected, %lld collections, %lld examined\n",
           (long long)(g_allocs - a0), (long long)st.collected, (long long)st.runs, (long long)st.examined);
    if (slang_live_objects() != live)
        printf("       left: %lld live objects\n", (long long)(slang_live_objects() - live));
    assert(slang_live_objects() == live);
    assert(g_drops - d0 == g_allocs - a0);
}

int main(void) {
    setenv("SLANG_CYCLES", "manual", 1);
    slang_cycles_register(types, (int64_t)(sizeof types / sizeof types[0]));
    assert(slang_cycles_mode() == SLANG_CYCLES_MANUAL);
    printf("test_cycles\n");
    RUN(flags_set);
    RUN(self_loop);
    RUN(pair_and_tail);
    RUN(external_ref_keeps_alive);
    RUN(garbage_references_live);
    RUN(big_ring);
    RUN(two_rings_one_held);
    RUN(destructors_first);
    RUN(resurrection_keeps_and_never_redrops);
    RUN(weak_cleared);
    RUN(buffered_death_same_thread);
    RUN(pin_release_noroot);
    RUN(zombie_other_thread);
    RUN(stress_concurrent);
    slang_cycles_set_mode(SLANG_CYCLES_AUTO);
    return 0;
}
