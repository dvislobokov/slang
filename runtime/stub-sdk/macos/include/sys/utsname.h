/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_SYS_UTSNAME_H
#define ZELL_STUB_SYS_UTSNAME_H
#define _SYS_NAMELEN 256
struct utsname { char sysname[_SYS_NAMELEN]; char nodename[_SYS_NAMELEN]; char release[_SYS_NAMELEN]; char version[_SYS_NAMELEN]; char machine[_SYS_NAMELEN]; };
int uname(struct utsname *);
#endif
