// SPDX-License-Identifier: MIT
//
// Copyright (C) 2026 The Half-Life DS port's authors

// The stack's end of the IP stack kept apart (dswifi_lwip.h): its table,
// handed to the hook build's core. In the stack's own library only.

#if defined(DSWIFI_ENABLE_LWIP) && defined(DSWIFI_LWIP_DSL)

#include <nds.h>
#include <dswifi9.h>
#include <dswifi_lwip.h>

#include "arm9/lwip/lwip_nds.h"

static void set_ip(uint32_t ip, uint32_t gateway, uint32_t netmask, uint32_t dns1, uint32_t dns2)
{
    Wifi_SetIP(ip, gateway, netmask, dns1, dns2);
}

static const Wifi_LwipOps lwip_ops = {
    .init = wifi_lwip_init,
    .deinit = wifi_lwip_deinit,
    .update = wifi_lwip_update,
    .netif_is_up = wifi_netif_is_up,
    .send_to_lwip = Wifi_SendPacketToLwip,
    .netif_set_up = wifi_netif_set_up,
    .using_dhcp = wifi_using_dhcp,
    .get_ip = wifi_get_ip,
    .get_dns = wifi_get_dns,
    .get_ip6 = wifi_get_ip6,
    .set_ip = set_ip,
};

void Wifi_LwipInstall(void)
{
    Wifi_SetLwipOps(&lwip_ops);
}

#endif
