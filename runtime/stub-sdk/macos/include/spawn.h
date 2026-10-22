/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_SPAWN_H
#define ZELL_STUB_SPAWN_H
#include <sys/types.h>
#include <signal.h>
typedef void *posix_spawnattr_t;
typedef void *posix_spawn_file_actions_t;
#define POSIX_SPAWN_RESETIDS 0x0001
#define POSIX_SPAWN_SETPGROUP 0x0002
#define POSIX_SPAWN_SETSIGDEF 0x0004
#define POSIX_SPAWN_SETSIGMASK 0x0008
#define POSIX_SPAWN_SETEXEC 0x0040
#define POSIX_SPAWN_START_SUSPENDED 0x0080
#define POSIX_SPAWN_CLOEXEC_DEFAULT 0x4000
int posix_spawn(pid_t *restrict, const char *restrict, const posix_spawn_file_actions_t *, const posix_spawnattr_t *restrict, char *const[restrict], char *const[restrict]);
int posix_spawnp(pid_t *restrict, const char *restrict, const posix_spawn_file_actions_t *, const posix_spawnattr_t *restrict, char *const[restrict], char *const[restrict]);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *, int, int);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *restrict, int, const char *restrict, int, mode_t);
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *, const char *);
int posix_spawn_file_actions_addinherit_np(posix_spawn_file_actions_t *, int);
int posix_spawnattr_init(posix_spawnattr_t *);
int posix_spawnattr_destroy(posix_spawnattr_t *);
int posix_spawnattr_setflags(posix_spawnattr_t *, short);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *restrict, const sigset_t *restrict);
int posix_spawnattr_setsigmask(posix_spawnattr_t *restrict, const sigset_t *restrict);
int posix_spawnattr_setpgroup(posix_spawnattr_t *, pid_t);
#endif
