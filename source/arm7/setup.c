// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#include <time.h>

#include "arm7/debug.h"
#include "arm7/ipc.h"
#include "arm7/setup.h"
#include "arm7/ntr/setup.h"
#include "arm7/twl/setup.h"
#include "common/random.h"

// Requires the data returned by the ARM9 WiFi init call.
//
// The data returned by the ARM9 Wifi_Init() function must be passed to the ARM7
// and then given to this function.
//
// This function also enables power to the WiFi system, which will shorten
// battery life.
u8 wifi_init_stage7 = WIFI_INIT_STAGE_NONE;

int Wifi_GetInitStage7(void)
{
    return wifi_init_stage7;
}

void Wifi_Init(void *wifidata)
{
    WifiData = (Wifi_MainStruct *)wifidata;

    // Before anything else: were the two halves built from the same library?
    //
    // Everything from here on reads a structure both CPUs have to agree on, and
    // a core built against a different version reads the wrong fields without
    // ever failing to link. Stopping here turns that into a number the ARM9 can
    // report, which is the difference between a message and an afternoon.
    if (WifiData->abi != WIFI_SHARED_ABI)
    {
        WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_ABI_MISMATCH);
        return;
    }

    // Record how far we get. This runs from the FIFO address handler, so it is
    // interrupt context: if any of the calls below doesn't return, the console
    // stops answering anything at all and this byte is the only way to find out
    // where it stopped. See WIFI_INIT_STAGE_* in wifi_shared.h.
    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_ENTERED);

    Wifi_RandomAddEntropy(time(NULL));

    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
        Wifi_TWL_Init();
    else
        Wifi_NTR_Init();

    WLOG_PUTS("W: ARM7 ready\n");
    WLOG_FLUSH();

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_READY);
    WifiData->flags7 |= WFLAG_ARM7_ACTIVE;
}

// This function cuts power to the WiFi system. After this WiFi will be unusable
// until Wifi_Init() is called again.
void Wifi_Deinit(void)
{
    WLOG_PUTS("W: Stopping WiFi\n");
    WLOG_FLUSH();

    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
        Wifi_TWL_Deinit();
    else
        Wifi_NTR_Deinit();

    WLOG_PUTS("W: WiFi stopped\n");
    WLOG_FLUSH();

    // Tell the ARM9 that the ARM7 is now idle
    WifiData->flags7 &= ~WFLAG_ARM7_ACTIVE;
    WifiData = NULL;
}

void Wifi_Start(void)
{
    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
        Wifi_TWL_Start();
    else
        Wifi_NTR_Start();
}

void Wifi_Stop(void)
{
    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
        Wifi_TWL_Stop();
    else
        Wifi_NTR_Stop();
}

void Wifi_SetupFilterMode(Wifi_FilterMode mode)
{
    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
        Wifi_TWL_SetupFilterMode(mode);
    else
        Wifi_NTR_SetupFilterMode(mode);
}
