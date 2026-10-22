/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef ZELL_STUB_NETINET_IN_H
#define ZELL_STUB_NETINET_IN_H
#include <sys/socket.h>
#include <stdint.h>
typedef __uint32_t in_addr_t;
typedef __uint16_t in_port_t;
struct in_addr { in_addr_t s_addr; };
struct in6_addr { union { __uint8_t __u6_addr8[16]; __uint16_t __u6_addr16[8]; __uint32_t __u6_addr32[4]; } __u6_addr; };
#define s6_addr __u6_addr.__u6_addr8
struct sockaddr_in { __uint8_t sin_len; sa_family_t sin_family; in_port_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
struct sockaddr_in6 { __uint8_t sin6_len; sa_family_t sin6_family; in_port_t sin6_port; __uint32_t sin6_flowinfo; struct in6_addr sin6_addr; __uint32_t sin6_scope_id; };
struct ip_mreq { struct in_addr imr_multiaddr; struct in_addr imr_interface; };
struct ip_mreqn { struct in_addr imr_multiaddr; struct in_addr imr_address; int imr_ifindex; };
struct ipv6_mreq { struct in6_addr ipv6mr_multiaddr; unsigned int ipv6mr_interface; };
extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;
#define IN6ADDR_ANY_INIT {{{0}}}
#define INADDR_ANY ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE ((in_addr_t)0xffffffff)
#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#define IPPROTO_ICMPV6 58
#define IPPROTO_RAW 255
#define IP_TOS 3
#define IP_TTL 4
#define IP_MULTICAST_IF 9
#define IP_MULTICAST_TTL 10
#define IP_MULTICAST_LOOP 11
#define IP_ADD_MEMBERSHIP 12
#define IP_DROP_MEMBERSHIP 13
#define IP_BOUND_IF 25
#define IPV6_UNICAST_HOPS 4
#define IPV6_MULTICAST_IF 9
#define IPV6_MULTICAST_HOPS 10
#define IPV6_MULTICAST_LOOP 11
#define IPV6_JOIN_GROUP 12
#define IPV6_LEAVE_GROUP 13
#define IPV6_V6ONLY 27
#define IPV6_BOUND_IF 125
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46
#define IN6_IS_ADDR_LINKLOCAL(a) (((a)->s6_addr[0] == 0xfe) && (((a)->s6_addr[1] & 0xc0) == 0x80))
#define IN6_IS_ADDR_MC_LINKLOCAL(a) (((a)->s6_addr[0] == 0xff) && (((a)->s6_addr[1] & 0x0f) == 0x02))
#define IN6_IS_ADDR_V4MAPPED(a) ((a)->__u6_addr.__u6_addr32[0] == 0 && (a)->__u6_addr.__u6_addr32[1] == 0 && (a)->__u6_addr.__u6_addr32[2] == 0xffff0000u)
#define ntohs(x) __builtin_bswap16(x)
#define htons(x) __builtin_bswap16(x)
#define ntohl(x) __builtin_bswap32(x)
#define htonl(x) __builtin_bswap32(x)
#endif
