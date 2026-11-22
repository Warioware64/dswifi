// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#include <nds.h>
#include <dswifi_common.h>

#include "arm7/debug.h"
#include "arm7/ipc.h"
#include "arm7/setup.h"
#include "arm7/ntr/update.h"
#include "arm7/twl/update.h"

bool wifi_update_from_loop = false;

void *volatile wifi_init_due = NULL;
volatile bool wifi_deinit_due = false;
volatile bool wifi_sync_due = false;

void Wifi_SetUpdateFromLoop(bool on)
{
    wifi_update_from_loop = on;
}

// Wifi_Update() is called when the ARM7 receives a WIFI_SYNC message, but it's
// also called by hand in the VBL Handler of the default ARM7 cores. If
// interrupts are enabled inside Wifi_Update() it is possible that the interrupt
// is a VBL or FIFO interrupt that calls it again. This can cause problems, so
// make sure that the second time the function is called it exits right away.
// For example, it can cause stack overflows. (Upstream's 59d2636; here it also
// covers Wifi_LoopUpdate, which an interrupt's Wifi_Update must not run into.)
static volatile bool wifi_update_active = false;

void Wifi_Update(void)
{
    if (WifiData == NULL)
        return;

    if (wifi_update_active)
        return;

    wifi_update_active = true;

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

    wifi_update_active = false;
}

void Wifi_LoopUpdate(void)
{
    if (!wifi_update_from_loop)
        return;

    // What the FIFO's handlers left: an init, then a deinit, then an update.
    void *init = wifi_init_due;
    if (init != NULL)
    {
        wifi_init_due = NULL;
        Wifi_Init(init);
    }

    if (WifiData == NULL)
        return;

    if (wifi_update_active)
        return;

    if (wifi_deinit_due)
    {
        wifi_deinit_due = false;
        wifi_sync_due = false;
        Wifi_Deinit();
        return;
    }

    wifi_update_active = true;

    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
    {
        wifi_sync_due = false;
        Wifi_TWL_Update();
    }
    else if (wifi_sync_due)
    {
        wifi_sync_due = false;
        Wifi_NTR_Update();
    }

    wifi_update_active = false;
}
