// SPDX-License-Identifier: MIT
//
// Copyright (C) 2026 The Half-Life DS port's authors

// The core's end of the IP stack kept apart (dswifi_lwip.h): the hook build
// only.

#if defined(DSWIFI_ENABLE_LWIP) && defined(DSWIFI_LWIP_HOOKED) && !defined(DSWIFI_LWIP_DSL)

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include <dswifi_lwip.h>

const Wifi_LwipOps *wifi_lwip_ops = NULL;

// The stack's own flag (lwip/netif.c sets it as it starts): held here, by the
// core, which reads it whether a stack is installed or not.
bool wifi_lwip_enabled = false;

void Wifi_SetLwipOps(const Wifi_LwipOps *ops)
{
    wifi_lwip_ops = ops;
}

bool Wifi_LwipInstalled(void)
{
    return wifi_lwip_ops != NULL;
}

// (The stack's errno, set here: include_dsl/lwipopts.h says why.)
void dswifi_set_errno(int err)
{
    errno = err;
}

#endif
