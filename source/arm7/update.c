// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#include <nds.h>
#include <dswifi_common.h>

#include "arm7/debug.h"
#include "arm7/ipc.h"
#include "arm7/ntr/update.h"
#include "arm7/twl/update.h"

bool wifi_update_from_loop = false;

void Wifi_SetUpdateFromLoop(bool on)
{
    wifi_update_from_loop = on;
}

void Wifi_Update(void)
{
    if (WifiData == NULL)
        return;

    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
    {
        // (Updated from the program's loop instead: Wifi_LoopUpdate.)
        if (!wifi_update_from_loop)
            Wifi_TWL_Update();
    }
    else
    {
        Wifi_NTR_Update();
    }
}

void Wifi_LoopUpdate(void)
{
    if (WifiData == NULL || !wifi_update_from_loop)
        return;

    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
        Wifi_TWL_Update();
}
