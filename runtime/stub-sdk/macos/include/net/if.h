/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_NET_IF_H
#define SLANG_STUB_NET_IF_H
#include <sys/socket.h>
#define IF_NAMESIZE 16
#define IFNAMSIZ IF_NAMESIZE
#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_DEBUG 0x4
#define IFF_LOOPBACK 0x8
#define IFF_POINTOPOINT 0x10
#define IFF_RUNNING 0x40
#define IFF_NOARP 0x80
#define IFF_PROMISC 0x100
#define IFF_MULTICAST 0x8000
struct if_nameindex { unsigned int if_index; char *if_name; };
struct if_data { unsigned char ifi_type, ifi_typelen, ifi_physical, ifi_addrlen, ifi_hdrlen, ifi_recvquota, ifi_xmitquota, ifi_unused1; __uint32_t ifi_mtu; __uint32_t ifi_metric; __uint32_t ifi_baudrate; __uint32_t ifi_ipackets, ifi_ierrors, ifi_opackets, ifi_oerrors, ifi_collisions, ifi_ibytes, ifi_obytes, ifi_imcasts, ifi_omcasts, ifi_iqdrops, ifi_noproto, ifi_recvtiming, ifi_xmittiming; char __pad[32]; };
struct ifreq { char ifr_name[IFNAMSIZ]; union { struct sockaddr ifru_addr; short ifru_flags; int ifru_mtu; int ifru_metric; void *ifru_data; } ifr_ifru; };
#define ifr_mtu ifr_ifru.ifru_mtu
#define ifr_flags ifr_ifru.ifru_flags
unsigned int if_nametoindex(const char *);
char *if_indextoname(unsigned int, char *);
struct if_nameindex *if_nameindex(void);
void if_freenameindex(struct if_nameindex *);
#endif
