// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2021 Max Thomas
// Copyright (C) 2025 Antonio Niño Díaz

#ifndef DSWIFI_ARM7_WFC_H__
#define DSWIFI_ARM7_WFC_H__

#include <stdint.h>
#include <stdlib.h>

#include "common/wifi_shared.h"

// The firmware's connection settings (common/wfc_nvram.h: the ARM9 reads them
// too, wfc_read.c).
#include "common/wfc_nvram.h"

// Functions
// =========

// Ensure that WifiData is cleared before the functions are called. Then, call
// the NTR and TWL functions one after the other.
void Wifi_NTR_GetWfcSettings(void);
void Wifi_TWL_GetWfcSettings(bool allow_wpa);

int Wifi_GetWfcAccessPointIndex(const void *ssid, size_t ssid_len);

#endif // DSWIFI_ARM7_WFC_H__
