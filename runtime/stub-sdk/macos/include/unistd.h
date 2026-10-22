/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_UNISTD_H
#define ZELL_STUB_UNISTD_H
#include <stddef.h>
#include <sys/types.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define _SC_PAGESIZE 29
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_CONF 57
#define _SC_NPROCESSORS_ONLN 58
#define _SC_OPEN_MAX 5
#define _SC_CLK_TCK 3
#define _SC_GETPW_R_SIZE_MAX 71
#define _SC_GETGR_R_SIZE_MAX 70
#define _SC_HOST_NAME_MAX 72
#define _PC_PATH_MAX 5
extern char **environ;
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
ssize_t pread(int, void *, size_t, off_t);
ssize_t pwrite(int, const void *, size_t, off_t);
off_t lseek(int, off_t, int);
int close(int);
int dup(int);
int dup2(int, int);
int pipe(int[2]);
int isatty(int);
int access(const char *, int);
int faccessat(int, const char *, int, int);
int unlink(const char *);
int unlinkat(int, const char *, int);
int rmdir(const char *);
int chdir(const char *);
int fchdir(int);
char *getcwd(char *, size_t);
int link(const char *, const char *);
int symlink(const char *, const char *);
ssize_t readlink(const char *restrict, char *restrict, size_t);
int chown(const char *, uid_t, gid_t);
int fchown(int, uid_t, gid_t);
int lchown(const char *, uid_t, gid_t);
int truncate(const char *, off_t);
int ftruncate(int, off_t);
int fsync(int);
int fdatasync(int);
long sysconf(int);
long pathconf(const char *, int);
long fpathconf(int, int);
pid_t getpid(void);
pid_t getppid(void);
pid_t getpgrp(void);
pid_t setsid(void);
int setpgid(pid_t, pid_t);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int getgroups(int, gid_t[]);
int getgrouplist(const char *, int, int *, int *);
int setuid(uid_t);
int setgid(gid_t);
int gethostname(char *, size_t);
char *getlogin(void);
int getlogin_r(char *, size_t);
pid_t fork(void);
int execv(const char *, char *const[]);
int execve(const char *, char *const[], char *const[]);
int execvp(const char *, char *const[]);
void _exit(int) __attribute__((noreturn));
unsigned sleep(unsigned);
int usleep(useconds_t);
unsigned alarm(unsigned);
int pause(void);
int getentropy(void *, size_t);
char *ttyname(int);
int ttyname_r(int, char *, size_t);
int lockf(int, int, off_t);
int nice(int);
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int, char *const[], const char *);
#endif
