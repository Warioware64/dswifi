// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2025 Antonio Niño Díaz
// Copyright (C) 2026 The Half-Life DS port's authors

// The console's saved connections read from its firmware by the ARM9, for a
// program to show or copy (Wifi_ReadWfcSlots, dswifi9.h): the same checks as
// the ARM7's, when the wireless starts (arm7/wfc.c).

#include <stdlib.h>
#include <string.h>

#include <nds.h>
#include <dswifi9.h>

#include "common/wfc_nvram.h"
#include "common/wifi_shared.h"

static u16 wfc_crc16(const u8 *data, int length)
{
    int crc = 0x0000;
    for (int i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc & 0xFFFF;
}

static bool wfc_all_zero(const u8 *data, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        if (data[i] != 0)
            return false;
    }
    return true;
}

// A prefix length (1..0x1C leading ones) as a mask in network byte order.
static u32 wfc_netmask(u8 ones)
{
    u32 s = 0;
    for (int n = 0; n < ones && n < 32; n++)
        s |= 1u << (31 - n);
    return (s << 24) | (s >> 24) | ((s & 0xFF00) << 8) | ((s & 0xFF0000) >> 8);
}

static void wfc_common(Wifi_WfcSlot *out, const char *ssid, const u8 *ip, const u8 *gateway,
                       const u8 *dns1, const u8 *dns2, u8 subnet_mask)
{
    int len = 0;
    while (len < 32 && ssid[len] != 0)
        len++;
    memcpy(out->ssid, ssid, len);
    out->ssid[len] = 0;
    out->ssid_len = len;
    memcpy(&out->ip, ip, 4);
    memcpy(&out->gateway, gateway, 4);
    memcpy(&out->dns1, dns1, 4);
    memcpy(&out->dns2, dns2, 4);
    out->subnet_mask = subnet_mask ? wfc_netmask(subnet_mask) : 0;
}

static void wfc_wep(Wifi_WfcSlot *out, u8 wep_mode, const u8 *key)
{
    out->wep_mode = wep_mode;
    if (wep_mode == WEPMODE_NONE)
        return;
    out->security = WFC_SECURITY_WEP;
    out->key_len = Wifi_WepKeySizeFromMode(wep_mode);
    memcpy(out->key, key, out->key_len);
}

int Wifi_ReadWfcSlots(Wifi_WfcSlot *slots, int most)
{
    if (slots == NULL || most <= 0)
        return 0;

    // (In main memory, for the firmware's reads: libnds passes them to the
    // ARM7 by address.)
    u8 *buffer = malloc(sizeof(nvram_cfg_twl));
    if (buffer == NULL)
        return 0;

    int count = 0;

    u16 settings_offset = 0;
    if (readFirmware(F_USER_SETTINGS_OFFSET, buffer, 2) != 0)
        goto done;
    memcpy(&settings_offset, buffer, 2);

    // Slots 1 to 3: every console's.
    u32 base = settings_offset * 8 - 0x400;
    for (int i = 0; i < 3 && count < most; i++)
    {
        nvram_cfg_ntr *ap = (nvram_cfg_ntr *)buffer;
        if (readFirmware(base + i * sizeof(nvram_cfg_ntr), buffer, sizeof(nvram_cfg_ntr)) != 0)
            continue;
        if (wfc_all_zero(buffer, sizeof(nvram_cfg_ntr)) || wfc_crc16(buffer, 256) != 0)
            continue;
        if (ap->status != 0x00 || ap->wep_mode > 7)
            continue; // (AOSS, deleted, or not readable)

        Wifi_WfcSlot *out = &slots[count];
        memset(out, 0, sizeof(*out));
        out->slot = i + 1;
        wfc_common(out, ap->ssid, ap->ip, ap->gateway, ap->primary_dns, ap->secondary_dns,
                   ap->subnet_mask);
        wfc_wep(out, ap->wep_mode, ap->wep_key1);
        count++;
    }

    // Slots 4 to 6: a DSi's own (WPA possible).
    if (!isDSiMode())
        goto done;

    base = settings_offset * 8 - 0xA00;
    for (int i = 0; i < 3 && count < most; i++)
    {
        nvram_cfg_twl *ap = (nvram_cfg_twl *)buffer;
        if (readFirmware(base + i * sizeof(nvram_cfg_twl), buffer, sizeof(nvram_cfg_twl)) != 0)
            continue;
        if (wfc_all_zero(buffer, sizeof(nvram_cfg_twl)) || wfc_crc16(buffer, 256) != 0 ||
            wfc_crc16(buffer + 256, 256) != 0)
            continue;
        if (ap->slot_idx == 0x00 || ap->wep_mode > 7)
            continue;

        Wifi_WfcSlot *out = &slots[count];
        memset(out, 0, sizeof(*out));
        out->slot = i + 4;
        wfc_common(out, ap->ssid, ap->ip, ap->gateway, ap->primary_dns, ap->secondary_dns,
                   ap->subnet_mask);
        wfc_wep(out, ap->wep_mode, ap->wep_key1);
        if (ap->wpa_mode != 0)
        {
            out->security = WFC_SECURITY_WPA;
            int len = 0;
            while (len < 64 && ap->pass[len] != 0)
                len++;
            memcpy(out->key, ap->pass, len);
            out->key_len = len;
        }
        count++;
    }

done:
    free(buffer);
    return count;
}
