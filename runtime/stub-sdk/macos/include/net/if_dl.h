/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_NET_IF_DL_H
#define ZELL_STUB_NET_IF_DL_H
#include <sys/types.h>
struct sockaddr_dl { u_char sdl_len; u_char sdl_family; u_short sdl_index; u_char sdl_type; u_char sdl_nlen; u_char sdl_alen; u_char sdl_slen; char sdl_data[12]; };
#define LLADDR(s) ((caddr_t)((s)->sdl_data + (s)->sdl_nlen))
#endif
