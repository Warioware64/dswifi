// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2025 Antonio Niño Díaz

/// @file dswifi_common.h
///
/// @brief ARM9 and ARM7 common definitions.

#ifndef DSWIFI_COMMON_H
#define DSWIFI_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>

#include <nds/ndstypes.h>

/// Maxmimum number of multiplayer clients that can connect to a host.
///
/// This is limited by W_AID_LOW, which can only go from 0 to 15 (with 0 being
/// reserved by the host).
#define WIFI_MAX_MULTIPLAYER_CLIENTS 15

/// Size in bytes reserved in beacon frames for the multiplayer host player name.
#define DSWIFI_BEACON_NAME_SIZE 20

/// Biggest "extra data" block that can be stored in the Nintendo vendor
/// information element of a beacon frame.
///
/// DS Download Play uses blocks of this exact size.
#define DSWIFI_BEACON_EXTRA_DATA_MAX 0x70

/// Maximum number of interchangeable "extra data" blocks of a beacon frame.
///
/// DS Download Play splits its game information record in 10 blocks.
#define DSWIFI_BEACON_MAX_FRAGMENTS 10

/// Layout of the "extra data" of the Nintendo vendor information element.
///
/// It determines which fields of the beacon frame the ARM7 is allowed to update
/// on its own while the beacon is being transmitted.
typedef enum {
    /// Format used by DSWifi in local multiplayer mode. The ARM7 keeps the
    /// number of connected players and the "allows connections" flag updated.
    DSWIFI_BEACON_LAYOUT_DSWIFI = 0,
    /// Format defined by the application, like the one of DS Download Play. The
    /// ARM7 never modifies the extra data on its own.
    DSWIFI_BEACON_LAYOUT_RAW    = 1,
} Wifi_BeaconExtraDataLayout;

/// This AP is an ad hoc AP. Not used.
#define WFLAG_APDATA_ADHOC         0x0001 // TODO: Unused
/// This AP uses WEP encryption.
#define WFLAG_APDATA_WEP           0x0002
/// This AP uses WPA or WPA2 encryption.
#define WFLAG_APDATA_WPA           0x0004
/// This AP is compatible with DSWiFI.
#define WFLAG_APDATA_COMPATIBLE    0x0008
/// This AP is compatible with DSWiFI, with tricks. Not unused.
#define WFLAG_APDATA_EXTCOMPATIBLE 0x0010 // TODO: Unused
/// This AP contains Nintendo AP information, it's likely a multiplayer host.
#define WFLAG_APDATA_NINTENDO_TAG  0x0040
/// The WFC settings of the console have configuration data for this AP.
#define WFLAG_APDATA_CONFIG_IN_WFC 0x0080
/// This AP is active.
#define WFLAG_APDATA_ACTIVE        0x8000

/// List of which devices to add to the AP list while scanning for APs.
typedef enum {
    /// List unprotected and WEP APs to the list (excluding NDS hosts).
    WSCAN_LIST_AP_COMPATIBLE   = 0x1,
    /// List incompatible APs, such as the ones using WPA encryption
    WSCAN_LIST_AP_INCOMPATIBLE = 0x2,
    /// List all APs that aren't NDS hosts.
    WSCAN_LIST_AP_ALL          = 0x3,
    /// List NDS devices acting as multiplayer hosts. They are the ones that
    /// include the Nintendo tag in their beacon frames.
    WSCAN_LIST_NDS_HOSTS       = 0x4,
    /// Add all APs that are found to the list.
    WSCAN_LIST_ALL             = 0x7
} Wifi_APScanFlags;

/// Supported WEP modes.
///
/// - 64 bit (40 bit) WEP mode:   5 ASCII characters (or 10 hex numbers).
/// - 128 bit (104 bit) WEP mode: 13 ASCII characters (or 26 hex numbers).
/// - 152 bit (128 bit) WEP mode: 16 ASCII characters (or 32 hex numbers).
///
/// @warning
///     152 bit mode is untested/unsupported.
enum WEPMODES
{
    WEPMODE_NONE         = 0, ///< No WEP security is used.
    WEPMODE_64BIT        = 1, ///< 10 hexadecimal digits.
    WEPMODE_128BIT       = 2, ///< 26 hexadecimal digits.
    WEPMODE_152BIT       = 3, ///< 32 hexadecimal digits (Unsupported?)
    WEPMODE_64BIT_ASCII  = 5, ///< 5 ASCII characters.
    WEPMODE_128BIT_ASCII = 6, ///< 13 ASCII characters.
    WEPMODE_152BIT_ASCII = 7, ///< 16 ASCII characters (Unsupported?).
    WEPMODE_40BIT        = WEPMODE_64BIT, ///< Compatibility define
};

/// 1 Mbit/s transfer rate define in NDS mode.
#define WIFI_TRANSFER_RATE_1MBPS    0x0A
/// 2 Mbit/s transfer rate define in NDS mode.
#define WIFI_TRANSFER_RATE_2MBPS    0x14

/// Types of encryption used in an Access Point.
typedef enum
{
    AP_SECURITY_OPEN, ///< Open AP (no security).
    AP_SECURITY_WEP,  ///< This AP uses WEP encryption.
    AP_SECURITY_WPA,  ///< This AP uses WPA encryption. Currently unsupported.
    AP_SECURITY_WPA2, ///< This AP uses WPA2 encryption.
}
Wifi_ApSecurityType;

/// Returns a short string that describes the AP security type.
///
/// @param type
///     Security type.
///
/// @return
///     A string that describes the security type or "Unk" if it's unknown.
const char *Wifi_ApSecurityTypeString(Wifi_ApSecurityType type);

/// Encryption algorithm.
typedef enum
{
    AP_CRYPT_NONE = 1, ///< None. Used for open networks.
    AP_CRYPT_WEP = 2,  ///< Used for WEP networks.
    AP_CRYPT_TKIP = 3, ///< TKIP. Used for WPA, and WPA/WPA2 hotspots.
    AP_CRYPT_AES = 4,  ///< AES. Used for WPA2 netowrks.
}
Wifi_ApCryptType;

/// Returns a short string that describes the crypt type.
///
/// @param type
///     Crypt type.
///
/// @return
///     A string that describes the crypt type or "Unk" if it's unknown.
const char *Wifi_ApCryptString(Wifi_ApCryptType type);

/// Authentication type for WPA networks.
typedef enum {
    AP_AUTH_NONE = 0,  ///< None. Used for open and WEP networks.
    AP_AUTH_8021X = 1, ///< 802.1X
    AP_AUTH_PSK = 2,   ///< PSK (Pre-Shared Key/Personal)
}
Wifi_ApAuthType;

/// Returns a short string that describes the authentication type.
///
/// @param type
///     Authentication type.
///
/// @return
///     A string that describes the authentication type or "Unk" if it's unknown.
const char *Wifi_ApAuthTypeString(Wifi_ApAuthType type);

/// List of available WiFi statistics.
enum WIFI_STATS
{
    // Software statistics

    WSTAT_RXQUEUEDPACKETS,  ///< Number of packets queued into the RX FIFO
    WSTAT_TXQUEUEDPACKETS,  ///< Number of packets queued into the TX FIFO
    WSTAT_RXQUEUEDBYTES,    ///< Number of bytes queued into the RX FIFO
    WSTAT_TXQUEUEDBYTES,    ///< Number of bytes queued into the TX FIFO
    WSTAT_RXQUEUEDLOST,     ///< Number of packets lost due to space limitations in queuing
    WSTAT_TXQUEUEDREJECTED, ///< Number of packets rejected due to space limitations in queuing
    WSTAT_RXPACKETS,
    WSTAT_RXBYTES,
    WSTAT_RXDATABYTES,
    WSTAT_TXPACKETS,
    WSTAT_TXBYTES,
    WSTAT_TXDATABYTES,
    WSTAT_ARM7_UPDATES,
    WSTAT_DEBUG,

    // DS mode harware statistics (function mostly unknown)
    WSTAT_HW_1B0,
    WSTAT_HW_1B1,
    WSTAT_HW_1B2,
    WSTAT_HW_1B3,
    WSTAT_HW_1B4,
    WSTAT_HW_1B5,
    WSTAT_HW_1B6,
    WSTAT_HW_1B7,
    WSTAT_HW_1B8,
    WSTAT_HW_1B9,
    WSTAT_HW_1BA,
    WSTAT_HW_1BB,
    WSTAT_HW_1BC,
    WSTAT_HW_1BD,
    WSTAT_HW_1BE,
    WSTAT_HW_1BF,
    WSTAT_HW_1C0,
    WSTAT_HW_1C1,
    WSTAT_HW_1C4,
    WSTAT_HW_1C5,
    WSTAT_HW_1D0,
    WSTAT_HW_1D1,
    WSTAT_HW_1D2,
    WSTAT_HW_1D3,
    WSTAT_HW_1D4,
    WSTAT_HW_1D5,
    WSTAT_HW_1D6,
    WSTAT_HW_1D7,
    WSTAT_HW_1D8,
    WSTAT_HW_1D9,
    WSTAT_HW_1DA,
    WSTAT_HW_1DB,
    WSTAT_HW_1DC,
    WSTAT_HW_1DD,
    WSTAT_HW_1DE,
    WSTAT_HW_1DF,

    NUM_WIFI_STATS
};

/// Information send by Nintendo DS hosts in beacon frames.
///
/// This is only used if WFLAG_APDATA_NINTENDO_TAG is set in "flags" in the
/// Wifi_AccessPoint struct.
typedef struct {
    /// Maximum number of players allowed by the host (including the host)
    u8 players_max;
    /// Current number of players connected to the host (including the host)
    u8 players_current;
    /// Set to 1 if the host accepts new connections, 0 if not.
    u8 allows_connections;
    /// Game ID of the host. It doesn't need to match your own ID (you could
    /// have two different games that can talk to each other).
    u32 game_id;
    /// Length of the player name. Hosts can define the contents by using
    /// Wifi_MultiplayerHostName() so that clients can see it. By default,
    /// this is the length of the player name stored in the DS firmware.
    u8 name_len;
    /// Player name of the console that is hosting the game. Hosts can define
    /// the contents by using Wifi_MultiplayerHostName() so that clients can
    /// see it. By default, this is the player name stored in the DS firmware.
    u16 name[DSWIFI_BEACON_NAME_SIZE / sizeof(u16)];
} Wifi_NintendoVendorInfo;

/// Structure that defines information about an Access Point.
typedef struct WIFI_ACCESSPOINT
{
    /// Name (SSID) of the Access Point. The max supported length is 32
    /// characters, the last byte of the array must be zero.
    char ssid[33];
    // Number of valid bytes in the ssid field (0-32)
    u8 ssid_len;
    /// BSSID of the Access Point (usually the same value as the MAC address).
    u8 bssid[6];
    /// Valid channels are 1-14.
    u8 channel;
    /// Running average of the recent RSSI values for this AP, will be set to 0
    /// after not receiving beacons for a while. On DS this is a positive value
    /// between 0 and 255. On DSi it's a value in dBm. Usual values are -40 dBm
    /// (strong signal) to -90 dBm (weak signal).
    s16 rssi;
    /// Flags indicating various parameters for the AP
    u16 flags;
    /// Information send by Nintendo DS hosts in beacon frames, used if
    /// WFLAG_APDATA_NINTENDO_TAG is set in "flags".
    Wifi_NintendoVendorInfo nintendo;

    /// Type of security used in this Access Point.
    Wifi_ApSecurityType security_type;
    /// Group cipher
    Wifi_ApCryptType group_crypt_type;
    /// Pair cipher
    Wifi_ApCryptType pair_crypt_type;
    /// Authentication cipher
    Wifi_ApAuthType auth_type;

    // Private fields
    // --------------

    // Max rate is measured in steps of 1/2Mbit - 5.5Mbit will be represented
    // as 11, or 0x0B (NTR mode only).
    u16 maxrate;
    // Internal information about how recently a beacon has been received
    u32 timectr;
    // Internal data word used to lock the record to guarantee data coherence.
    // Its size must be a word because the swp instruction is used to access it,
    // which operates on words.
    u32 spinlock;
} Wifi_AccessPoint;

/// Reasons why the ARM7 may not replace the "extra data" of the beacon frame
/// before a transmission.
typedef enum {
    /// A fragment was written, nothing was skipped.
    DSWIFI_BEACON_ROTATE_OK = 0,
    /// The beacon has no Nintendo vendor information element to write into.
    DSWIFI_BEACON_ROTATE_NO_VENDOR_IE,
    /// The hardware isn't transmitting a beacon.
    DSWIFI_BEACON_ROTATE_BEACON_OFF,
    /// There is nothing to cycle through: no fragments, or only one.
    DSWIFI_BEACON_ROTATE_NO_FRAGMENTS,
    /// The fragments aren't the same size as the extra data of the beacon.
    DSWIFI_BEACON_ROTATE_SIZE_MISMATCH,
} Wifi_BeaconRotateSkip;

/// Reasons why a frame from a client can be discarded before it reaches the
/// packet handler of the application.
typedef enum {
    /// No handler has been registered.
    DSWIFI_MP_DROP_NO_HANDLER = 0,
    /// The frame is shorter than the multiplayer header.
    DSWIFI_MP_DROP_TOO_SHORT,
    /// The frame isn't a reply or a data frame from a client.
    DSWIFI_MP_DROP_FRAME_TYPE,
    /// A reply frame wasn't addressed to the multiplayer reply address.
    DSWIFI_MP_DROP_REPLY_MAC,
    /// A data frame wasn't addressed to this console.
    DSWIFI_MP_DROP_DEST_MAC,
    /// The association ID in the frame doesn't match the one of the client that
    /// owns the source MAC address.
    DSWIFI_MP_DROP_AID_MISMATCH,

    DSWIFI_MP_DROP_COUNT
} Wifi_MultiplayerDropReason;

/// Number of bytes kept from the last discarded frame.
#define DSWIFI_MP_DIAG_BYTES    16

/// Counters that describe what the multiplayer layer has done with the frames
/// received from clients.
///
/// Frames that don't look the way the library expects are discarded without
/// reaching the application, which makes a wrong wire format look the same as a
/// silent client. These counters, and the copy of the last discarded frame, tell
/// the two apart.
typedef struct {
    /// Number of frames discarded for each reason in Wifi_MultiplayerDropReason.
    u16 drops[DSWIFI_MP_DROP_COUNT];
    /// Number of frames passed on to the handler.
    u16 accepted;

    /// First bytes of the last discarded frame, starting at the IEEE header.
    u8 last_drop_frame[DSWIFI_MP_DIAG_BYTES];
    /// Number of bytes stored in "last_drop_frame".
    u8 last_drop_len;
    /// Reason the frame in "last_drop_frame" was discarded.
    u8 last_drop_reason;
} Wifi_MultiplayerRxDiag;

/// Counters of the multiplayer host, kept by the ARM7.
typedef struct {
    /// Distinct CMD frames handed to the hardware.
    u16 cmd_armed;
    /// Times a CMD frame was handed over again after a failure. The hardware
    /// sees "cmd_armed + cmd_retry" transmissions.
    u16 cmd_retry;
    /// Transfers the hardware reported as finished.
    u16 cmd_done;
    /// Transfers the hardware reported as failed. For a CMD frame this means the
    /// replies it polled for never arrived.
    u16 cmd_failed;
    /// Reply frames carrying data, counted before any are filtered.
    u16 reply_rx;
    /// Replies with nothing in them. A client always answers in its reply slot,
    /// and sends an empty reply while its software has nothing queued, so these
    /// mean the client is there and listening but hasn't accepted what it was
    /// sent.
    u16 reply_empty;
} Wifi_MultiplayerHostCounters;

/// Possible states of a client
typedef enum {
    /// This client is disconnected.
    WIFI_CLIENT_DISCONNECTED = 0,
    /// The client is authenticated but not associated.
    WIFI_CLIENT_AUTHENTICATED,
    /// The client is associated and ready to communicate data.
    WIFI_CLIENT_ASSOCIATED,
} Wifi_ConnectedClientState;

/// Structure that represents a DS connected to a host DS
typedef struct {
    /// MAC address of the client
    u16 macaddr[3];
    /// Association ID from 1 to 15 (WIFI_MAX_MULTIPLAYER_CLIENTS)
    u16 association_id;
    /// One of the values of Wifi_ConnectedClientState
    u8 state;
} Wifi_ConnectedClient;

#ifdef __cplusplus
}
#endif

#endif // DSWIFI_COMMON_H
