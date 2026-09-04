// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2025 Antonio Niño Díaz

#include <netinet/in.h>
#include <stdlib.h>
#include <errno.h>

#include <nds.h>
#include <nds/arm9/cp15_asm.h>
#include <dswifi9.h>

#include "arm9/ipc.h"
#include "arm9/lwip/lwip_nds.h"
#include "arm9/wifi_arm9.h"
#include "common/common_ntr_defs.h"
#include "common/ieee_defs.h"
#include "common/spinlock.h"

// Cached mirror. This should only be used when initializing the struct
static Wifi_MainStruct *WifiDataCached = NULL;

// Uncached mirror. This must be used by ARM9 code communicating with the ARM7.
volatile Wifi_MainStruct *WifiData = NULL;

void Wifi_CallSyncHandler(void)
{
    fifoSendValue32(FIFO_DSWIFI, WIFI_SYNC);
}

static void wifiValue32Handler(u32 value, void *data)
{
    (void)data;

    switch (value)
    {
        case WIFI_SYNC:
#if DSWIFI_ENABLE_LWIP
            // Don't do anything here. Receiving this message is enough to
            // activate the thread wifi_update_thread(), where we need to handle
            // all received messages.
#else
            // If lwIP is disabled there are no other threads.
            Wifi_Update();
#endif
            break;
        default:
            break;
    }
}

// How long to give the ARM7 to come up before giving up on it. Five seconds is
// far longer than it takes and short enough that a user isn't left guessing.
//
// The wait is a plain spin rather than a wait on the vertical blank interrupt.
// The point of a timeout is to hold even when something is badly wrong, and an
// interrupt that never arrives would stop the loop from ever completing an
// iteration, leaving the bound unable to apply. swiDelay() is a delay loop
// inside the BIOS, so it depends on nothing but the CPU running.
//
// swiDelay() spends about four cycles per iteration and the ARM9 runs at
// 67.03 MHz, so this is roughly a millisecond a chunk.
#define WIFI_ARM7_READY_DELAY_CHUNK 16757
#define WIFI_ARM7_READY_CHUNKS      5000

// How far the ARM7 got through its Wifi_Init() the last time initialization
// failed. Kept here because the shared struct is freed on the way out.
//
// Read twice, through the two views this CPU has of that struct. Normally they
// are the same memory and must agree. If the uncached view reads zero while the
// cached one doesn't, the mirror isn't uncached and this CPU has been reading
// its own stale copy; if neither shows what the ARM7 says it wrote, the two
// CPUs are not looking at the same memory at all.
static int wifi_init_fail_stage = WIFI_INIT_STAGE_NONE;
static int wifi_init_fail_stage_cached = WIFI_INIT_STAGE_NONE;


// Check that a candidate address really is another view of the same memory, and
// that reading it doesn't go through the data cache.
//
// The library talks to the ARM7 through an uncached mirror of main RAM so that
// neither CPU has to manage the cache, and it has always trusted memUncached()
// to name one. That trust is misplaced on some consoles: libnds picks the
// mirror from a model detection that reads SCFG_A9ROM at 0x04004000, an address
// that doesn't exist on a DS and answers with whatever was last on the bus. Get
// the wrong answer and the mirror lands at 0x0C000000, which is not memory on a
// DS: every read returns zero, so the ARM7 can finish its half of the
// initialization and the ARM9 never sees it.
//
// The three tests below are the three ways this can be wrong: a candidate that
// is the same address (no mirror at all), one that names different memory, and
// one that names the right memory but through the cache, which would go stale
// the moment the ARM7 writes.
static bool Wifi_MirrorWorks(Wifi_MainStruct *cached, volatile Wifi_MainStruct *mirror)
{
    if ((uintptr_t)mirror == (uintptr_t)cached)
        return false;

    // Only one byte is touched, and it is put back if the candidate turns out
    // to name somebody else's memory.
    u8 saved = mirror->initStage;

    uintptr_t field = (uintptr_t)&cached->initStage;
    uintptr_t line = field & ~((uintptr_t)CACHE_LINE_SIZE - 1);

    bool ok = false;

    // Write through the cached view, read it back through the candidate.
    cached->initStage = 0xA5;
    DC_FlushRange((void *)line, CACHE_LINE_SIZE);

    if (mirror->initStage == 0xA5)
    {
        // The same way round, to catch a candidate that only appears to work
        // because it answers with something constant.
        cached->initStage = 0x3C;
        DC_FlushRange((void *)line, CACHE_LINE_SIZE);

        // The read above populated the cache if this candidate is cacheable, so
        // a stale answer here means the mirror isn't uncached.
        if (mirror->initStage == 0x3C)
        {
            // And back the other way, which is the direction that matters: this
            // is how the ARM7 tells the ARM9 anything.
            mirror->initStage = 0x5A;
            DC_InvalidateRange((void *)line, CACHE_LINE_SIZE);

            if (cached->initStage == 0x5A)
                ok = true;
        }
    }

    if (!ok)
        mirror->initStage = saved;

    cached->initStage = WIFI_INIT_STAGE_NONE;
    DC_FlushRange((void *)line, CACHE_LINE_SIZE);

    return ok;
}

// The mirror of main RAM that each console model uses, tried in turn when the
// one libnds names doesn't work. Keeping them here rather than guessing from
// the model avoids repeating the detection that got it wrong in the first
// place: a candidate either mirrors the memory or it doesn't, and the test
// above says which.
static const struct
{
    u32 mask;
    u32 base;
} wifi_mirror_candidates[] =
{
    { 0x00FFFFFF, 0x0C000000 }, // DSi, 16 MB
    { 0x003FFFFF, 0x02C00000 }, // DS, 4 MB
    { 0x007FFFFF, 0x02800000 }, // DS debugger, 8 MB
};

// Find a view of the struct that the ARM7's writes actually reach.
static volatile Wifi_MainStruct *Wifi_SelectMirror(Wifi_MainStruct *cached)
{
    // What libnds says, first: when the model detection is right this is the
    // answer, and nothing below changes behaviour.
    volatile Wifi_MainStruct *mirror = memUncached(cached);

    if (Wifi_MirrorWorks(cached, mirror))
        return mirror;

    for (size_t i = 0; i < sizeof(wifi_mirror_candidates) / sizeof(wifi_mirror_candidates[0]); i++)
    {
        uintptr_t address = ((uintptr_t)cached & wifi_mirror_candidates[i].mask)
                          | wifi_mirror_candidates[i].base;

        mirror = (volatile Wifi_MainStruct *)address;

        if (Wifi_MirrorWorks(cached, mirror))
            return mirror;
    }

    return NULL;
}

static bool Wifi_InitIPC(unsigned int flags)
{
    assert(WifiDataCached == NULL);

    // See comment at the top of Wifi_MainStruct
    WifiDataCached = aligned_alloc(CACHE_LINE_SIZE, sizeof(Wifi_MainStruct));
    if (WifiDataCached == NULL)
        return false;

    // Clear the struct
    memset(WifiDataCached, 0, sizeof(Wifi_MainStruct));

    // Say which version of the library laid this out, so that an ARM7 built
    // against a different one can say so instead of reading the wrong fields.
    WifiDataCached->abi = WIFI_SHARED_ABI;

    DC_FlushRange(WifiDataCached, sizeof(Wifi_MainStruct));

    // Normally we will access the struct through an uncached mirror so that the
    // ARM7 and ARM9 always see the same values without any need for cache
    // management. Check that the one we get really is one, because on some
    // consoles it isn't -- see Wifi_MirrorWorks().
    WifiData = Wifi_SelectMirror(WifiDataCached);

    if (WifiData == NULL)
    {
        wifi_init_fail_stage = WIFI_INIT_STAGE_NO_MIRROR;

        free(WifiDataCached);
        WifiDataCached = NULL;

        return false;
    }

    // Start in Internet mode by default for compatibility with old code.
    if (flags & WIFI_LOCAL_ONLY)
        WifiData->reqLibraryMode = DSWIFI_MULTIPLAYER_CLIENT;
    else
        WifiData->reqLibraryMode = DSWIFI_INTERNET;

    WifiData->reqMode = WIFIMODE_DISABLED;

    // Use the LED by default
    if ((flags & WIFI_DISABLE_LED) == 0)
        WifiData->reqFlags |= WFLAG_REQ_USELED;

    // Use DSi mode only if has been requested and we're running on a DSi
    if ((flags & WIFI_ATTEMPT_DSI_MODE) && isDSiMode())
        WifiData->reqFlags |= WFLAG_REQ_DSI_MODE;

    // Set the default host name from the firmware settings.
    //
    // The length is clamped because it comes from memory the library doesn't
    // own. It is a byte, so an unexpected value writes up to 490 bytes past a
    // ten entry array and into the rest of this struct, which the ARM7 is
    // reading at the same time. On a normal boot it is never more than ten, but
    // a program started by DS Download Play inherits whatever the program before
    // it left in the settings area.
    u8 name_len = PersonalData->nameLen;
    u8 name_max = sizeof(WifiData->hostPlayerName) / sizeof(WifiData->hostPlayerName[0]);

    if (name_len > name_max)
        name_len = name_max;

    WifiData->hostPlayerNameLen = name_len;
    for (u8 i = 0; i < name_len; i++)
        WifiData->hostPlayerName[i] = PersonalData->name[i];

    // Send the cached mirror to the ARM7 (the ARM7 doesn't have cache, so the
    // cached address in main RAM is enough).
    //
    // Check that it was accepted. It is refused when the address isn't in main
    // RAM, and when the software queue behind the hardware FIFO is full, which
    // is what happens when the ARM7 has stopped draining its side. Without this
    // check both cases look exactly like an ARM7 that answered nothing.
    if (!fifoSendAddress(FIFO_DSWIFI, WifiDataCached))
    {
        wifi_init_fail_stage = WIFI_INIT_STAGE_ADDRESS_REFUSED;

        free(WifiDataCached);
        WifiDataCached = NULL;
        WifiData = NULL;

        return false;
    }

    // Wait for the ARM7 to be ready, but not for ever.
    //
    // The flag is set on the last line of the ARM7's Wifi_Init(), which only
    // runs when the message above reaches it. If the ARM7 never gets there --
    // stuck in its own start-up, or in one of the hardware waits inside the
    // initialisation -- this used to spin until the console was switched off,
    // with nothing on screen to say why. Failing is not a fix for whatever went
    // wrong, but it hands the problem back to the caller, which can say so.
    for (int i = 0; i < WIFI_ARM7_READY_CHUNKS; i++)
    {
        if (WifiData->flags7 & WFLAG_ARM7_ACTIVE)
        {
            wifi_init_fail_stage = WIFI_INIT_STAGE_NONE;
            return true;
        }

        swiDelay(WIFI_ARM7_READY_DELAY_CHUNK);
    }

    // Keep the last stage the ARM7 reported. Wifi_Init() runs in interrupt
    // context on that side, so a console stuck inside it can't be asked
    // anything else, and this says which call didn't return.
    wifi_init_fail_stage = WifiData->initStage;

    // Invalidate exactly the one cache line that holds the field, aligned at
    // both ends: DC_InvalidateRange() refuses a partial line, because dropping
    // one would throw away whatever else that line was holding. Only the field
    // matters here, and the struct is about to be freed anyway.
    uintptr_t field = (uintptr_t)&WifiDataCached->initStage;
    uintptr_t line = field & ~((uintptr_t)CACHE_LINE_SIZE - 1);

    DC_InvalidateRange((void *)line, CACHE_LINE_SIZE);

    wifi_init_fail_stage_cached = WifiDataCached->initStage;

    free(WifiDataCached);
    WifiDataCached = NULL;
    WifiData = NULL;

    return false;
}

int Wifi_GetInitFailStage(void)
{
    return wifi_init_fail_stage;
}

int Wifi_GetInitFailStageCached(void)
{
    return wifi_init_fail_stage_cached;
}

int Wifi_CheckInit(void)
{
    if (!WifiData)
        return 0;

    return 1;
}

bool Wifi_InitDefault(unsigned int flags)
{
    // You can't connect to WFC APs if the IP stack isn't initialized
    if ((flags & WIFI_LOCAL_ONLY) && (flags & WFC_CONNECT))
        return false;

    // DSi mode works only in Internet mode.
    if ((flags & WIFI_ATTEMPT_DSI_MODE) && (flags & WIFI_LOCAL_ONLY))
        return false;

    // Initialize the ARM7 side of the library and wait until it's ready
    if (!Wifi_InitIPC(flags))
        return false;

#ifdef DSWIFI_ENABLE_LWIP
    bool initialize_lwip = (flags & WIFI_LOCAL_ONLY) ? false : true;

    if (initialize_lwip)
    {
        if (wifi_lwip_init() != 0)
            return false;
    }
#endif

    // Clear FIFO queue
    while (fifoCheckValue32(FIFO_DSWIFI))
        fifoGetValue32(FIFO_DSWIFI);

    // Only start handling update events when everything else is ready
    fifoSetValue32Handler(FIFO_DSWIFI, wifiValue32Handler, 0);

    if (flags & WFC_CONNECT)
    {
#ifdef DSWIFI_ENABLE_LWIP
        int wifiStatus = ASSOCSTATUS_DISCONNECTED;

        Wifi_AutoConnect(); // request connect

        while (true)
        {
            // TODO: Stop using association status, use wifi mode
            wifiStatus = Wifi_AssocStatus(); // check status
            if (wifiStatus == ASSOCSTATUS_ASSOCIATED)
                break;
            if (wifiStatus == ASSOCSTATUS_CANNOTCONNECT)
                return false;
            cothread_yield_irq(IRQ_VBLANK);
        }
#else
        libndsCrash("DSWiFi built without lwIP");
#endif // DSWIFI_ENABLE_LWIP
    }

    return true;
}

bool Wifi_Deinit(void)
{
    // Only allow deinitializing DSWifi if the current mode is "disabled" and a
    // different mode hasn't been requested.
    if ((WifiData->reqMode != WIFIMODE_DISABLED) ||
        (WifiData->curMode != WIFIMODE_DISABLED))
        return false;

    fifoSetValue32Handler(FIFO_DSWIFI, NULL, 0);

    fifoSendValue32(FIFO_DSWIFI, WIFI_DEINIT);

    while (WifiData->flags7 & WFLAG_ARM7_ACTIVE)
        cothread_yield_irq(IRQ_VBLANK);

#ifdef DSWIFI_ENABLE_LWIP
    if (wifi_lwip_enabled)
        wifi_lwip_deinit();
#endif

    // Free the pointer in main RAM, not the one in the uncached mirror
    free(WifiDataCached);
    WifiDataCached = NULL;

    return true;
}

int Wifi_GetData(int datatype, int bufferlen, unsigned char *buffer)
{
    if (datatype < 0 || datatype >= MAX_WIFIGETDATA)
        return -1;

    switch (datatype)
    {
        case WIFIGETDATA_MACADDRESS:
            if (bufferlen < 6 || !buffer)
                return -1;
            memcpy(buffer, (void *)WifiData->MacAddr, 6);
            return 6;
        case WIFIGETDATA_NUMWFCAPS:
            return WifiData->wfc_number_of_configs;
        case WIFIGETDATA_RSSI:
            return WifiData->rssi;
    }

    return -1;
}

u32 Wifi_GetStats(int statnum)
{
    if (statnum < 0 || statnum >= NUM_WIFI_STATS)
        return 0;
    return WifiData->stats[statnum];
}

void Wifi_DisableWifi(void)
{
    WifiData->reqMode = WIFIMODE_DISABLED;
}

void Wifi_EnableWifi(void)
{
    WifiData->reqMode = WIFIMODE_NORMAL;
}

void Wifi_SetPromiscuousMode(int enable)
{
    if (enable)
        WifiData->reqFlags |= WFLAG_REQ_PROMISC;
    else
        WifiData->reqFlags &= ~WFLAG_REQ_PROMISC;
}

void Wifi_ScanModeFilter(Wifi_APScanFlags flags)
{
    WifiData->reqApScanFlags = flags;
    WifiData->reqMode = WIFIMODE_SCAN;
}

void Wifi_ScanMode(void)
{
    if (WifiData->curLibraryMode == DSWIFI_MULTIPLAYER_CLIENT)
        Wifi_ScanModeFilter(WSCAN_LIST_NDS_HOSTS);
    else if (WifiData->curLibraryMode == DSWIFI_INTERNET)
        Wifi_ScanModeFilter(WSCAN_LIST_AP_ALL);
    // Don't switch to scan mode when acting as a multiplayer host
}

void Wifi_IdleMode(void)
{
    WifiData->reqMode = WIFIMODE_NORMAL;
}

bool Wifi_LibraryModeReady(void)
{
    return WifiData->curLibraryMode == WifiData->reqLibraryMode;
}

void Wifi_InternetMode(void)
{
#ifdef DSWIFI_ENABLE_LWIP
    if (!wifi_lwip_enabled)
        return;

    WifiData->reqLibraryMode = DSWIFI_INTERNET;
    WifiData->reqMode = WIFIMODE_NORMAL;
#else
    libndsCrash("Internet mode not available without lwIP");
#endif
}

int Wifi_MultiplayerClientMode(size_t client_packet_size)
{
    // IEEE header, client AID, user data, FCS
    size_t client_size = HDR_DATA_MAC_SIZE + 1 + client_packet_size + 4;

    // Make sure client frames would fit in the space reserved for them
    if (client_size > MAC_CLIENT_RX_SIZE)
        return -1;

    WifiData->reqLibraryMode = DSWIFI_MULTIPLAYER_CLIENT;
    WifiData->reqMode = WIFIMODE_NORMAL;
    WifiData->reqReplyDataSize = client_size;

    return 0;
}

int Wifi_MultiplayerHostMode(int max_clients, size_t host_packet_size,
                             size_t client_packet_size)
{
    if (max_clients > WIFI_MAX_MULTIPLAYER_CLIENTS)
        max_clients = WIFI_MAX_MULTIPLAYER_CLIENTS;
    if (max_clients < 1)
        max_clients = 1;

    // IEEE header, client time, client bits, user data, FCS
    size_t host_size = HDR_DATA_MAC_SIZE + 2 + 2 + host_packet_size + 4;
    // IEEE header, client AID, user data, FCS
    size_t client_size = HDR_DATA_MAC_SIZE + 1 + client_packet_size + 4;

    // Make sure client frames would fit in the space reserved for them
    if (client_size > MAC_CLIENT_RX_SIZE)
        return -1;

    // Make sure CMD frames would fit in the buffer reserved for them
    if (HDR_TX_SIZE + host_size > MAC_CMDBUF_SIZE)
        return -1;

    WifiData->reqLibraryMode = DSWIFI_MULTIPLAYER_HOST;
    WifiData->reqMaxClients = max_clients;
    WifiData->reqCmdDataSize = host_size;
    WifiData->reqReplyDataSize = client_size;

    WifiData->reqMode = WIFIMODE_ACCESSPOINT;
    WifiData->reqFlags |= WFLAG_REQ_ALLOWCLIENTS;

    return 0;
}

void Wifi_MultiplayerAllowNewClients(bool allow)
{
    if (allow)
        WifiData->reqFlags |= WFLAG_REQ_ALLOWCLIENTS;
    else
        WifiData->reqFlags &= ~WFLAG_REQ_ALLOWCLIENTS;
}

void Wifi_MultiplayerHostName(const void *buffer, u8 len)
{
    // Copy data blindly: DSWifi doesn't use it for anything, it just sends it
    // as it is to the clients.
    WifiData->hostPlayerNameLen = len;
    memcpy((void *)WifiData->hostPlayerName, buffer, DSWIFI_BEACON_NAME_SIZE);
}

void Wifi_MultiplayerKickClientByAID(int association_id)
{
    if ((association_id < 1) || (association_id > WIFI_MAX_MULTIPLAYER_CLIENTS))
        return;

    WifiData->clients.reqKickClientAIDMask |= BIT(association_id);
}

void Wifi_SetChannel(int channel)
{
    if (channel < 1 || channel > 13)
        return;

    WifiData->reqChannel = channel;
}

int Wifi_TxBufferAllocBuffer(size_t total_size)
{
    u8 *txbufData = (u8 *)WifiData->txbufData;

    u32 write_idx = WifiData->txbufWrite;
    u32 read_idx = WifiData->txbufRead;

    assert((read_idx & 3) == 0); // Packets must be aligned to 32 bit
    assert((write_idx & 3) == 0);

    if (read_idx <= write_idx)
    {
        if ((write_idx + total_size) >= WIFI_TXBUFFER_SIZE)
        {
            // The packet doesn't fit at the end of the buffer:
            //
            //                    | NEW |
            //
            // | ......... | XXXX | . |           ("X" = Used, "." = Empty)
            //            RD      WR

            // Try to fit it at the beginning. Don't wrap it.
            if (total_size >= read_idx)
            {
                // The packet doesn't fit anywhere:

                // | NEW |            | NEW |
                //
                // | . | XXXXXXXXXXXX | . |
                //     RD             WR
                return -1;
            }

            // Write the stop marker before we write the wrap marker
            write_u32(txbufData + 0, 0);

            write_u32(txbufData + write_idx, WIFI_SIZE_WRAP);
            write_idx = 0;
        }
        else
        {
            // The packet fits at the end:
            //
            //               | NEW |
            //
            // | .... | XXXX | ...... |
            //       RD      WR
        }
    }
    else
    {
        if ((write_idx + total_size) >= read_idx)
        {
            //      | NEW |
            //
            // | XX | . | XXXXXXXXXXX |
            //     WR   RD

            return -1;
        }

        //      | NEW |
        //
        // | XX | ........ | XXXX |
        //     WR          RD
    }

    return write_idx;
}
