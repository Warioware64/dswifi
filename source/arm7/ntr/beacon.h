// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2025 Antonio Niño Díaz

#ifndef DSWIFI_ARM7_NTR_BEACON_H__
#define DSWIFI_ARM7_NTR_BEACON_H__

#include <stddef.h>

#include <nds/ndstypes.h>

void Wifi_BeaconStop(void);
void Wifi_BeaconSetup(void);
void Wifi_SetBeaconChannel(int channel);

// Answers a probe request with the same information the beacon carries, minus
// the TIM, which is what Nintendo's firmware does in MakeProbeResFrame. A host
// that only sends beacons can only be found in the gaps between them.
//
// "ssid" is the one the request asked for, which may be empty to ask every host
// in range to answer. Returns 0 if nothing was sent.
int Wifi_SendProbeResponse(const void *dest_mac, const u8 *ssid, size_t ssid_len);
void Wifi_SetBeaconCurrentPlayers(int num);

void Wifi_SetBeaconAllowsConnections(int allows);
int Wifi_GetBeaconAllowsConnections(void);

void Wifi_SetBeaconPeriod(int beacon_period);

// Writes the next "extra data" fragment provided by the ARM9 to the beacon
// frame stored in MAC RAM, so that every beacon frame that is transmitted
// carries a different one. It is meant to be called from the pre-beacon
// interrupt handler.
void Wifi_BeaconRotateFragment(void);

// Applies the individual byte patches to the Nintendo vendor information tag
// requested by the ARM9.
void Wifi_BeaconApplyPatches(void);

#endif // DSWIFI_ARM7_NTR_BEACON_H__
