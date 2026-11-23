// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025-2026 Antonio Niño Díaz

#ifndef LWIP_NDS_H__
#define LWIP_NDS_H__

#ifdef DSWIFI_ENABLE_LWIP

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint8_t dest_mac[6];
    uint8_t src_mac[6];
    uint16_t ether_type;
}
EthernetFrameHeader;

int Wifi_TransmitFunctionLink(const void *src, size_t size);
// Not const, we need to overwrite the data for speed
void Wifi_SendPacketToLwip(u8 *packet, size_t size);

extern bool wifi_lwip_enabled;

void wifi_lwip_update(void);

int wifi_lwip_init(void);
void wifi_lwip_deinit(void);

// This must be called when the console connects to an AP.
void wifi_netif_set_up(void);
// This must be called when the console disconnects from an AP.
void wifi_netif_set_down(void);
// Returns true if the netif is up.
bool wifi_netif_is_up(void);

// Returns true if DHCP is enabled, false if using manual IP settings.
bool wifi_using_dhcp(void);

u32 wifi_get_ip(void);
u32 wifi_get_dns(int index);

bool wifi_get_ip6(struct in6_addr *addr);

void dswifi_send_data_to_lwip(void *data, u32 len);

void dswifi_lwip_setup_io_posix(void);

// errno set: by the core when this is the stack built apart (it cannot reach
// a thread's own storage: include_dsl/lwipopts.h).
#ifdef DSWIFI_LWIP_DSL
void dswifi_set_errno(int err);
#define DSWIFI_ERRNO(e) dswifi_set_errno(e)
#else
#define DSWIFI_ERRNO(e) (errno = (e))
#endif

#if defined(DSWIFI_LWIP_HOOKED) && !defined(DSWIFI_LWIP_DSL)

// The core built without the IP stack (libdswifi_dl9_hook.a): each call into
// it goes through the table the stack installs (dswifi_lwip.h, lwip_hook.c);
// with none installed, it is as if the library had no IP stack.
#include <dswifi_lwip.h>

extern const Wifi_LwipOps *wifi_lwip_ops;

#define wifi_lwip_init()           (wifi_lwip_ops ? wifi_lwip_ops->init() : -1)
#define wifi_lwip_deinit()         do { if (wifi_lwip_ops) wifi_lwip_ops->deinit(); } while (0)
#define wifi_lwip_update()         do { if (wifi_lwip_ops) wifi_lwip_ops->update(); } while (0)
#define wifi_netif_is_up()         (wifi_lwip_ops ? wifi_lwip_ops->netif_is_up() : false)
#define Wifi_SendPacketToLwip(p, n) do { if (wifi_lwip_ops) wifi_lwip_ops->send_to_lwip((p), (n)); } while (0)
#define wifi_netif_set_up()        do { if (wifi_lwip_ops) wifi_lwip_ops->netif_set_up(); } while (0)
#define wifi_using_dhcp()          (wifi_lwip_ops ? wifi_lwip_ops->using_dhcp() : false)
#define wifi_get_ip()              (wifi_lwip_ops ? wifi_lwip_ops->get_ip() : 0)
#define wifi_get_dns(i)            (wifi_lwip_ops ? wifi_lwip_ops->get_dns(i) : 0)
#define wifi_get_ip6(a)            (wifi_lwip_ops ? wifi_lwip_ops->get_ip6(a) : false)
#define wifi_lwip_set_ip(a, g, m, d1, d2) \
    do { if (wifi_lwip_ops) wifi_lwip_ops->set_ip((a), (g), (m), (d1), (d2)); } while (0)

#else

#define wifi_lwip_set_ip(a, g, m, d1, d2) Wifi_SetIP((a), (g), (m), (d1), (d2))

#endif

#endif // DSWIFI_ENABLE_LWIP

#endif // LWIP_NDS_H__
