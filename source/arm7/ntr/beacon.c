// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2025 Antonio Niño Díaz

#include <string.h>

#include "arm7/debug.h"
#include "arm7/ipc.h"
#include "arm7/ntr/beacon.h"
#include "arm7/ntr/mac.h"
#include "arm7/ntr/registers.h"
#include "arm7/ntr/tx_queue.h"
#include "common/common_ntr_defs.h"
#include "common/ieee_defs.h"
#include "common/mac_addresses.h"
#include "common/wifi_shared.h"

// The probe response is built here rather than on the stack: it is as big as a
// beacon frame, and it is built from the frame received interrupt.
static u8 probe_response[MAC_BEACON_SIZE];

// Address to the channel inside the beacon frame saved in MAC RAM. Note that
// this isn't required to be aligned to a halfword.
static u16 beacon_channel_addr = 0;

// Address of the first byte (the OUI) of the Nintendo vendor information tag
// inside the beacon frame saved in MAC RAM.
static u16 vendor_ie_addr = 0;

// Address of the extra data that follows the fixed part of the Nintendo vendor
// information tag, and the size announced for it in the tag itself.
static u16 beacon_extra_data_addr = 0;
static u8 beacon_extra_data_size = 0;

// Index of the extra data fragment currently stored in the beacon, and the last
// generation of the fragment table that the ARM7 has seen.
static u8 beacon_fragment_index = 0;
static u8 beacon_fragment_generation = 0;

// Patches to individual bytes of the Nintendo vendor information tag requested
// by the ARM9. They are kept here (instead of being applied and forgotten)
// because they need to be applied again after every fragment change.
#define BEACON_MAX_STICKY_PATCHES   8

static u8 sticky_patch_offset[BEACON_MAX_STICKY_PATCHES];
static u8 sticky_patch_value[BEACON_MAX_STICKY_PATCHES];
static u8 sticky_patch_count = 0;

// Whether the beacon is currently announcing that the host accepts new clients.
// It is tracked here instead of being read back from MAC RAM because beacons
// that don't use the DSWifi layout don't have this field at all.
static bool beacon_allows_connections = false;

// Returns true if the ARM7 is allowed to update the fields of the extra data
// that DSWifi defines. Beacons of other formats (like the ones of DS Download
// Play) are owned by the application, which stores unrelated information in
// those same bytes.
static bool Wifi_BeaconExtraDataIsDSWifi(void)
{
    if (WifiData->beaconExtraDataLayout != DSWIFI_BEACON_LAYOUT_DSWIFI)
        return false;

    if (beacon_extra_data_size < sizeof(DSWifiExtraData))
        return false;

    return true;
}

void Wifi_BeaconStop(void)
{
    W_TXBUF_BEACON &= ~TXBUF_BEACON_ENABLE;
    W_BEACONINT = 0x64;
}

void Wifi_BeaconSetup(void)
{
    u8 data[MAC_BEACON_END_OFFSET - MAC_BEACON_START_OFFSET];

    int packetlen = Wifi_MACReadHWord(MAC_BEACON_START_OFFSET, HDR_TX_IEEE_FRAME_SIZE);
    int len = HDR_TX_SIZE + packetlen - 4;

    // Start reading after the headers, timestamp, beacon interval and
    // capability info.
    int i = HDR_TX_SIZE + HDR_MGT_MAC_SIZE + 8 + 2 + 2;
    if (len <= i)
    {
        // Disable beacon transmission if the beacon packet is too small to be a
        // valid beacon.
        Wifi_BeaconStop();
        return;
    }

    WLOG_PRINTF("W: Beacon setup (%d b)\n", len);

    if (len > (int)sizeof(data))
        len = sizeof(data);

    // Read the whole frame so that it's easier to parse
    Wifi_MACRead((u16 *)data, MAC_BEACON_START_OFFSET, 0, len);

    // A new beacon replaces the contents of the old one, so any information
    // that referred to the old one is now invalid.
    vendor_ie_addr = 0;
    beacon_extra_data_addr = 0;
    beacon_extra_data_size = 0;
    beacon_fragment_index = 0;
    beacon_fragment_generation = WifiData->beaconFragmentGeneration;
    sticky_patch_count = 0;

    // The ARM9 has just built this beacon from the requested flags, so this is
    // what it is announcing right now.
    beacon_allows_connections = WifiData->reqFlags & WFLAG_REQ_ALLOWCLIENTS ? true : false;

    while (i < len)
    {
        int type      = data[i++];
        size_t seglen = data[i++];

        switch (type)
        {
            case MGT_FIE_ID_DS_PARAM_SET: // Channel
                // Address in MAC RAM to the channel field in the beacon frame
                beacon_channel_addr = MAC_BEACON_START_OFFSET + i;
                break;

            case MGT_FIE_ID_TIM: // TIM

                // TIM offset within beacon frame body (skipping headers)
                W_TXBUF_TIM = i - HDR_TX_SIZE - HDR_MGT_MAC_SIZE;

                W_LISTENINT = data[i + 1]; // Listen interval
                if (W_LISTENCOUNT >= W_LISTENINT)
                    W_LISTENCOUNT = 0;

                break;

            case MGT_FIE_ID_VENDOR:

                if ((seglen >= sizeof(FieVendorNintendoHeader)) &&
                    // Nintendo OUI
                    (data[i + 0] == 0x00) && (data[i + 1] == 0x09) &&
                    (data[i + 2] == 0xBF) && (data[i + 3] == 0x00))
                {
                    WLOG_PUTS("W: Nintendo info found\n");

                    vendor_ie_addr = MAC_BEACON_START_OFFSET + i;

                    // The extra data follows the fixed part of the tag. Trust
                    // the size announced in the tag, but never let it go past
                    // the end of the tag itself.
                    size_t extra_size = data[i + FIE_NINTENDO_OFS_EXTRA_DATA_SIZE];
                    if (extra_size > (seglen - sizeof(FieVendorNintendoHeader)))
                        extra_size = seglen - sizeof(FieVendorNintendoHeader);

                    beacon_extra_data_addr =
                        vendor_ie_addr + sizeof(FieVendorNintendoHeader);
                    beacon_extra_data_size = extra_size;
                }

                break;
        }
        i += seglen;
    }

    // Enable beacon transmission now that we have a valid beacon
    W_TXBUF_BEACON = TXBUF_BEACON_ENABLE | (MAC_BEACON_START_OFFSET >> 1);

    // Beacon interval
    W_BEACONINT = ((u16 *)data)[(HDR_TX_SIZE + HDR_MGT_MAC_SIZE + 8) / 2];

    // Refresh channel
    Wifi_SetBeaconChannel(WifiData->curChannel);

    WLOG_FLUSH();
}

int Wifi_SendProbeResponse(const void *dest_mac, const u8 *ssid, size_t ssid_len)
{
    // There is nothing to answer with until a beacon has been set up.
    if (!(W_TXBUF_BEACON & TXBUF_BEACON_ENABLE))
        return 0;

    int packetlen = Wifi_MACReadHWord(MAC_BEACON_START_OFFSET, HDR_TX_IEEE_FRAME_SIZE);
    int len = HDR_TX_SIZE + packetlen - 4; // Without the checksum

    // The information elements start after the headers, the timestamp, the
    // beacon interval and the capability information.
    int first = HDR_TX_SIZE + HDR_MGT_MAC_SIZE + 8 + 2 + 2;

    if ((len <= first) || (len > (int)sizeof(probe_response)))
        return 0;

    Wifi_MACRead((u16 *)probe_response, MAC_BEACON_START_OFFSET, 0, (len + 1) & ~1);

    // Answer a request that names our SSID, or one that names none at all, which
    // is how a client asks every host in range to identify itself.
    //
    // The SSID comes from what was announced when the beacon was built rather
    // than from the beacon itself, because a DS Download Play host doesn't put
    // it in the frame: a client works it out from the game ID and the stream
    // code and only ever sends it back.
    if (ssid_len != 0)
    {
        if (ssid_len != WifiData->curAp.ssid_len)
            return 0;

        for (size_t i = 0; i < ssid_len; i++)
        {
            if (ssid[i] != WifiData->curAp.ssid[i])
                return 0;
        }
    }

    // Copy the elements over, dropping the TIM. It says when the host will next
    // have traffic buffered for a sleeping client, which only means anything in
    // a frame that is transmitted on a schedule.
    int out = first;
    int i = first;

    while ((i + 2) <= len)
    {
        int type = probe_response[i];
        int seglen = probe_response[i + 1];

        // Ignore a truncated element instead of reading past the frame
        if ((i + 2 + seglen) > len)
            break;

        if (type != MGT_FIE_ID_TIM)
        {
            if (out != i)
                memmove(probe_response + out, probe_response + i, 2 + seglen);

            out += 2 + seglen;
        }

        i += 2 + seglen;
    }

    IEEE_MgtFrameHeader *ieee = (void *)(probe_response + HDR_TX_SIZE);

    ieee->frame_control = TYPE_PROBE_RESPONSE;
    ieee->duration = 0;
    Wifi_CopyMacAddr(ieee->da, dest_mac);
    ieee->seq_ctl = 0;

    size_t ieee_size = out - HDR_TX_SIZE;
    size_t tx_size = HDR_TX_SIZE + ieee_size;

    // The rest of the hardware header belongs to the beacon it was copied from,
    // and a beacon is transmitted by a different part of the hardware.
    Wifi_TxHeader *tx = (void *)probe_response;

    memset(tx, 0, sizeof(Wifi_TxHeader));
    tx->tx_rate = WIFI_TRANSFER_RATE_2MBPS;
    tx->tx_length = ieee_size + 4; // Checksum

    return Wifi_TxArm7QueueAdd((u16 *)probe_response, tx_size);
}

void Wifi_SetBeaconChannel(int channel)
{
    if (beacon_channel_addr == 0)
        return;

    // This function edits the channel of the beacon frame that we have saved in
    // MAC RAM (if we have saved one!).

    if (W_TXBUF_BEACON & TXBUF_BEACON_ENABLE)
        Wifi_MacWriteByte(beacon_channel_addr, channel);
}

void Wifi_SetBeaconCurrentPlayers(int num)
{
    if (beacon_extra_data_addr == 0)
        return;

    // In any other layout these bytes belong to the application.
    if (!Wifi_BeaconExtraDataIsDSWifi())
        return;

    if (W_TXBUF_BEACON & TXBUF_BEACON_ENABLE)
        Wifi_MacWriteByte(beacon_extra_data_addr + 1, num);
}

void Wifi_SetBeaconAllowsConnections(int allows)
{
    beacon_allows_connections = allows ? true : false;

    if (beacon_extra_data_addr == 0)
        return;

    // In any other layout these bytes belong to the application.
    if (!Wifi_BeaconExtraDataIsDSWifi())
        return;

    if (W_TXBUF_BEACON & TXBUF_BEACON_ENABLE)
        Wifi_MacWriteByte(beacon_extra_data_addr + 2, allows);
}

int Wifi_GetBeaconAllowsConnections(void)
{
    return beacon_allows_connections ? 1 : 0;
}

// Writes all the patches requested by the ARM9 to the beacon frame in MAC RAM.
static void Wifi_BeaconWriteStickyPatches(void)
{
    for (int i = 0; i < sticky_patch_count; i++)
        Wifi_MacWriteByte(vendor_ie_addr + sticky_patch_offset[i], sticky_patch_value[i]);
}

void Wifi_BeaconApplyPatches(void)
{
    // Move the patches requested by the ARM9 to our own list. They are kept
    // there because a fragment change overwrites part of the vendor information
    // tag, so they need to be applied again afterwards.
    while (WifiData->beaconPatchRead != WifiData->beaconPatchWrite)
    {
        u8 read = WifiData->beaconPatchRead;
        u8 offset = WifiData->beaconPatch[read].offset;
        u8 value = WifiData->beaconPatch[read].value;

        WifiData->beaconPatchRead =
            (read + 1) % (sizeof(WifiData->beaconPatch) / sizeof(WifiData->beaconPatch[0]));

        // If there is already a patch for this offset, replace its value.
        int index = -1;
        for (int i = 0; i < sticky_patch_count; i++)
        {
            if (sticky_patch_offset[i] == offset)
            {
                index = i;
                break;
            }
        }

        if (index == -1)
        {
            if (sticky_patch_count == BEACON_MAX_STICKY_PATCHES)
                continue; // No space left, drop the patch

            index = sticky_patch_count;
            sticky_patch_count++;
        }

        sticky_patch_offset[index] = offset;
        sticky_patch_value[index] = value;
    }

    if (vendor_ie_addr == 0)
        return;

    if (W_TXBUF_BEACON & TXBUF_BEACON_ENABLE)
        Wifi_BeaconWriteStickyPatches();
}

void Wifi_BeaconRotateFragment(void)
{
    // This is only called from the PreTBTT interrupt, so counting calls counts
    // the beacons that the hardware is about to transmit.
    WifiData->beaconPreTbttCount++;

    if (beacon_extra_data_addr == 0)
    {
        WifiData->beaconRotateSkip = DSWIFI_BEACON_ROTATE_NO_VENDOR_IE;
        return;
    }

    if (!(W_TXBUF_BEACON & TXBUF_BEACON_ENABLE))
    {
        WifiData->beaconRotateSkip = DSWIFI_BEACON_ROTATE_BEACON_OFF;
        return;
    }

    u8 count = WifiData->beaconFragmentCount;
    u8 size = WifiData->beaconFragmentSize;

    // A single fragment (or none at all) means that the extra data never
    // changes, so there is nothing to do here.
    if ((count < 2) || (size == 0))
    {
        WifiData->beaconRotateSkip = DSWIFI_BEACON_ROTATE_NO_FRAGMENTS;
        return;
    }

    // The fragments must be exactly as big as the extra data announced in the
    // beacon, or we would be writing over unrelated fields.
    if (size != beacon_extra_data_size)
    {
        WifiData->beaconRotateSkip = DSWIFI_BEACON_ROTATE_SIZE_MISMATCH;
        return;
    }

    WifiData->beaconRotateSkip = DSWIFI_BEACON_ROTATE_OK;
    WifiData->beaconFragmentWrites++;

    // If the ARM9 has replaced the table, start again from the first fragment.
    u8 generation = WifiData->beaconFragmentGeneration;
    if (generation != beacon_fragment_generation)
    {
        beacon_fragment_generation = generation;
        beacon_fragment_index = 0;
    }

    Wifi_MacWriteBytes(beacon_extra_data_addr,
                       (const u8 *)WifiData->beaconFragments[beacon_fragment_index],
                       size);

    // The fragment we have just written has overwritten any patch that falls
    // inside the extra data, so they all need to be applied again.
    Wifi_BeaconWriteStickyPatches();

    beacon_fragment_index++;
    if (beacon_fragment_index >= count)
        beacon_fragment_index = 0;
}

void Wifi_SetBeaconPeriod(int beacon_period)
{
    if (beacon_period < 0x10 || beacon_period > 0x3E7)
        return;

    W_BEACONINT = beacon_period;
}
