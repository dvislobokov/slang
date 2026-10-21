/* leak.c - naming the fields that close reference cycles in the leak report
 * (ABI.md 3.5, leak check mode 2). The compiler registers the `*T` fields
 * of struct types with slang_leak_fields; at shutdown every leaked object's
 * registered fields that point at another leaked object are edges, and an
 * edge that lies on a cycle is reported as a candidate for `weak`. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const slang_type_info *type; const slang_leak_field *fields; const char *acyclic; } reg;

static reg *g_regs;
static int64_t g_nregs, g_capregs;
static slang__plat_mutex g_mu = SLANG_PLAT_MUTEX_INIT;

void slang_leak_fields(const slang_type_info *type, const slang_leak_field *fields) {
    if (!type || !fields) return;
    slang__plat_mutex_lock(&g_mu);
    if (g_nregs == g_capregs) {
        int64_t cap = g_capregs ? g_capregs * 2 : 32;
        reg *n = realloc(g_regs, (size_t)cap * sizeof *n);
        if (!n) { slang__plat_mutex_unlock(&g_mu); return; }
        g_regs = n;
        g_capregs = cap;
    }
    g_regs[g_nregs++] = (reg){ type, fields, NULL };
    slang__plat_mutex_unlock(&g_mu);
}

/* `@acyclic` types (DESIGN.md "Циклы"): a leaked object of one that lies on
 * a cycle breaks the promise, and the report says so. */
void slang_leak_acyclic(const slang_type_info *type, const char *name) {
    if (!type || !name) return;
    slang__plat_mutex_lock(&g_mu);
    for (int64_t i = 0; i < g_nregs; i++)
        if (g_regs[i].type == type) g_regs[i].acyclic = name;
    slang__plat_mutex_unlock(&g_mu);
}

static const char *acyclic_of(const slang_type_info *t) {
    for (int64_t i = 0; i < g_nregs; i++)
        if (g_regs[i].type == t) return g_regs[i].acyclic;
    return NULL;
}

static const slang_leak_field *fields_of(const slang_type_info *t) {
    for (int64_t i = 0; i < g_nregs; i++)
        if (g_regs[i].type == t) return g_regs[i].fields;
    return NULL;
}

#define MAX_OBJECTS 100000

typedef struct { slang_object **objs; int64_t n, cap; } list;

static void collect(slang_object *o, void *arg) {
    list *l = arg;
    if (l->n == l->cap) {
        int64_t cap = l->cap ? l->cap * 2 : 256;
        if (cap > MAX_OBJECTS) return;
        slang_object **n = realloc(l->objs, (size_t)cap * sizeof *n);
        if (!n) return;
        l->objs = n;
        l->cap = cap;
    }
    l->objs[l->n++] = o;
}

static int cmp_ptr(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)*(slang_object *const *)a, y = (uintptr_t)*(slang_object *const *)b;
    return x < y ? -1 : x > y;
}

/* Index of the live object whose payload is p, or -1. */
static int64_t find(const list *l, const void *p) {
    if (!p) return -1;
    slang_object *key = (slang_object *)((const char *)p - sizeof(slang_object));
    slang_object **r = bsearch(&key, l->objs, (size_t)l->n, sizeof *l->objs, cmp_ptr);
    return r ? r - l->objs : -1;
}

/* Target of field f of object i (index), or -1. */
static int64_t target(const list *l, int64_t i, const slang_leak_field *f) {
    void *p;
    memcpy(&p, (char *)l->objs[i] + sizeof(slang_object) + f->offset, sizeof p);
    return find(l, p);
}

/* True if `to` is reachable from `from` through registered fields. */
static int reaches(const list *l, int64_t from, int64_t to, unsigned char *seen, int64_t *stack) {
    memset(seen, 0, (size_t)l->n);
    int64_t sp = 0;
    stack[sp++] = from;
    seen[from] = 1;
    while (sp) {
        int64_t i = stack[--sp];
        if (i == to) return 1;
        const slang_leak_field *fs = fields_of(l->objs[i]->type);
        for (; fs && fs->offset >= 0; fs++) {
            int64_t j = target(l, i, fs);
            if (j >= 0 && !seen[j]) {
                seen[j] = 1;
                stack[sp++] = j;
            }
        }
    }
    return 0;
}

void slang__leak_cycles(void) {
    if (!g_nregs) return;
    list l = { 0 };
    slang__live_each(collect, &l);
    if (l.n == 0) { free(l.objs); return; }
    qsort(l.objs, (size_t)l.n, sizeof *l.objs, cmp_ptr);
    unsigned char *seen = malloc((size_t)l.n);
    int64_t *stack = malloc((size_t)l.n * sizeof *stack);
    const char *names[64], *broken[64];
    int nnames = 0, nbroken = 0;
    for (int64_t i = 0; seen && stack && i < l.n && nnames < 64; i++) {
        const slang_leak_field *fs = fields_of(l.objs[i]->type);
        for (; fs && fs->offset >= 0 && nnames < 64; fs++) {
            int dup = 0;
            for (int k = 0; k < nnames; k++) dup |= strcmp(names[k], fs->name) == 0;
            if (dup) continue;
            int64_t j = target(&l, i, fs);
            if (j >= 0 && reaches(&l, j, i, seen, stack)) names[nnames++] = fs->name;
        }
    }
    /* Leaked objects of @acyclic types on a cycle (any registered field). */
    for (int64_t i = 0; seen && stack && i < l.n && nbroken < 64; i++) {
        const char *a = acyclic_of(l.objs[i]->type);
        if (!a) continue;
        int dup = 0;
        for (int k = 0; k < nbroken; k++) dup |= strcmp(broken[k], a) == 0;
        if (dup) continue;
        const slang_leak_field *fs = fields_of(l.objs[i]->type);
        for (; fs && fs->offset >= 0; fs++) {
            int64_t j = target(&l, i, fs);
            if (j >= 0 && reaches(&l, j, i, seen, stack)) {
                broken[nbroken++] = a;
                break;
            }
        }
    }
    for (int k = 0; k < nbroken; k++)
        fprintf(stderr, "  type %s is @acyclic but was found in a cycle\n", broken[k]);
    for (int k = 0; k < nnames; k++)
        fprintf(stderr, "  cycle through %s (make one field of the cycle `weak *T` to break it)\n", names[k]);
    free(seen);
    free(stack);
    free(l.objs);
}
