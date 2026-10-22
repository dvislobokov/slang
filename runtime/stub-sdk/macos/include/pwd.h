/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_PWD_H
#define ZELL_STUB_PWD_H
#include <sys/types.h>
struct passwd { char *pw_name; char *pw_passwd; uid_t pw_uid; gid_t pw_gid; time_t pw_change; char *pw_class; char *pw_gecos; char *pw_dir; char *pw_shell; time_t pw_expire; };
struct passwd *getpwnam(const char *);
struct passwd *getpwuid(uid_t);
int getpwnam_r(const char *, struct passwd *, char *, size_t, struct passwd **);
int getpwuid_r(uid_t, struct passwd *, char *, size_t, struct passwd **);
void setpwent(void);
struct passwd *getpwent(void);
void endpwent(void);
#endif
