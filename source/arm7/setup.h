// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2025 Antonio Niño Díaz

#ifndef DSWIFI_ARM7_SETUP_H__
#define DSWIFI_ARM7_SETUP_H__

#include <nds/ndstypes.h>

void Wifi_Init(void *wifidata);
void Wifi_Deinit(void);

// The stage the ARM7 reached in Wifi_Init(), kept on this side as well as in
// the shared struct. The two are written together, so a difference between them
// says the two CPUs disagree about where the struct is rather than that the
// ARM7 stopped. See WIFI_INIT_STAGE_* in wifi_shared.h.
extern u8 wifi_init_stage7;

#define WIFI_SET_INIT_STAGE(stage)          \
    do                                      \
    {                                       \
        wifi_init_stage7 = (stage);         \
        WifiData->initStage = (stage);      \
    } while (0)

void Wifi_Start(void);
void Wifi_Stop(void);

typedef enum {
    WIFI_FILTERMODE_IDLE,
    WIFI_FILTERMODE_SCAN,
    WIFI_FILTERMODE_INTERNET,
    WIFI_FILTERMODE_MULTIPLAYER_HOST,
    WIFI_FILTERMODE_MULTIPLAYER_CLIENT,
    WIFI_FILTERMODE_PROMISCUOUS,
} Wifi_FilterMode;

void Wifi_SetupFilterMode(Wifi_FilterMode mode);

#endif // DSWIFI_ARM7_SETUP_H__
