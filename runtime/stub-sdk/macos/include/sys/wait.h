/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_SYS_WAIT_H
#define ZELL_STUB_SYS_WAIT_H
#include <sys/types.h>
#include <signal.h>
#define WNOHANG 0x00000001
#define WUNTRACED 0x00000002
#define _WSTATUS(x) ((x) & 0177)
#define WIFEXITED(x) (_WSTATUS(x) == 0)
#define WEXITSTATUS(x) (((x) >> 8) & 0x000000ff)
#define WIFSIGNALED(x) (_WSTATUS(x) != 0177 && _WSTATUS(x) != 0)
#define WTERMSIG(x) (_WSTATUS(x))
#define WIFSTOPPED(x) (_WSTATUS(x) == 0177)
#define WSTOPSIG(x) ((x) >> 8)
pid_t wait(int *);
pid_t waitpid(pid_t, int *, int);
#endif
