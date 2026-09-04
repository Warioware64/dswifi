// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#include <nds.h>

#include "arm9/ipc.h"
#include "arm9/wifi_arm9.h"
#include "arm9/ntr/beacon.h"
#include "arm9/ntr/rx_tx_queue.h"
#include "common/common_ntr_defs.h"
#include "common/ieee_defs.h"
#include "common/mac_addresses.h"

// The multi-byte fields of the Nintendo vendor information element are stored
// in little endian order, which is the order used by official software.

static void Wifi_FieWrite16(u8 *dest, u16 value)
{
    dest[0] = (value >> 0) & 0xFF;
    dest[1] = (value >> 8) & 0xFF;
}

static void Wifi_FieWrite32(u8 *dest, u32 value)
{
    dest[0] = (value >> 0) & 0xFF;
    dest[1] = (value >> 8) & 0xFF;
    dest[2] = (value >> 16) & 0xFF;
    dest[3] = (value >> 24) & 0xFF;
}

// Access points created by official games acting as multiplayer hosts have no
// encryption and no BSSID.

static int Wifi_BeaconStartInternal(const u8 *ssid, size_t ssid_len,
                                    const Wifi_BeaconVendorInfo *info)
{
    u8 data[MAC_BEACON_SIZE];

    if (ssid_len > 32)
        return -1;
    if ((ssid_len > 0) && (ssid == NULL))
        return -1;

    if (info != NULL)
    {
        if (info->extra_data_size > DSWIFI_BEACON_EXTRA_DATA_MAX)
            return -1;
        if ((info->extra_data_size > 0) && (info->extra_data == NULL))
            return -1;
    }

    WifiData->curAp.ssid_len = ssid_len;
    for (size_t i = 0; i < sizeof(WifiData->curAp.ssid); i++)
        WifiData->curAp.ssid[i] = (i < ssid_len) ? ssid[i] : '\0';

    // Copy hardware TX and IEEE headers
    // =================================

    Wifi_TxHeader *tx = (void *)data;
    IEEE_MgtFrameHeader *ieee = (void *)(data + sizeof(Wifi_TxHeader));
    u8 *body = (void *)(((u8 *)ieee) + sizeof(IEEE_MgtFrameHeader));

    // Hardware TX header
    // ------------------

    memset(tx, 0, sizeof(Wifi_TxHeader));
    tx->tx_rate = WIFI_TRANSFER_RATE_2MBPS; // This is always 2 Mbit/s

    // IEEE 802.11 header
    // ------------------

    ieee->frame_control = TYPE_BEACON;
    ieee->duration = 0;
    Wifi_CopyMacAddr(ieee->da, wifi_broadcast_addr);
    Wifi_CopyMacAddr(ieee->sa, WifiData->MacAddr); // SA and BSSID are the same
    Wifi_CopyMacAddr(ieee->bssid, WifiData->MacAddr);
    ieee->seq_ctl = 0;

    // Frame body
    // ----------

    size_t body_size = 0;

    // Timestamp
    for (int i = 0; i < 8; i++)
        *body++ = 0;
    body_size += 8;

    // Beacon interval

    *(u16 *)body = 100; // It is common for the Beacon interval to be set to 100
                        // time units (interval between Beacon transmissions of
                        // approximately 100 ms).
    body += 2;
    body_size += 2;

    // Capability info

    *(u16 *)body = CAPS_ESS | CAPS_SHORT_PREAMBLE;
    body += 2;
    body_size += 2;

    // SSID
    //
    // 802.11 requires this element to be present, and to come first. Access
    // points that don't publish a name send it with a length of zero rather than
    // leaving it out.
    //
    // A DS Download Play host is the exception: a retail game hosting one was
    // captured with the sniffer, and its beacon goes straight from the
    // capability information to the supported rates, with no SSID element at
    // all. GBATEK says the same. The SSID is still recorded above, because a
    // client derives one from the game ID and the stream code and sends it in
    // its association and probe requests.

    if ((info == NULL) || !info->omit_ssid)
    {
        *body++ = MGT_FIE_ID_SSID;
        *body++ = ssid_len;
        for (size_t i = 0; i < ssid_len; i++)
            *body++ = ssid[i];
        body_size += 2 + ssid_len;
    }

    // Supported rates

    *body++ = MGT_FIE_ID_SUPPORTED_RATES;
    *body++ = 2;
    *body++ = RATE_MANDATORY | RATE_2_MBPS;
    *body++ = RATE_MANDATORY | RATE_1_MBPS;
    body_size += 4;

    // DS parameter set (WiFi channel)

    *body++ = MGT_FIE_ID_DS_PARAM_SET;
    *body++ = 1;
    *body++ = WifiData->reqChannel; // This will be modified by the ARM7
    body_size += 3;

    // TIM
    //
    // The hardware fills in the traffic map itself, but the length and the DTIM
    // period are ours to get right. Every DS host beacon captured with the
    // sniffer carries five bytes here with a DTIM period of 2, which is also
    // what GBATEK documents; this used to send six zeros, and a client that
    // sleeps between beacons reads this to decide when to listen.

    *body++ = MGT_FIE_ID_TIM;
    *body++ = 5;
    *body++ = 0; // DTIM count, adjusted by the hardware
    *body++ = 2; // DTIM period
    *body++ = 0; // Bitmap control
    *body++ = 0; // Partial virtual bitmap, filled by the hardware
    *body++ = 0;
    body_size += 7;

    // Vendor (Nintendo)

    if (info != NULL)
    {
        *body++ = MGT_FIE_ID_VENDOR;
        *body++ = sizeof(FieVendorNintendoHeader) + info->extra_data_size;
        body_size += 2;

        FieVendorNintendoHeader *hdr = (void *)body;
        memset(hdr, 0, sizeof(FieVendorNintendoHeader));

        hdr->oui[0] = 0x00;
        hdr->oui[1] = 0x09;
        hdr->oui[2] = 0xBF;
        hdr->oui_type = 0x00;

        Wifi_FieWrite16(hdr->stepping_offset, info->stepping_offset);
        Wifi_FieWrite16(hdr->lcd_video_sync, info->lcd_video_sync);
        Wifi_FieWrite32(hdr->fixed_id, info->fixed_id);
        Wifi_FieWrite32(hdr->game_id, info->game_id);
        Wifi_FieWrite16(hdr->stream_code, info->stream_code);

        hdr->extra_data_size = info->extra_data_size;
        hdr->beacon_type = info->beacon_type;

        Wifi_FieWrite16(hdr->cmd_data_size, info->cmd_data_size);
        Wifi_FieWrite16(hdr->reply_data_size, info->reply_data_size);

        body += sizeof(FieVendorNintendoHeader);
        body_size += sizeof(FieVendorNintendoHeader);

        if (info->extra_data_size > 0)
        {
            memcpy(body, info->extra_data, info->extra_data_size);
            body += info->extra_data_size;
            body_size += info->extra_data_size;
        }

        WifiData->beaconExtraDataLayout = info->extra_data_layout;
    }
    else
    {
        WifiData->beaconExtraDataLayout = DSWIFI_BEACON_LAYOUT_RAW;
    }

    // Total size to add to the buffer
    size_t frame_size =
        sizeof(Wifi_TxHeader) + sizeof(IEEE_MgtFrameHeader) +
        body_size + // Actual size of the data in the memory block
        4; // FCS

    // The beacon is stored in a dedicated buffer in MAC RAM, so it can't be
    // bigger than it.
    if (frame_size > MAC_BEACON_SIZE)
        return -1;

    size_t total_size = sizeof(u32) + round_up_32(frame_size) + sizeof(u32);

    tx->tx_length = frame_size - sizeof(Wifi_TxHeader);

    // Send frame to the ARM7
    // ----------------------

    int oldIME = enterCriticalSection();

    int alloc_idx = Wifi_TxBufferAllocBuffer(total_size);
    if (alloc_idx == -1)
    {
        WifiData->stats[WSTAT_TXQUEUEDREJECTED]++;
        leaveCriticalSection(oldIME);
        return -1;
    }

    u32 write_idx = alloc_idx;

    u8 *txbufData = (u8 *)WifiData->txbufData;

    // Skip writing the size until we've finished the packet
    u32 size_idx = write_idx;
    write_idx += sizeof(u32);

    // Data
    // ----

    memcpy(txbufData + write_idx, &data,
           sizeof(Wifi_TxHeader) + sizeof(IEEE_MgtFrameHeader) + body_size);
    write_idx += sizeof(Wifi_TxHeader) + sizeof(IEEE_MgtFrameHeader) + body_size;

    // FCS
    // ---

    write_idx += 4;

    // Done
    // ----

    write_idx = round_up_32(write_idx); // Pad to 32 bit

    // Mark the next block as empty, but don't move pointer so that the size of
    // the next block is written here eventually.
    write_u32(txbufData + write_idx, 0);

    assert(write_idx <= (WIFI_TXBUFFER_SIZE - sizeof(u32)));

    WifiData->txbufWrite = write_idx;

    // Now that the packet is finished, write real size of data without padding
    // or the size of the size tags
    write_u32(txbufData + size_idx, frame_size | WFLAG_SEND_AS_BEACON);

    leaveCriticalSection(oldIME);

    WifiData->stats[WSTAT_TXQUEUEDPACKETS]++;
    WifiData->stats[WSTAT_TXQUEUEDBYTES] += frame_size;

    Wifi_CallSyncHandler();

    return 0;
}

int Wifi_BeaconStartEx(const char *ssid, const Wifi_BeaconVendorInfo *info)
{
    return Wifi_BeaconStartInternal((const u8 *)ssid,
                                    (ssid != NULL) ? strlen(ssid) : 0, info);
}

int Wifi_BeaconStartRawSsid(const void *ssid, size_t ssid_len,
                            const Wifi_BeaconVendorInfo *info)
{
    return Wifi_BeaconStartInternal(ssid, ssid_len, info);
}

int Wifi_BeaconStart(const char *ssid, u32 game_id)
{
    DSWifiExtraData extra;

    memset(&extra, 0, sizeof(extra));

    extra.players_max = WifiData->curMaxClients + 1; // Clients + host
    extra.players_current = 1; // No clients, only host. Updated from the ARM7
    extra.allows_connections = WifiData->reqFlags & WFLAG_REQ_ALLOWCLIENTS ? 1 : 0;

    extra.name_len = WifiData->hostPlayerNameLen;
    for (u8 i = 0; i < DSWIFI_BEACON_NAME_SIZE / sizeof(u16); i++)
    {
        extra.name[i * 2] = WifiData->hostPlayerName[i] & 0xFF;
        extra.name[(i * 2) + 1] = (WifiData->hostPlayerName[i] >> 8) & 0xFF;
    }

    Wifi_BeaconVendorInfo info =
    {
        // DSWifi has always stored the game ID in the beacon in big endian
        // order, unlike official software. The byte swap preserves that so that
        // hosts and clients of older versions of the library keep working.
        .game_id = __builtin_bswap32(game_id),
        .beacon_type = 1, // Multicart
        // Note that these are the sizes of the whole multiplayer frames, not
        // the sizes of the data that the user can send in them.
        .cmd_data_size = WifiData->curCmdDataSize,
        .reply_data_size = WifiData->curReplyDataSize,
        .extra_data = &extra,
        .extra_data_size = sizeof(DSWifiExtraData),
        .extra_data_layout = DSWIFI_BEACON_LAYOUT_DSWIFI,
    };

    int ret = Wifi_BeaconStartEx(ssid, &info);

    // Clamped for the same reason as in Wifi_InitIPC(): the length is a byte
    // read from memory the library doesn't own, and the array holds ten.
    u8 name_len = PersonalData->nameLen;
    u8 name_max = sizeof(WifiData->hostPlayerName) / sizeof(WifiData->hostPlayerName[0]);

    if (name_len > name_max)
        name_len = name_max;

    for (u8 i = 0; i < name_len; i++)
        WifiData->hostPlayerName[i] = PersonalData->name[i];

    return ret;
}

void Wifi_BeaconGetRotateStatus(int *pre_tbtt, int *writes, int *skip_reason)
{
    if (pre_tbtt != NULL)
        *pre_tbtt = WifiData->beaconPreTbttCount;
    if (writes != NULL)
        *writes = WifiData->beaconFragmentWrites;
    if (skip_reason != NULL)
        *skip_reason = WifiData->beaconRotateSkip;
}

int Wifi_BeaconSetExtraDataFragments(const void *fragments, size_t fragment_size,
                                     int count)
{
    if ((count < 1) || (count > DSWIFI_BEACON_MAX_FRAGMENTS))
        return -1;
    if ((fragment_size == 0) || (fragment_size > DSWIFI_BEACON_EXTRA_DATA_MAX))
        return -1;
    if (fragments == NULL)
        return -1;

    const u8 *src = fragments;

    int oldIME = enterCriticalSection();

    for (int i = 0; i < count; i++)
    {
        memcpy((void *)WifiData->beaconFragments[i], src + (i * fragment_size),
               fragment_size);
    }

    WifiData->beaconFragmentSize = fragment_size;
    WifiData->beaconFragmentCount = count;

    // Let the ARM7 know that the table has changed. This must be done last.
    WifiData->beaconFragmentGeneration++;

    leaveCriticalSection(oldIME);

    return 0;
}

int Wifi_BeaconPatchVendorByte(size_t offset, u8 value)
{
    if (offset >= (sizeof(FieVendorNintendoHeader) + DSWIFI_BEACON_EXTRA_DATA_MAX))
        return -1;

    int oldIME = enterCriticalSection();

    u8 write = WifiData->beaconPatchWrite;
    u8 next = (write + 1) % (sizeof(WifiData->beaconPatch) / sizeof(WifiData->beaconPatch[0]));

    if (next == WifiData->beaconPatchRead)
    {
        // The ring buffer is full, the ARM7 hasn't consumed the patches yet.
        leaveCriticalSection(oldIME);
        return -1;
    }

    WifiData->beaconPatch[write].offset = offset;
    WifiData->beaconPatch[write].value = value;
    WifiData->beaconPatchWrite = next;

    leaveCriticalSection(oldIME);

    return 0;
}
