/* std_sync.c - lock-free primitives for sync (stdlib_src/sync.zl):
 * sequentially consistent atomic operations on an int64 field (passed as
 * &field, like task.rtLock) and a thread yield for spin waits. */
#include "rt_internal.h"

#include "platform/platform.h"

int64_t zell_sync_rt_load(int64_t *p);
void zell_sync_rt_store(int64_t *p, int64_t v);
int64_t zell_sync_rt_add(int64_t *p, int64_t delta);
int64_t zell_sync_rt_swap(int64_t *p, int64_t v);
bool zell_sync_rt_cas(int64_t *p, int64_t old, int64_t new_);
void zell_sync_rt_yield(void);

int64_t zell_sync_rt_load(int64_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
void zell_sync_rt_store(int64_t *p, int64_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
int64_t zell_sync_rt_add(int64_t *p, int64_t delta) { return __atomic_add_fetch(p, delta, __ATOMIC_SEQ_CST); }
int64_t zell_sync_rt_swap(int64_t *p, int64_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
bool zell_sync_rt_cas(int64_t *p, int64_t old, int64_t new_) {
    return __atomic_compare_exchange_n(p, &old, new_, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
void zell_sync_rt_yield(void) { zell__plat_yield(); }
