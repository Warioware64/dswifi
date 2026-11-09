// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#ifndef DSWIFI_ARM7_UPDATE_H__
#define DSWIFI_ARM7_UPDATE_H__

#include <stdbool.h>

void Wifi_Update(void);

// Wifi_SetUpdateFromLoop's: the DSi's wireless is driven from the program's
// loop (Wifi_LoopUpdate), never from an interrupt.
extern bool wifi_update_from_loop;

#endif // DSWIFI_ARM7_UPDATE_H__
