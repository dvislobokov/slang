/* std_unsafe.c - std.unsafe: raw memory access and calls by address
 * (runtime/ABI.md section 22.5). Only packages whose manifest grants the
 * `unsafe` capability (and the standard library itself, for std.debug)
 * may import std.unsafe; nothing here checks addresses. */
#include "rt_internal.h"

#include <string.h>

int64_t slang_unsafe_read_int(int64_t addr, int64_t size) {
    const void *p = (const void *)(intptr_t)addr;
    switch (size) {
    case 1: { int8_t v; memcpy(&v, p, 1); return v; }
    case 2: { int16_t v; memcpy(&v, p, 2); return v; }
    case 4: { int32_t v; memcpy(&v, p, 4); return v; }
    case 8: { int64_t v; memcpy(&v, p, 8); return v; }
    default: slang__panicf("unsafe.ReadInt: bad size %lld", (long long)size);
    }
}

int64_t slang_unsafe_read_uint(int64_t addr, int64_t size) {
    const void *p = (const void *)(intptr_t)addr;
    switch (size) {
    case 1: { uint8_t v; memcpy(&v, p, 1); return v; }
    case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
    case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
    case 8: { int64_t v; memcpy(&v, p, 8); return v; }
    default: slang__panicf("unsafe.ReadUint: bad size %lld", (long long)size);
    }
}

double slang_unsafe_read_float(int64_t addr, int64_t size) {
    const void *p = (const void *)(intptr_t)addr;
    if (size == 4) { float f; memcpy(&f, p, 4); return f; }
    double d;
    memcpy(&d, p, 8);
    return d;
}

void slang_unsafe_write_int(int64_t addr, int64_t size, int64_t v) {
    void *p = (void *)(intptr_t)addr;
    switch (size) {
    case 1: { int8_t x = (int8_t)v; memcpy(p, &x, 1); break; }
    case 2: { int16_t x = (int16_t)v; memcpy(p, &x, 2); break; }
    case 4: { int32_t x = (int32_t)v; memcpy(p, &x, 4); break; }
    case 8: memcpy(p, &v, 8); break;
    default: slang__panicf("unsafe.WriteInt: bad size %lld", (long long)size);
    }
}

void slang_unsafe_read_bytes(int64_t addr, int64_t n, slang_slice *out) {
    if (n < 0) n = 0;
    slang_slice_new(out, 1, n, n);
    if (n) memcpy(out->data, (const void *)(intptr_t)addr, (size_t)n);
}

void slang_unsafe_write_bytes(int64_t addr, const slang_slice *b) {
    if (b->len) memcpy((void *)(intptr_t)addr, b->data, (size_t)b->len);
}

/* The text of the NUL-terminated C string at addr ("" for 0). */
slang_string *slang_unsafe_c_string(int64_t addr) {
    return slang_string_from_cstr(addr ? (const char *)(intptr_t)addr : "");
}

/* The data word of an `any` (the address of its boxed value). */
int64_t slang_unsafe_addr_of(void *data, void *type) {
    (void)type;
    return (int64_t)(intptr_t)data;
}

/* The type word of an `any` (its run-time type info). */
int64_t slang_unsafe_type_of(void *data, void *type) {
    (void)data;
    return (int64_t)(intptr_t)type;
}

typedef int64_t (*f0)(void);
typedef int64_t (*f1)(int64_t);
typedef int64_t (*f2)(int64_t, int64_t);
typedef int64_t (*f3)(int64_t, int64_t, int64_t);
typedef int64_t (*f4)(int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*f5)(int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*f6)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*f7)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*f8)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

/* Calls the C-ABI function at addr with up to 8 integer/pointer arguments;
 * returns its integer result (a `bool` result is in the low byte). */
int64_t slang_unsafe_call_func(int64_t addr, const slang_slice *args) {
    if (!addr) slang_panic_nil();
    const int64_t *a = args->data;
    void *fp = (void *)(intptr_t)addr;
    switch (args->len) {
    case 0: return ((f0)fp)();
    case 1: return ((f1)fp)(a[0]);
    case 2: return ((f2)fp)(a[0], a[1]);
    case 3: return ((f3)fp)(a[0], a[1], a[2]);
    case 4: return ((f4)fp)(a[0], a[1], a[2], a[3]);
    case 5: return ((f5)fp)(a[0], a[1], a[2], a[3], a[4]);
    case 6: return ((f6)fp)(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 7: return ((f7)fp)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    case 8: return ((f8)fp)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    default: slang__panicf("unsafe.CallFunc: at most 8 arguments (got %lld)", (long long)args->len);
    }
}
