// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2025 Antonio Niño Díaz

// Shared structures to be used by arm9 and arm7

#ifndef DSWIFI_COMMON_WIFI_SHARED_H__
#define DSWIFI_COMMON_WIFI_SHARED_H__

#include <nds.h>
#include <nds/arm9/cp15_asm.h>
#include <dswifi_common.h>

// Space reserved for incoming and outgoing packets
#define WIFI_RXBUFFER_SIZE  (1024 * 12)
#define WIFI_TXBUFFER_SIZE  (1024 * 24)

// Value written in RX/TX buffers to restart the pointer to the beginning
#define WIFI_SIZE_WRAP      0xFFFFFFFF

// Max number of Access Points that the library will keep track of
#define WIFI_MAX_AP         32

// Max number of saved WFC configurations
#define WIFI_MAX_WFC        6

// In scan mode, whenever there is a channel switch, the timeout counter in each
// AP is incremented. When WIFI_AP_TIMEOUT is reached, the AP is removed from
// the list. The timeout value lets us scan all 13 channels twice (plus a switch
// to return to the original channel) before any AP is removed from the list.
#define WIFI_AP_TIMEOUT (13 * 2 + 1)

// How the two CPUs agree that they were built from the same library.
//
// Everything below is shared between them, so the ARM9 and the ARM7 must have
// been compiled against the same version of it. Pairing an ARM7 core with an
// ARM9 built from a different one links without complaint and then reads the
// wrong fields, which produces no error at all -- and packaging a ready-made
// core (sys/arm7) makes that pairing easy to end up with by accident.
//
// So the ARM9 writes this in and the ARM7 checks it before touching anything
// else. Change it whenever the layout of Wifi_MainStruct or the MAC RAM map
// changes, which is the only thing that makes an old core wrong.
#define WIFI_SHARED_ABI     1

// Flags that inform us of the state of the ARM7
#define WFLAG_ARM7_ACTIVE  0x0001
#define WFLAG_ARM7_RUNNING 0x0002 // TODO: Delete? It seems redundant

// How far the ARM7 got through Wifi_Init(), stored in Wifi_MainStruct.initStage
// so that the ARM9 can name the call that didn't return.
//
// One value per call that can plausibly stop: everything here either talks to
// the hardware over a bus (SPI for the firmware, the baseband and radio serial
// interfaces) or waits on a flag the hardware alone clears. Zero is the value
// the ARM9 leaves in the struct, so it means the address message never reached
// the ARM7's FIFO handler.
#define WIFI_INIT_STAGE_NONE            0  // The ARM7 never entered Wifi_Init()
#define WIFI_INIT_STAGE_ENTERED         1  // Wifi_Init(), WifiData assigned
#define WIFI_INIT_STAGE_NTR_ENTERED     2  // Wifi_NTR_Init(), past the DSi GPIO
#define WIFI_INIT_STAGE_POWERED         3  // powerOff()/powerOn(POWER_WIFI)
#define WIFI_INIT_STAGE_FLASH_READ      4  // Wifi_FlashInitData(), SPI
#define WIFI_INIT_STAGE_SHUTDOWN        5  // Wifi_NTR_Stop()/Wifi_NTR_Shutdown()
#define WIFI_INIT_STAGE_MACMEM_CLEARED  6  // MAC RAM cleared
#define WIFI_INIT_STAGE_WFC_READ        7  // Wifi_NTR_GetWfcSettings()
#define WIFI_INIT_STAGE_WOKE_UP         8  // Wifi_NTR_WakeUp()
#define WIFI_INIT_STAGE_MAC_BB_INIT     9  // Wifi_MacInit()/Wifi_BBInit()
#define WIFI_INIT_STAGE_CHANNEL_SET     10 // Wifi_SetChannel(1)
#define WIFI_INIT_STAGE_NTR_DONE        11 // Wifi_NTR_Init() returning
#define WIFI_INIT_STAGE_READY           12 // About to set WFLAG_ARM7_ACTIVE

// Not a step the ARM7 reached: the ARM9 never managed to hand the address over.
// libnds refuses the send when the address isn't in main RAM, and when the
// queue behind the hardware FIFO is full because the ARM7 stopped draining its
// side. Without this the failure is indistinguishable from stage 0.
#define WIFI_INIT_STAGE_ADDRESS_REFUSED 255

// Nor is this one: the ARM7 was built against a different version of the library
// than the ARM9, so the structure they share doesn't mean the same thing to
// both. Rebuild both from the same tree, or point the program at the matching
// ARM7 core in sys/arm7.
#define WIFI_INIT_STAGE_ABI_MISMATCH    253

// Nor is this one: no view of the shared struct could be found that the ARM7's
// writes reach. See Wifi_MirrorWorks() in the ARM9's ipc.c.
#define WIFI_INIT_STAGE_NO_MIRROR       254

// Requests from the ARM9 to the ARM7
#define WFLAG_REQ_USELED        0x0001 // NTR only
#define WFLAG_REQ_PROMISC       0x0010 // NTR only
#define WFLAG_REQ_ALLOWCLIENTS  0x0040 // NTR only
#define WFLAG_REQ_DSI_MODE      0x0080
#define WFLAG_REQ_LOAD_WFC_KEY  0x0100 // Ask ARM7 to load the key from WFC data

// Enum values for the FIFO WiFi commands (FIFO_DSWIFI).
typedef enum
{
    WIFI_SYNC,
    WIFI_DEINIT,
}
DSWifi_IpcCommands;

// Modes of operation of DSWifi
typedef enum {
    // Connect to an access point to access the Internet.
    DSWIFI_INTERNET,
    // Connect to a DS acting as access point for local multiplayer.
    DSWIFI_MULTIPLAYER_CLIENT,
    // Act as access point for other DSs to connect to for local multiplayer.
    DSWIFI_MULTIPLAYER_HOST,
} DSWifi_Mode;

enum WIFI_MODE
{
    // The WiFi hardware is off.
    WIFIMODE_DISABLED,
    // The WiFi hardware is initializing (TWL).
    WIFIMODE_INITIALIZING,
    // The WiFi hardware is on, but idle.
    WIFIMODE_NORMAL,
    // The ARM7 is iterating through all channels looking for access points.
    WIFIMODE_SCAN,
    // The ARM7 is trying to connect to an AP
    WIFIMODE_CONNECTING,
    // The ARM7 is connected to the AP.
    WIFIMODE_CONNECTED,
    // The ARM7 is disconnecting from an AP (TWL).
    WIFIMODE_DISCONNECTING,
    // The ARM7 is unable to connect to the AP.
    WIFIMODE_CANNOTCONNECT,
    // The WiFi hardware is on and acting as an AP (multiplayer host).
    WIFIMODE_ACCESSPOINT,
};

enum WIFI_AUTHLEVEL
{
    WIFI_AUTHLEVEL_DISCONNECTED,
    WIFI_AUTHLEVEL_AUTHENTICATED,
    WIFI_AUTHLEVEL_ASSOCIATED,
    WIFI_AUTHLEVEL_DEASSOCIATED,
    WIFI_AUTHLEVEL_ERROR,
};

// Returns the size in bytes
static inline size_t Wifi_WepKeySizeFromMode(enum WEPMODES wepmode)
{
    switch (wepmode)
    {
        case WEPMODE_NONE:
            return 0;
        case WEPMODE_64BIT:
        case WEPMODE_64BIT_ASCII:
            return 5;
        case WEPMODE_128BIT:
        case WEPMODE_128BIT_ASCII:
            return 13;
        case WEPMODE_152BIT:
        case WEPMODE_152BIT_ASCII:
            return 16; // Unused
    }

    return 0;
}

static inline enum WEPMODES Wifi_WepModeFromKeySize(size_t size)
{
    if (size == 5)
        return WEPMODE_64BIT;
    else if (size == 13)
        return WEPMODE_128BIT;
    else if (size == 16)
        return WEPMODE_152BIT;

    return WEPMODE_NONE;
}

typedef struct {
    // List of clients connected
    Wifi_ConnectedClient list[WIFI_MAX_MULTIPLAYER_CLIENTS];

    // Mask of AIDs of clients currently associated (not authenticated!)
    u16 aid_mask;

    // Number of clients currently connected
    u8 num_connected;

    // Mask of AIDs that the ARM9 has requested to kick out
    u8 reqKickClientAIDMask;

    // Current AID when the DS is connected to a host DS
    u8 curClientAID;

    // Internal lock to access this struct. This only needs to be aqcuired when
    // the ARM7 is writing to the struct and when the ARM9 is reading from it.
    // The ARM7 is free to read from it without using the lock as long as
    // interrupts are disabled.
    u32 spinlock;
} Wifi_ClientsInfoIpc;

// Security information about an AP
typedef struct {
    u8 pass_len; // Length of the password. For WEP it must be 5, 13 or 16.
    u8 pass[64]; // Max size for WPA is 64 bytes
    u8 pmk[32];  // For WPA and WPA2
} Wifi_ApSecurity;

// This struct is allocated in main RAM, but it is only accessed through an
// uncached mirror. We use aligned_alloc() to ensure that the beginning of the
// struct isn't in the same cache line as other variables, but we need to pad
// the end of the struct to fill a cache line so that variables that follow the
// struct are in a different line.
//
// Without this padding, the ARM9 may access a variable right next to this
// struct, so a cache line will be loaded, including the current values of the
// variables at the start or end of this struct (the ones that share the same
// line). Later, the ARM7 may modify them, but they will be restored to their
// previous value when the ARM9 flushes the line. With the additional padding
// this can't happen.
typedef struct WIFI_MAINSTRUCT
{
    // Global library information
    // --------------------------

    // Current mode (enum WIFI_MODE). Written only by Wifi_Update() in the ARM7.
    u8 curMode;
    // Requested mode. Written only by the ARM9.
    u8 reqMode;

    // Current channel and requested channel. NTR mode
    u8 curChannel, reqChannel;

    // Current AP connection status. NTR mode
    u8 authlevel, authctr;

    // Written by the ARM9 before the ARM7 is told where this struct is, and
    // checked by the ARM7 before it reads anything else. See WIFI_SHARED_ABI.
    u32 abi;

    u32 flags7; // Current status of the ARM7
    u16 reqFlags; // ARM9 writes requests, the ARM7 reads them

    // How far the ARM7 got through Wifi_Init(). Written only by the ARM7, read
    // by the ARM9 when the ARM7 fails to report itself ready.
    //
    // Wifi_Init() runs from the FIFO address handler, which is interrupt
    // context, so a console that stops inside it stops servicing every other
    // interrupt too and can't be asked anything. This byte is the only thing
    // that gets out. The ARM9 clears the whole struct before handing its
    // address over, so zero means the ARM7 never entered Wifi_Init() at all.
    // See WIFI_INIT_STAGE_* below.
    u8 initStage;
    u32 counter7; // NTR mode
    u16 MacAddr[3]; // MAC address of this console

    // Mode of operation of DSWifi. Check enum DSWifi_Mode
    u8 curLibraryMode, reqLibraryMode;

    // Access Point information
    // ------------------------

    // Data of the AP the ARM9 has requested and the one currently being used by
    // the ARM7.
    Wifi_AccessPoint curAp;

    // Security information of the current AP
    Wifi_ApSecurity curApSecurity;

    u8 maxrate7;
    bool realRates;
    u8 rssi;
#if 0
    u16 pspoll_period; // TODO: This is currently set but unused
#endif

    // Scanned AP data
    Wifi_AccessPoint aplist[WIFI_MAX_AP];
    u8 curApScanFlags, reqApScanFlags;

    // WFC data
    u8 wfc_number_of_configs; // Total number of configs loaded
    struct {
        u8  ssid[33];
        u8  ssid_len;
        u32 ip;
        u32 gateway;
        u32 subnet_mask;
        u32 dns_primary;
        u32 dns_secondary;
        Wifi_ApSecurity security;
    } wfc[WIFI_MAX_WFC];

    // ARM9 <-> ARM7 transfer circular buffers
    // ---------------------------------------

    // RX buffer. It sends received packets from other devices from the ARM7
    // to the ARM9.
    u32 rxbufWrite; // We will write starting from this entry in rxbufData[]
    u32 rxbufRead;  // And we will read starting from this entry in rxbufData[]
    ALIGN(4) u8 rxbufData[WIFI_RXBUFFER_SIZE];

    // TX buffer. It is used to send packets from the ARM9 to the ARM7 to be
    // transferred to other devices.
    u32 txbufWrite; // We will write starting from this entry in txbufData[]
    u32 txbufRead;  // And we will read starting from this entry in txbufData[]
    ALIGN(4) u8 txbufData[WIFI_TXBUFFER_SIZE];

    // Local multiplay information (NTR mode only)
    // ---------------------------

    // This struct can only be modified by the ARM7, the ARM9 should only read
    // from it.
    Wifi_ClientsInfoIpc clients;

    // Maximum number of clients allowed by this host (up to 15)
    u8 curMaxClients, reqMaxClients;

    u16 curCmdDataSize, reqCmdDataSize;
    u16 curReplyDataSize, reqReplyDataSize;

    u16 hostPlayerName[10]; // UTF-16LE
    u8 hostPlayerNameLen;

    // Beacon vendor information element (NTR multiplayer host mode)
    // ------------------------------------------------------------

    // Table of interchangeable "extra data" blocks. Before every beacon
    // transmission the ARM7 writes one of them into the beacon stored in MAC
    // RAM, cycling through all of them. This is how DS Download Play splits a
    // game information record that is too big for one beacon. A count of 0 or 1
    // disables cycling.
    u8 beaconFragments[DSWIFI_BEACON_MAX_FRAGMENTS][DSWIFI_BEACON_EXTRA_DATA_MAX];
    u8 beaconFragmentSize;
    u8 beaconFragmentCount;
    // Incremented by the ARM9 whenever the table above is modified, so that the
    // ARM7 knows that it has to read the new contents.
    u8 beaconFragmentGeneration;

    // Ring buffer of single byte patches to apply to the Nintendo vendor
    // information element of the beacon. The offsets are relative to the first
    // byte of the element. This is used for fields that change while the beacon
    // is being transmitted, like the beacon type.
    struct {
        u8 offset;
        u8 value;
    } beaconPatch[8];
    u8 beaconPatchWrite;
    u8 beaconPatchRead;

    // Layout of the extra data of the beacon, which determines whether the ARM7
    // is allowed to update some of its fields on its own. See
    // Wifi_BeaconExtraDataLayout.
    u8 beaconExtraDataLayout;

    // Written by the ARM7 so that the ARM9 can tell whether the fragments of the
    // beacon are really being cycled. Without this a beacon that is never
    // updated looks exactly like one that is updated but rejected.
    //
    // beaconRotateSkip holds a Wifi_BeaconRotateSkip value describing what the
    // last attempt did.
    u16 beaconPreTbttCount;
    u16 beaconFragmentWrites;
    u8 beaconRotateSkip;

    // Also written by the ARM7, for the multiplayer host. Together these say
    // whether a stalled transfer is the host not transmitting or the client not
    // answering.
    //
    // mpCmdArmed counts distinct CMD frames handed to the hardware and
    // mpCmdRetry the times one was handed over again after a failure, so the two
    // have to be read together: the hardware sees their sum. mpCmdFailed counts
    // the transfers the hardware reported as failed, which for a CMD frame means
    // the replies it expected never arrived.
    u16 mpCmdArmed;
    u16 mpCmdRetry;
    u16 mpCmdDone;
    u16 mpCmdFailed;
    u16 mpReplyRx;
    u16 mpReplyEmpty;

    // Other information
    // -----------------

    // Stats data
    u32 stats[NUM_WIFI_STATS];

    // Semirandom number updated at the convenience of the ARM7. Used for
    // initial seeds and such. Don't count on it being updated every frame.
    u32 hardware_rng_seed;

    // The following two values are normally used by each CPU when they
    // generate random numbers. They are seeded from hardware_rng_seed by the
    // ARM7. They must never be set to zero.
    u32 random7, random9;

    // End
    // ---

    u8 padding[CACHE_LINE_SIZE]; // See comment at top of struct
} Wifi_MainStruct;

static inline u32 round_up_32(u32 value)
{
    return (value + 3) & ~3;
}

static inline u16 read_u16(const u8 *ptr)
{
    return *(u16 *)ptr;
}

static inline u32 read_u32(const u8 *ptr)
{
    return *(u32 *)ptr;
}

static inline void write_u32(u8 *ptr, u32 val)
{
    *(u32 *)ptr = val;
}

#endif // DSWIFI_COMMON_WIFI_SHARED_H__
