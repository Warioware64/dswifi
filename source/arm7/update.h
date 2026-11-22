// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#ifndef DSWIFI_ARM7_UPDATE_H__
#define DSWIFI_ARM7_UPDATE_H__

#include <stdbool.h>

void Wifi_Update(void);

// Wifi_SetUpdateFromLoop's: the library is driven from the program's loop
// (Wifi_LoopUpdate), never from an interrupt nor from libnds's FIFO thread.
extern bool wifi_update_from_loop;

// What the FIFO's handlers left for Wifi_LoopUpdate, in loop mode: an init
// (its address), a deinit, an update the ARM9 asked for.
extern void *volatile wifi_init_due;
extern volatile bool wifi_deinit_due;
extern volatile bool wifi_sync_due;

#endif // DSWIFI_ARM7_UPDATE_H__
