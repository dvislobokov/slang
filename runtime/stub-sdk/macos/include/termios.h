/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_TERMIOS_H
#define SLANG_STUB_TERMIOS_H
#include <sys/types.h>
typedef unsigned long tcflag_t;
typedef unsigned char cc_t;
typedef unsigned long speed_t;
#define NCCS 20
struct termios { tcflag_t c_iflag, c_oflag, c_cflag, c_lflag; cc_t c_cc[NCCS]; speed_t c_ispeed, c_ospeed; };
#define ECHO 0x00000008
#define ICANON 0x00000100
#define ISIG 0x00000080
#define IEXTEN 0x00000400
#define ICRNL 0x00000100
#define IXON 0x00000200
#define OPOST 0x00000001
#define VMIN 16
#define VTIME 17
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
int tcgetattr(int, struct termios *);
int tcsetattr(int, int, const struct termios *);
#endif
