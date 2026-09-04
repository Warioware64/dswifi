// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2025 Antonio Niño Díaz

/// @file dswifi7.h
///
/// @brief ARM7 header of DSWifi.

#ifndef DSWIFI7_H
#define DSWIFI7_H

#ifdef __cplusplus
extern "C" {
#endif

#include <nds/ndstypes.h>

#include <dswifi_version.h>

/// Sync function to ensure data continues to flow between the two CPUs smoothly.
///
/// It Should be called at a periodic interval, such as in VBlank.
void Wifi_Update(void);

/// Setup system FIFO handler for WiFi library messages.
void installWifiFIFO(void);

/// How far the ARM7 got through its half of the initialization.
///
/// The same number the ARM9 reads with Wifi_GetInitFailStage(), but taken from
/// this CPU's own memory instead of the struct the two CPUs share. It is only
/// useful while working out why initialization failed: if the ARM9 reads zero
/// and this doesn't, the two CPUs disagree about where that struct is, which is
/// a different problem from the ARM7 never having started.
///
/// @return
///     The last step this CPU reached, or zero if it reached none.
int Wifi_GetInitStage7(void);

#ifdef __cplusplus
}
#endif

#endif // DSWIFI7_H
