/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_SYS__PTHREAD_TYPES_H
#define SLANG_STUB_SYS__PTHREAD_TYPES_H
struct __darwin_pthread_handler_rec { void (*__routine)(void *); void *__arg; struct __darwin_pthread_handler_rec *__next; };
struct _opaque_pthread_attr_t { long __sig; char __opaque[56]; };
struct _opaque_pthread_cond_t { long __sig; char __opaque[40]; };
struct _opaque_pthread_condattr_t { long __sig; char __opaque[8]; };
struct _opaque_pthread_mutex_t { long __sig; char __opaque[56]; };
struct _opaque_pthread_mutexattr_t { long __sig; char __opaque[8]; };
struct _opaque_pthread_once_t { long __sig; char __opaque[8]; };
struct _opaque_pthread_rwlock_t { long __sig; char __opaque[192]; };
struct _opaque_pthread_t { long __sig; struct __darwin_pthread_handler_rec *__cleanup_stack; char __opaque[8176]; };
typedef struct _opaque_pthread_attr_t pthread_attr_t;
typedef struct _opaque_pthread_cond_t pthread_cond_t;
typedef struct _opaque_pthread_condattr_t pthread_condattr_t;
typedef struct _opaque_pthread_mutex_t pthread_mutex_t;
typedef struct _opaque_pthread_mutexattr_t pthread_mutexattr_t;
typedef struct _opaque_pthread_once_t pthread_once_t;
typedef struct _opaque_pthread_rwlock_t pthread_rwlock_t;
typedef struct _opaque_pthread_t *pthread_t;
typedef unsigned long pthread_key_t;
#endif
