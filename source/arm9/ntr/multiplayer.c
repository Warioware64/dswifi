// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#include <nds.h>
#include <dswifi9.h>

#include "arm9/ipc.h"
#include "arm9/wifi_arm9.h"
#include "arm9/ntr/multiplayer.h"
#include "arm9/ntr/rx_tx_queue.h"
#include "common/ieee_defs.h"
#include "common/mac_addresses.h"
#include "common/spinlock.h"

// Functions to get information about clients connected to a host DS
// =================================================================

int Wifi_MultiplayerGetNumClients(void)
{
    if (WifiData->curLibraryMode != DSWIFI_MULTIPLAYER_HOST)
        return 0;

    u16 mask = WifiData->clients.aid_mask;

    int count = 0;
    for (int i = 0; i < WIFI_MAX_MULTIPLAYER_CLIENTS; i++)
    {
        if (mask & BIT(i + 1))
            count++;
    }

    return count;
}

u16 Wifi_MultiplayerGetClientMask(void)
{
    if (WifiData->curLibraryMode != DSWIFI_MULTIPLAYER_HOST)
        return 0;

    return WifiData->clients.aid_mask;
}

int Wifi_MultiplayerGetClients(int max_clients, Wifi_ConnectedClient *client_data)
{
    if (WifiData->curLibraryMode != DSWIFI_MULTIPLAYER_HOST)
        return 0;

    if ((max_clients <= 0) || (client_data == NULL))
        return -1;

    int oldIME = enterCriticalSection();
    while (Spinlock_Acquire(WifiData->clients) != SPINLOCK_OK);

    int c = 0;
    for (int i = 0; i < WIFI_MAX_MULTIPLAYER_CLIENTS; i++)
    {
        volatile Wifi_ConnectedClient *client = &(WifiData->clients.list[i]);

        if (client->state != WIFI_CLIENT_DISCONNECTED)
        {
            *client_data++ = *client; // Copy struct
            c++;
        }

        if (c == max_clients)
            break;
    }

    Spinlock_Release(WifiData->clients);
    leaveCriticalSection(oldIME);

    return c;
}

bool Wifi_MultiplayerClientGetMacFromAID(int aid, void *dest_macaddr)
{
    if (dest_macaddr == NULL)
        return false;

    if (aid < 1)
        return false;

    int oldIME = enterCriticalSection();
    while (Spinlock_Acquire(WifiData->clients) != SPINLOCK_OK);

    if (aid > WifiData->curMaxClients)
    {
        // Returning here without undoing the two lines above would leave the
        // lock held and interrupts disabled for good, hanging the console. A
        // client that reports an unexpected AID is exactly the case that has to
        // survive this.
        Spinlock_Release(WifiData->clients);
        leaveCriticalSection(oldIME);
        return false;
    }

    int index = aid - 1;

    volatile Wifi_ConnectedClient *client = &(WifiData->clients.list[index]);

    bool ret = false;

    if (client->state == WIFI_CLIENT_ASSOCIATED)
    {
        Wifi_CopyMacAddr(dest_macaddr, client->macaddr);
        ret = true;
    }

    Spinlock_Release(WifiData->clients);
    leaveCriticalSection(oldIME);

    return ret;
}

// Finds the client that owns a MAC address. Reply frames don't say which client
// sent them: the association ID is something the host already knows, from the
// address in the header of the frame. Returns 0 if no associated client has that
// address.
int Wifi_MultiplayerClientGetAIDFromMac(const void *macaddr)
{
    if (macaddr == NULL)
        return 0;

    int oldIME = enterCriticalSection();
    while (Spinlock_Acquire(WifiData->clients) != SPINLOCK_OK);

    int ret = 0;

    for (int i = 0; i < WifiData->curMaxClients; i++)
    {
        volatile Wifi_ConnectedClient *client = &(WifiData->clients.list[i]);

        if (client->state != WIFI_CLIENT_ASSOCIATED)
            continue;

        if (Wifi_CmpMacAddr(macaddr, client->macaddr))
        {
            ret = client->association_id;
            break;
        }
    }

    Spinlock_Release(WifiData->clients);
    leaveCriticalSection(oldIME);

    return ret;
}

bool Wifi_MultiplayerClientMatchesMacAndAID(int aid, const void *macaddr)
{
    if (macaddr == NULL)
        return false;

    if (aid < 1)
        return false;

    int oldIME = enterCriticalSection();
    while (Spinlock_Acquire(WifiData->clients) != SPINLOCK_OK);

    if (aid > WifiData->curMaxClients)
    {
        // Returning here without undoing the two lines above would leave the
        // lock held and interrupts disabled for good, hanging the console. A
        // client that reports an unexpected AID is exactly the case that has to
        // survive this.
        Spinlock_Release(WifiData->clients);
        leaveCriticalSection(oldIME);
        return false;
    }

    int index = aid - 1;

    volatile Wifi_ConnectedClient *client = &(WifiData->clients.list[index]);

    bool ret = false;

    if (client->state == WIFI_CLIENT_ASSOCIATED)
    {
        if (Wifi_CmpMacAddr(macaddr, client->macaddr))
            ret = true;
    }

    Spinlock_Release(WifiData->clients);
    leaveCriticalSection(oldIME);

    return ret;
}

// Multiplayer mode packet handlers
// ================================

WifiFromHostPacketHandler wifi_from_host_packet_handler = NULL;
WifiFromClientPacketHandler wifi_from_client_packet_handler = NULL;

void Wifi_MultiplayerFromHostSetPacketHandler(WifiFromHostPacketHandler func)
{
    wifi_from_host_packet_handler = func;
}

void Wifi_MultiplayerFromClientSetPacketHandler(WifiFromClientPacketHandler func)
{
    wifi_from_client_packet_handler = func;
}

static Wifi_MultiplayerRxDiag wifi_mp_rx_diag;

// Records why a frame was thrown away, along with the start of the frame itself.
// Without the bytes there is no way to tell a client that talks a different
// dialect from one that isn't talking at all.
static void Wifi_MultiplayerDropFrame(Wifi_MultiplayerDropReason reason,
                                      const u8 *packet, size_t size)
{
    wifi_mp_rx_diag.drops[reason]++;

    if (size > DSWIFI_MP_DIAG_BYTES)
        size = DSWIFI_MP_DIAG_BYTES;

    memcpy(wifi_mp_rx_diag.last_drop_frame, packet, size);
    wifi_mp_rx_diag.last_drop_len = size;
    wifi_mp_rx_diag.last_drop_reason = reason;
}

const Wifi_MultiplayerRxDiag *Wifi_MultiplayerGetRxDiag(void)
{
    return &wifi_mp_rx_diag;
}

void Wifi_MultiplayerGetHostCounters(Wifi_MultiplayerHostCounters *counters)
{
    if (counters == NULL)
        return;

    counters->cmd_armed = WifiData->mpCmdArmed;
    counters->cmd_retry = WifiData->mpCmdRetry;
    counters->cmd_done = WifiData->mpCmdDone;
    counters->cmd_failed = WifiData->mpCmdFailed;
    counters->reply_rx = WifiData->mpReplyRx;
    counters->reply_empty = WifiData->mpReplyEmpty;
}

void Wifi_MultiplayerHandlePacketFromClient(const u8 *packet, size_t size)
{
    if (wifi_from_client_packet_handler == NULL)
    {
        Wifi_MultiplayerDropFrame(DSWIFI_MP_DROP_NO_HANDLER, packet, size);
        return;
    }

    if (size < sizeof(MultiplayerClientIeeeDataFrame))
    {
        Wifi_MultiplayerDropFrame(DSWIFI_MP_DROP_TOO_SHORT, packet, size);
        return;
    }

    // Pointers to access the data

    const IEEE_DataFrameHeader *ieee = (const void *)packet;

    // Check packet type from header

    const u16 mask = FC_TO_DS | FC_FROM_DS | FC_TYPE_SUBTYPE_MASK;

    Wifi_MPPacketType type;

    // Only accept valid packet types
    if ((ieee->frame_control & mask) == (TYPE_DATA_CF_ACK | FC_TO_DS))
        type = WIFI_MPTYPE_REPLY;
    else if ((ieee->frame_control & mask) == (TYPE_DATA | FC_TO_DS))
        type = WIFI_MPTYPE_DATA;
    else
    {
        Wifi_MultiplayerDropFrame(DSWIFI_MP_DROP_FRAME_TYPE, packet, size);
        return;
    }

    if (type == WIFI_MPTYPE_REPLY)
    {
        // Check if it was sent to the magic multiplayer REPLY MAC address
        if (Wifi_CmpMacAddr(ieee->addr_3, wifi_reply_mac) == 0)
        {
            Wifi_MultiplayerDropFrame(DSWIFI_MP_DROP_REPLY_MAC, packet, size);
            return;
        }
    }
    else // if (type == WIFI_MPTYPE_DATA)
    {
        // Check if it was sent to us
        if (Wifi_CmpMacAddr(ieee->addr_3, WifiData->MacAddr) == 0)
        {
            Wifi_MultiplayerDropFrame(DSWIFI_MP_DROP_DEST_MAC, packet, size);
            return;
        }
    }

    size_t header_size = sizeof(MultiplayerClientIeeeDataFrame);

    // Work out which client sent this from its MAC address rather than from the
    // frame. The byte at offset 24 used to be read as an association ID, which
    // only works because DSWifi's own clients put one there. Consoles running
    // Nintendo's software use those two bytes for the size and flags of their
    // reply, so reading an ID out of them gives a number that belongs to nobody
    // and the frame is thrown away. The address in the header is right either
    // way, and the body starts at the same offset for both.
    int aid = Wifi_MultiplayerClientGetAIDFromMac(ieee->addr_2);

    if (aid == 0)
    {
        Wifi_MultiplayerDropFrame(DSWIFI_MP_DROP_AID_MISMATCH, packet, size);
        return;
    }

    wifi_mp_rx_diag.accepted++;

    (*wifi_from_client_packet_handler)(type, aid, (u32)(packet + header_size),
                                       size - header_size);
}

void Wifi_MultiplayerHandlePacketFromHost(const u8 *packet, size_t size)
{
    if (wifi_from_host_packet_handler == NULL)
        return;

    if (size < sizeof(MultiplayerHostIeeeDataFrame))
        return;

    // Pointers to access the data

    const IEEE_DataFrameHeader *ieee = (const void *)packet;

    // Check packet type from header

    const u16 mask = FC_TO_DS | FC_FROM_DS | FC_TYPE_SUBTYPE_MASK;

    Wifi_MPPacketType type;

    if ((ieee->frame_control & mask) == (TYPE_DATA_CF_POLL | FC_FROM_DS))
        type = WIFI_MPTYPE_CMD;
    else if ((ieee->frame_control & mask) == (TYPE_DATA | FC_FROM_DS))
        type = WIFI_MPTYPE_DATA;
    else
        return;

    // Read basic information from the data header

    size_t header_size;

    if (type == WIFI_MPTYPE_CMD)
    {
        header_size = sizeof(MultiplayerHostIeeeDataFrame);

        // Check if it was sent to the magic multiplayer CMD MAC address
        if (Wifi_CmpMacAddr(ieee->addr_1, wifi_cmd_mac) == 0)
            return;
    }
    else // if (type == WIFI_MPTYPE_DATA)
    {
        header_size = sizeof(IEEE_DataFrameHeader);

        // Check if it was sent to us
        if (Wifi_CmpMacAddr(ieee->addr_1, WifiData->MacAddr) == 0)
            return;
    }

    // Check that the source MAC is the BSSID we're connected to
    if (Wifi_CmpMacAddr(ieee->addr_3, WifiData->curAp.bssid) == 0)
        return;

    (*wifi_from_host_packet_handler)(type, (u32)(packet + header_size),
                                     size - header_size);
}
