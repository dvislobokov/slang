/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_SYS_IOCTL_H
#define SLANG_STUB_SYS_IOCTL_H
#include <sys/types.h>
#define IOCPARM_MASK 0x1fff
#define IOC_OUT 0x40000000
#define IOC_IN 0x80000000
#define IOC_INOUT (IOC_IN | IOC_OUT)
#define _IOC(inout, group, num, len) (inout | ((len & IOCPARM_MASK) << 16) | ((group) << 8) | (num))
#define _IOR(g, n, t) _IOC(IOC_OUT, (g), (n), sizeof(t))
#define _IOW(g, n, t) _IOC(IOC_IN, (g), (n), sizeof(t))
#define _IOWR(g, n, t) _IOC(IOC_INOUT, (g), (n), sizeof(t))
#define FIONREAD _IOR('f', 127, int)
#define FIONBIO _IOW('f', 126, int)
#define FIOCLEX 0x20006601
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#define TIOCGWINSZ _IOR('t', 104, struct winsize)
#define SIOCGIFMTU 0xc0206933
#define SIOCGIFFLAGS 0xc0206911
int ioctl(int, unsigned long, ...);
#endif
