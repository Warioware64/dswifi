// SPDX-License-Identifier: MIT
//
// Copyright (C) 2026 The Half-Life DS port's authors

// The options of the stack built apart (LWIP_MODE=dsl in Makefile.arm9:
// libdswifi_dl9_lwip.a, for a library loaded at run time): the library's own,
// less what a game talking UDP to one server needs not -- no TCP, no IPv6,
// no raw sockets, no multicast -- and its pools in its own memory, sized
// here, rather than taken from the heap a packet at a time.

#ifndef DSWIFI_LWIPOPTS_DSL_H__
#define DSWIFI_LWIPOPTS_DSL_H__

#include "../include/lwipopts.h"

// Pools of fixed size, in the library's memory. (Allocations lwIP makes with
// mem_malloc, the packets it sends, still come from the heap and go back.)
#undef MEMP_MEM_MALLOC
#define MEMP_MEM_MALLOC             0

#undef MEMP_NUM_PBUF
#define MEMP_NUM_PBUF               16
#undef MEMP_NUM_UDP_PCB
#define MEMP_NUM_UDP_PCB            4
#undef MEMP_NUM_REASSDATA
#define MEMP_NUM_REASSDATA          2
#undef MEMP_NUM_ARP_QUEUE
#define MEMP_NUM_ARP_QUEUE          4
#undef MEMP_NUM_NETCONN
#define MEMP_NUM_NETCONN            4

// What comes in: a pool of pieces of 512 bytes, a datagram over several
// pieces chained. 32 hold a full queue of small datagrams (a game's frames)
// or a dozen of the largest a relay sends.
#undef PBUF_POOL_SIZE
#define PBUF_POOL_SIZE              32
#define PBUF_POOL_BUFSIZE           512

#undef IP_REASS_MAX_PBUFS
#define IP_REASS_MAX_PBUFS          8

#undef LWIP_IPV6
#define LWIP_IPV6                   0
#undef LWIP_IPV6_AUTOCONFIG
#define LWIP_IPV6_AUTOCONFIG        0
#undef LWIP_ICMP6
#define LWIP_ICMP6                  0
#undef LWIP_IPV6_DHCP6
#define LWIP_IPV6_DHCP6             0
#undef LWIP_IPV6_DHCP6_STATELESS
#define LWIP_IPV6_DHCP6_STATELESS   0

#undef LWIP_IGMP
#define LWIP_IGMP                   0
#undef LWIP_ACD
#define LWIP_ACD                    0
#undef LWIP_MULTICAST_PING
#define LWIP_MULTICAST_PING         0

#undef LWIP_TCP
#define LWIP_TCP                    0
#undef LWIP_TCP_KEEPALIVE
#define LWIP_TCP_KEEPALIVE          0
#undef LWIP_RAW
#define LWIP_RAW                    0

#undef LWIP_DNS_ADDRTYPE_DEFAULT
#define LWIP_DNS_ADDRTYPE_DEFAULT   LWIP_DNS_ADDRTYPE_IPV4

// errno is a thread's own (picolibc's, in each thread's storage), which a
// library loaded at run time cannot reach: the program's core sets it
// (lwip_hook.c), for the sockets as for the glue (lwip_nds.h's DSWIFI_ERRNO).
void dswifi_set_errno(int err);
#define set_errno(err) do { if (err) { dswifi_set_errno(err); } } while (0)

// No diagnostics and no assertions: nothing to print them to, and the
// program they are linked against need not carry printf for them.
#define LWIP_NOASSERT
#define LWIP_PLATFORM_DIAG(x)       do { } while (0)
#define LWIP_PLATFORM_ASSERT(x)     do { } while (0)

#endif // DSWIFI_LWIPOPTS_DSL_H__
