/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_SYS_UN_H
#define ZELL_STUB_SYS_UN_H
#include <sys/socket.h>
struct sockaddr_un { unsigned char sun_len; sa_family_t sun_family; char sun_path[104]; };
#endif
