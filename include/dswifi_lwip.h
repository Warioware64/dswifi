// SPDX-License-Identifier: MIT
//
// Copyright (C) 2026 The Half-Life DS port's authors

#ifndef DSWIFI_LWIP_H__
#define DSWIFI_LWIP_H__

/// @file dswifi_lwip.h
///
/// @brief The IP stack kept apart from the library's core (this fork's).
///
/// libdswifi_dl9_hook.a is the library without lwIP: it reaches the stack
/// through a table, empty until the stack is installed, and does what
/// libdswifi_dl9_noip.a does until then. The stack is libdswifi_dl9_lwip.a
/// (lwIP and its glue, UDP over IPv4 with DHCP and DNS), built to be linked
/// into a dynamic library loaded at run time; Wifi_LwipInstall(), which it
/// defines, fills the table. Install it before Wifi_InitDefault() without
/// WIFI_LOCAL_ONLY. Once that has run, the stack cannot be taken away again:
/// lwIP never stops.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct in6_addr;

/// What the core asks of the IP stack.
typedef struct Wifi_LwipOps
{
    int (*init)(void);
    void (*deinit)(void);
    void (*update)(void);
    bool (*netif_is_up)(void);
    void (*send_to_lwip)(uint8_t *packet, size_t size);
    void (*netif_set_up)(void);
    bool (*using_dhcp)(void);
    uint32_t (*get_ip)(void);
    uint32_t (*get_dns)(int index);
    bool (*get_ip6)(struct in6_addr *addr);
    void (*set_ip)(uint32_t ip, uint32_t gateway, uint32_t netmask, uint32_t dns1, uint32_t dns2);
} Wifi_LwipOps;

/// The core's side: the table it is to use (NULL: none). In the hook build.
void Wifi_SetLwipOps(const Wifi_LwipOps *ops);

/// Whether a stack has been installed. In the hook build.
bool Wifi_LwipInstalled(void);

/// The stack's side: its table handed to the core. In the stack's library.
void Wifi_LwipInstall(void);

#ifdef __cplusplus
}
#endif

#endif // DSWIFI_LWIP_H__
