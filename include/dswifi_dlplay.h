// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

/// @file dswifi_dlplay.h
///
/// @brief DS Download Play host support.

#ifndef DSWIFI_DLPLAY_H__
#define DSWIFI_DLPLAY_H__

#ifdef __cplusplus
extern "C" {
#endif

/// @defgroup dswifi_dlplay DS Download Play host API.
///
/// A console acting as a DS Download Play host announces a program with beacon
/// frames, and sends it to the first console that picks it from its Download
/// Play menu.
///
/// Only the ARM9 and ARM7 binaries of the program are transferred, never the
/// filesystem, the banner or the overlays, so the program must be able to run
/// without them.
///
/// Consoles reject programs that aren't signed by Nintendo, so homebrew sent
/// this way only boots on consoles with FlashMe.
///
/// @{

#include <stddef.h>

#include <nds/ndstypes.h>

/// Size in bytes of the icon bitmap announced in beacon frames.
#define DSWIFI_DLPLAY_ICON_BITMAP_SIZE  0x200

/// Size in bytes of the icon palette announced in beacon frames.
#define DSWIFI_DLPLAY_ICON_PALETTE_SIZE 0x20

/// Maximum length in characters of the title announced in beacon frames.
#define DSWIFI_DLPLAY_TITLE_LEN         48

/// Maximum length in characters of the description announced in beacon frames.
#define DSWIFI_DLPLAY_DESC_LEN          96

/// Maximum length in characters of the host name announced in beacon frames.
#define DSWIFI_DLPLAY_HOST_NAME_LEN     10

/// Maximum size in bytes of a program that can be sent with DS Download Play.
///
/// This is the size of the main RAM of the console, which is where the ARM9 and
/// ARM7 binaries are loaded by the client.
///

/// program still has to fit in main RAM.
#define DSWIFI_DLPLAY_MAX_PROGRAM_SIZE  0x400000

/// Size in bytes of the parameter a host can pass to the program it sends.
#define DSWIFI_DLPLAY_USER_PARAM_SIZE   0x20

/// The most players a program can be announced as supporting, counting the host.
/// It is the limit of the field in the record clients read, and of the number of
/// consoles one host can have associated at a time.
#define DSWIFI_DLPLAY_MAX_PLAYERS       16

/// Information about the program shown in the Download Play menu of clients.
typedef struct {
    /// 32x32 pixel icon, 4 bits per pixel, in the format used by the banner of
    /// NDS ROMs. It must be DSWIFI_DLPLAY_ICON_BITMAP_SIZE bytes in size.
    const void *icon_bitmap;
    /// 16 color palette in BGR555 format. It must be
    /// DSWIFI_DLPLAY_ICON_PALETTE_SIZE bytes in size.
    const void *icon_palette;
    /// Name of the host console in UTF-16LE, up to DSWIFI_DLPLAY_HOST_NAME_LEN
    /// characters. It may be NULL to use the name stored in the DS firmware.
    const u16 *host_name;
    /// Length in characters of host_name.
    u8 host_name_len;
    /// Title of the program in UTF-16LE, up to DSWIFI_DLPLAY_TITLE_LEN
    /// characters, zero padded.
    const u16 *title;
    /// Length in characters of the title.
    u8 title_len;
    /// Description of the program in UTF-16LE, up to DSWIFI_DLPLAY_DESC_LEN
    /// characters, zero padded.
    const u16 *description;
    /// Length in characters of the description.
    u8 description_len;
    /// Bytes handed to the program being sent, or NULL for none.
    ///
    /// The client's loader leaves them at 0x027FFBE0, where the program can read
    /// them once it is running.
    ///
    /// Note that a program started by Download Play already knows where it came
    /// from without this: the loader leaves the host's MAC address, channel and
    /// game ID at 0x027FFC40. This is for anything else.
    ///
    /// It must be DSWIFI_DLPLAY_USER_PARAM_SIZE bytes if it isn't NULL.
    const void *user_param;
    /// How many players the program supports, counting the host, from 2 to
    /// DSWIFI_DLPLAY_MAX_PLAYERS. Zero uses the default of two.
    ///
    /// This describes the program on offer, and is what the Download Play menu
    /// of a client shows next to it. It does not change how many consoles this
    /// host will serve at once, which is one.
    u8 max_players;
} Wifi_DlPlayInfo;

/// State of a DS Download Play transfer.
typedef enum {
    /// Announcing the program, no client has connected yet.
    WIFI_DLPLAY_IDLE = 0,
    /// A client has connected, DSWifi is asking for its name.
    WIFI_DLPLAY_CONNECTING,
    /// The boot information has been sent, the client is checking the signature
    /// of the program.
    WIFI_DLPLAY_VERIFYING,
    /// The blocks of the program are being sent.
    WIFI_DLPLAY_SENDING,
    /// The transfer has finished and the client is booting the program.
    WIFI_DLPLAY_BOOTING,
    /// A console running the DS Download Station program is connected and
    /// asking for files rather than for a program to boot.
    WIFI_DLPLAY_STATION,
    /// The transfer has failed.
    WIFI_DLPLAY_ERROR,
} Wifi_DlPlayState;

/// Starts acting as a DS Download Play host for the provided NDS ROM.
///
/// This switches the console to multiplayer host mode and starts sending beacon
/// frames that clients will show in their Download Play menu.
///
/// The ROM buffer must remain valid until Wifi_DlPlayStop() is called, because
/// its blocks are read from it as they are needed instead of being copied.
///
/// Call Wifi_DlPlayUpdate() regularly after this function succeeds.
///
/// @param rom
///     Pointer to a whole NDS ROM image.
/// @param rom_size
///     Size in bytes of the ROM image.
/// @param info
///     Information shown in the Download Play menu of the client. If it is
///     NULL, it is taken from the banner of the ROM and from the name stored in
///     the DS firmware.
///
/// @return
///     0 on success, a negative value on error.
int Wifi_DlPlayStart(const void *rom, size_t rom_size, const Wifi_DlPlayInfo *info);

/// Stops acting as a DS Download Play host.
void Wifi_DlPlayStop(void);

/// Advances the DS Download Play state machine.
///
/// This must be called regularly (once per frame is enough) while acting as a
/// host. It sends the frame that the client is waiting for, so the transfer
/// stalls if it isn't called.
void Wifi_DlPlayUpdate(void);

/// Returns the current state of the transfer.
Wifi_DlPlayState Wifi_DlPlayGetState(void);

/// Returns the progress of the transfer.
///
/// @param current
///     Filled with the index of the block being sent. It may be NULL.
/// @param total
///     Filled with the total number of blocks to send. It may be NULL.
void Wifi_DlPlayGetProgress(int *current, int *total);

/// Returns the name of the connected client.
///
/// @return
///     A NUL terminated string with the name of the client converted to ASCII,
///     or NULL if no client has reported its name yet.
const char *Wifi_DlPlayGetClientName(void);

/// The most bytes one message to or from a delivered program can carry.
///
/// A message from the host rides in the frame it sends every cycle, alongside
/// the header the protocol needs; a message from a console rides in its reply
/// slot, which is much smaller.
#define DSWIFI_DLPLAY_APP_MAX_SIZE      64

/// Called when a program this host sent says something.
///
/// A console that has booted a program delivered by this host can connect again
/// and talk to the application rather than ask to be sent anything. It is
/// recognised by what it says, the same way a Download Station client is: a
/// message that isn't part of the transfer protocol at all.
///
/// This runs in an interrupt, like every packet handler, so it must not call
/// anything that takes a lock -- no printf(), no malloc(). Copy what is needed
/// and deal with it from the main loop.
///
/// @param aid
///     Association ID of the console that sent it, 1 to 15.
/// @param data
///     What it sent.
/// @param size
///     How many bytes, up to DSWIFI_DLPLAY_APP_MAX_SIZE.
/// @param arg
///     The value given to Wifi_DlPlaySetAppHandler().
typedef void (*Wifi_DlPlayAppHandlerFn)(int aid, const void *data, size_t size,
                                        void *arg);

/// The most bytes one message from a delivered program can carry.
///
/// A console answers in its reply slot, which is far smaller than the frame the
/// host sends, and every console in the room gets one of the same size.
#define DSWIFI_DLPLAY_REPLY_MAX_SIZE    7

/// Sends a message to the host that delivered this program.
///
/// For the client side: a console running a program it was sent by
/// Wifi_DlPlayStart() calls this, having joined the host as an ordinary
/// multiplayer client, and the host's Wifi_DlPlayAppHandlerFn is called with
/// what it sent. The host tells such a console apart from one asking to be sent
/// a program by this message alone.
///
/// @param data
///     What to send.
/// @param size
///     How many bytes, up to DSWIFI_DLPLAY_REPLY_MAX_SIZE.
///
/// @return
///     0 on success, a negative value on error.
int Wifi_DlPlayAppReply(const void *data, size_t size);

/// Reads a message sent by the host out of a received frame.
///
/// For the client side, from a packet handler registered with
/// Wifi_MultiplayerFromHostSetPacketHandler(). Most of what a host sends is part
/// of the Download Play protocol and is not for the application, so this reports
/// whether the frame was a message at all.
///
/// @param base
///     The "base" argument the packet handler was given.
/// @param len
///     The "len" argument the packet handler was given.
/// @param buffer
///     Where to put the message.
/// @param size
///     How many bytes the buffer holds.
///
/// @return
///     The number of bytes stored, or -1 if the frame wasn't a message from the
///     application on the host.
int Wifi_DlPlayAppReadMessage(int base, int len, void *buffer, size_t size);

/// Sets the function called when a delivered program says something.
///
/// @param fn
///     The function to call, or NULL to ignore them.
/// @param arg
///     Passed back to it unchanged.
void Wifi_DlPlaySetAppHandler(Wifi_DlPlayAppHandlerFn fn, void *arg);

/// Sends a message to every console running a program this host delivered.
///
/// It goes out on the next frame that a transfer doesn't need, so a console
/// still being sent a program is never held up by one that already has it. Only
/// one message is held at a time; sending another before the first has gone out
/// is refused rather than replacing it.
///
/// @param data
///     What to send.
/// @param size
///     How many bytes, up to DSWIFI_DLPLAY_APP_MAX_SIZE.
///
/// @return
///     0 if the message was accepted, -1 if it was too big or one is already
///     waiting to go out.
int Wifi_DlPlayAppSend(const void *data, size_t size);

/// Who decides when a client starts the program it has downloaded.
typedef enum {
    /// Each console is told to start as soon as it has the whole program. This
    /// is the default, and what a host serving one console at a time wants.
    WIFI_DLPLAY_BOOT_AUTOMATIC = 0,
    /// Consoles wait with the program in memory until the host asks. A game that
    /// wants everyone to start together holds them here and then calls
    /// Wifi_DlPlayBootAll().
    WIFI_DLPLAY_BOOT_MANUAL,
} Wifi_DlPlayBootMode;

/// Chooses who decides when a client starts the program.
///
/// Call this *after* Wifi_DlPlayStart(), not before: starting a session resets
/// the mode to WIFI_DLPLAY_BOOT_AUTOMATIC along with the rest of its state, so
/// a mode chosen beforehand is silently discarded and every console boots the
/// moment it has the program.
///
/// @param mode
///     WIFI_DLPLAY_BOOT_AUTOMATIC or WIFI_DLPLAY_BOOT_MANUAL.
void Wifi_DlPlaySetBootMode(Wifi_DlPlayBootMode mode);

/// Tells one client to start the program it has downloaded.
///
/// Only a console that has the whole program can be started; asking earlier
/// would tell it to run something it hasn't finished receiving. It is also
/// refused for a console running the Download Station program, which is served
/// by a different layer and starts what it downloads by itself.
///
/// @param aid
///     Association ID of the client, 1 to 15.
///
/// @return
///     True if the client was told to start.
bool Wifi_DlPlayBootClient(int aid);

/// Tells every client that has the whole program to start it.
///
/// @return
///     True if at least one client was told to start.
bool Wifi_DlPlayBootAll(void);

/// Opens or closes the room to consoles that aren't in it yet.
///
/// A closed room disappears from the Download Play menu of every console
/// scanning for one, and refuses an association from any that tries anyway.
/// The consoles already being served are unaffected.
///
/// This is what a retail host does when a game is about to start: bit 0 of the
/// beacon's attribute byte says whether entries are being accepted, and closing
/// the room clears it. The host also closes the room by itself when it is full,
/// and opens it again when somebody leaves; an explicit lock outranks that and
/// stays until it is lifted.
///
/// @param locked
///     True to close the room, false to open it.
void Wifi_DlPlayLockRoom(bool locked);

/// Whether new consoles are being turned away, for any reason.
///
/// @return
///     True if the room is locked or full.
bool Wifi_DlPlayRoomLocked(void);

/// The consoles being served, one bit per association ID from 1 to 15.
///
/// @return
///     A bitmap of the clients the host is talking to. Bit 0 is never set.
u16 Wifi_DlPlayGetClientMask(void);

/// The name of one client's owner, as ASCII.
///
/// @param aid
///     Association ID of the client, 1 to 15.
///
/// @return
///     Its name, or NULL if that client hasn't said who it is.
const char *Wifi_DlPlayGetClientNameByAID(int aid);

/// How far through the exchange one client is.
///
/// @param aid
///     Association ID of the client, 1 to 15.
///
/// @return
///     Its state, or WIFI_DLPLAY_IDLE for an association ID that isn't serving
///     anyone.
Wifi_DlPlayState Wifi_DlPlayGetClientState(int aid);

/// How much of the program one client holds.
///
/// Unlike Wifi_DlPlayGetProgress(), which reports the block the host is sending,
/// this is what the client itself says it has received. Clients take the same
/// stream and keep whichever blocks they are missing, so they finish at
/// different times.
///
/// @param aid
///     Association ID of the client, 1 to 15.
/// @param current
///     Where to store the number of blocks it holds. May be NULL.
/// @param total
///     Where to store the number of blocks in the program. May be NULL.
void Wifi_DlPlayGetClientProgress(int aid, int *current, int *total);

/// The request a client sends to ask for the program.
///
/// It arrives in five pieces spread over five frames, so it is only meaningful
/// once "valid" is true. The name in it is the one the client's owner set in the
/// console settings; the rest describes what the client thinks it is asking for.
typedef struct {
    /// True once every piece of the request has arrived.
    bool valid;
    /// Game ID the client read from the beacon frames of this host.
    u32 game_id;
    /// Version of the download protocol the client speaks. Consoles send 1.
    u16 version;
    /// Which of the programs offered by the host is wanted. This host only
    /// offers one, but the number has to be sent back with every block or the
    /// client discards it.
    u8 file_id;
    /// Colour the client's owner picked in the console settings, 0 to 15.
    u8 favorite_color;
    /// Player number the client believes it has. Hosts assign this themselves.
    u8 player_no;
    /// Length in characters of the name.
    u8 name_len;
    /// Name of the client's owner, in UTF-16LE.
    u16 name[DSWIFI_DLPLAY_HOST_NAME_LEN];
    /// How many blocks of the program the client says it holds. It reports this
    /// with every block it asks for, so it is a progress report rather than a
    /// count the host keeps.
    u16 blocks_received;
    /// True when the client is a console already running the Download Station
    /// program rather than one asking to be sent a program.
    ///
    /// Such a client reports its name as ten spaces, which is the only thing
    /// that distinguishes it. It doesn't want the blocks of a program: it wants
    /// a menu and then a file, over the station's own protocol.
    bool is_station;
} Wifi_DlPlayClientRequest;

/// Returns the request sent by the connected client.
///
/// @return
///     A pointer to the request. It is never NULL, but its "valid" field is
///     false until the client has sent every piece of it.
const Wifi_DlPlayClientRequest *Wifi_DlPlayGetClientRequest(void);

/// The request sent by one client.
///
/// @param aid
///     Association ID of the client, 1 to 15.
///
/// @return
///     Its request. The "valid" field is false until every piece has arrived,
///     and for an association ID that isn't serving anyone.
const Wifi_DlPlayClientRequest *Wifi_DlPlayGetClientRequestByAID(int aid);

/// Checks whether a NDS ROM is signed.
///
/// Consoles refuse to boot programs received with DS Download Play that aren't
/// signed, unless they have FlashMe installed.
///
/// @param rom
///     Pointer to a whole NDS ROM image.
/// @param rom_size
///     Size in bytes of the ROM image.
///
/// @return
///     True if the ROM has a signature.
bool Wifi_DlPlayRomIsSigned(const void *rom, size_t rom_size);

/// Kinds of message a client can send, in the order a transfer goes through
/// them.
typedef enum {
    /// The client has nothing to say. This is what it sends whenever it is
    /// waiting, so it is the normal state of the exchange rather than a fault.
    WIFI_DLPLAY_REPLY_DUMMY = 0,
    /// One piece of the request asking for the program.
    WIFI_DLPLAY_REPLY_FILEREQ,
    /// The client has taken the boot information.
    WIFI_DLPLAY_REPLY_ACCEPT_FILEINFO,
    /// The client is asking for a block of the program.
    WIFI_DLPLAY_REPLY_CONTINUE,
    /// The client has every block and wants the transfer to stop.
    WIFI_DLPLAY_REPLY_STOPREQ,
    /// The client has accepted the request to start the program.
    WIFI_DLPLAY_REPLY_BOOTREQ_ACCEPTED,

    WIFI_DLPLAY_REPLY_KIND_COUNT
} Wifi_DlPlayReplyKind;

/// Reasons why a frame from a client can be discarded before it is acted on.
typedef enum {
    /// The frame wasn't a multiplayer reply frame.
    WIFI_DLPLAY_DROP_NOT_REPLY = 0,
    /// A frame arrived while no transfer was running.
    WIFI_DLPLAY_DROP_INACTIVE,
    /// The frame came from a client other than the one being served.
    WIFI_DLPLAY_DROP_BAD_AID,
    /// The frame was empty or longer than the protocol allows.
    WIFI_DLPLAY_DROP_BAD_LEN,
    /// The opcode isn't one of the ones used by the protocol.
    WIFI_DLPLAY_DROP_UNKNOWN_OPCODE,

    WIFI_DLPLAY_DROP_COUNT
} Wifi_DlPlayDropReason;

/// Number of reply frames kept by the capture buffer.
#define DSWIFI_DLPLAY_CAPTURE_FRAMES    4

/// Number of bytes kept from each captured reply frame.
#define DSWIFI_DLPLAY_CAPTURE_BYTES     16

/// Why the host stopped waiting for a client.
typedef enum {
    /// It stopped answering altogether. A console that is present and merely has
    /// nothing to say still replies every frame, so this one has gone.
    WIFI_DLPLAY_GAVE_UP_SILENT = 0,
    /// It kept answering but stopped getting anywhere: no further block asked
    /// for, or a finished download it never asked to start.
    WIFI_DLPLAY_GAVE_UP_STALLED,

    WIFI_DLPLAY_GAVE_UP_COUNT,
} Wifi_DlPlayGaveUpReason;

/// Length of the name a Download Station client asks for. It is a fixed field,
/// not a string: it is neither terminated nor trimmed.
#define DSWIFI_DLPLAY_STATION_NAME_LEN  8

/// The most content bytes one station frame can carry. A chunk smaller than
/// this is the last of a transfer.
#define DSWIFI_DLPLAY_STATION_CHUNK_MAX 126

/// Counters and captured frames that describe what the host has seen.
///
/// Frames from clients are discarded silently when they don't match what the
/// protocol expects, which makes a host that isn't working look identical to one
/// that no client is talking to. These counters tell the two apart, and the
/// capture buffer shows what the client actually sent.
typedef struct {
    /// Number of frames discarded for each reason in Wifi_DlPlayDropReason.
    u16 drops[WIFI_DLPLAY_DROP_COUNT];
    /// Number of reply frames that were acted on.
    u16 replies_handled;
    /// Number of replies of each kind in Wifi_DlPlayReplyKind. A client that is
    /// listening but stuck sends nothing but dummies, which reads the same way
    /// as one that isn't there unless the kinds are counted separately.
    u16 replies[WIFI_DLPLAY_REPLY_KIND_COUNT];
    /// Pieces of the client's request that have arrived, one bit each.
    u8 request_pieces;
    /// Last block the client asked for.
    u16 block_requested;
    /// Number of blocks the client says it has received so far.
    u16 blocks_received;
    /// Number of frames that couldn't be queued for transmission.
    u16 cmd_tx_failures;

    /// The first bytes of the most recently received reply frames. Entry
    /// "capture_next - 1" (modulo the number of entries) is the newest one.
    u8 capture[DSWIFI_DLPLAY_CAPTURE_FRAMES][DSWIFI_DLPLAY_CAPTURE_BYTES];
    /// Number of bytes stored in each entry of the capture buffer.
    u8 capture_len[DSWIFI_DLPLAY_CAPTURE_FRAMES];
    /// Entry of the capture buffer that the next frame will be stored in.
    u8 capture_next;
    /// Total number of frames that have gone through the capture buffer.
    u16 capture_total;

    /// The start of the last frame sent to the client, as handed to the
    /// multiplayer layer. The two bytes the layer puts in front of it aren't
    /// here. Useful for checking what actually goes on the air against what the
    /// protocol says it should be.
    u8 last_cmd[DSWIFI_DLPLAY_CAPTURE_BYTES];
    /// Number of bytes stored in "last_cmd".
    u8 last_cmd_stored;
    /// Full length of the last frame sent, which may be longer than what fits.
    u16 last_cmd_len;

    /// Frames received on a WM port other than the one the transfer of a
    /// program uses. A console running the Download Station program asks for
    /// files on its own ports, so anything here means one is talking to us.
    u16 station_frames;
    /// Port the last of those arrived on, 13 to 15 for a station client.
    u8 station_port;
    /// Start of the last one, which for a request is the name of the file the
    /// client is asking for.
    u8 station_last[DSWIFI_DLPLAY_CAPTURE_BYTES];
    /// Number of bytes stored in "station_last".
    u8 station_last_len;
    /// The name the client last asked for, as it arrived. Not terminated.
    u8 station_name[DSWIFI_DLPLAY_STATION_NAME_LEN];
    /// The stream it asked for it on, which is what decides the port.
    u16 station_stream;
    /// Association ID of the console the rest of these describe. Several can be
    /// taking content at once, and these report whichever acted most recently.
    u8 station_aid;
    /// The port chosen from that stream, or zero if the request was refused.
    u8 station_port_used;
    /// Requests naming a stream this host doesn't serve.
    u8 station_bad_stream;
    /// Requests for a name the application didn't recognise.
    u8 station_not_found;
    /// Size the application gave for what is being sent now.
    u32 station_size;
    /// Chunks of it sent so far.
    u16 station_chunks;
    /// Reads the application couldn't answer, each of which ended a transfer.
    u8 station_read_failures;
    /// Index of the last chunk queued, and how many bytes of content it carried.
    /// A short one is the last of a transfer.
    u16 station_last_index;
    /// Bytes in that chunk.
    u8 station_last_chunk_len;
    /// Whether that chunk was the end of the content. A short chunk is not the
    /// test: a full one is 30 bytes on one port and 126 on the other, so the
    /// size alone says nothing without knowing which.
    u8 station_last_is_final;
    /// Set once the marker saying there is nothing more has gone out.
    u8 station_end_sent;
    /// Clients given up on, by reason. A room that fills with consoles nobody is
    /// holding any more shows up here first.
    u8 gave_up[WIFI_DLPLAY_GAVE_UP_COUNT];
    /// Association ID of the last one.
    u8 gave_up_last_aid;
} Wifi_DlPlayDiag;

/// Returns the diagnostic counters of the host.
///
/// The returned structure is owned by the library and is updated as frames
/// arrive, including from interrupt handlers, so read it from the main loop and
/// don't expect a set of values that are all from the same instant.
///
/// @return
///     A pointer to the counters. It is never NULL.
const Wifi_DlPlayDiag *Wifi_DlPlayGetDiag(void);

/// @}

#ifdef __cplusplus
}
#endif

/// Says whether a name a Download Station client asked for is offered, and how
/// big it is.
///
/// A console that has booted a Download Station program connects again and asks
/// for files by name on the host's own ports. The names belong to the content,
/// not to this library: the menu a station program displays is a list of
/// entries and each one names the file to fetch, so only the application that
/// provided that menu knows what a name means. A retail kiosk resolves them by
/// opening "/ds_demo/<name>" inside its own ROM.
///
/// The name arrives exactly as it was sent, eight bytes, and is not a C string:
/// it isn't terminated and hasn't been trimmed. Compare all eight.
///
/// @param name
///     The name the client asked for, not terminated.
/// @param name_len
///     Its length, always DSWIFI_DLPLAY_STATION_NAME_LEN.
/// @param size
///     Where to store the size of the content in bytes.
/// @param handle
///     Where to store a value identifying what was opened. It is quoted back by
///     every read, and means whatever the application wants it to. Several
///     consoles can be taking different content at once, so their reads
///     interleave and "the one opened last" is not enough to say which file a
///     read is for.
/// @param arg
///     The value given to Wifi_DlPlayStationSetContentSource().
///
/// @return
///     True if the name is offered and its size and handle have been stored.
typedef bool (*Wifi_DlPlayStationOpenFn)(const char *name, size_t name_len,
                                         size_t *size, u32 *handle, void *arg);

/// Reads part of the content most recently accepted by the open function.
///
/// This is called once per frame for the thirty or so bytes that go into it,
/// rather than the whole file being asked for at once, because the content does
/// not have to fit in memory: a kiosk ROM is 16 MB and a demo inside one reaches
/// 2.5 MB, against the 4 MB a DS has. An application reading from a card should
/// keep a window of a few KB and refill it when a request falls outside, since a
/// card read blocks for long enough to disturb the wireless frame timing.
///
/// Returning false ends the transfer. It is better than serving the wrong bytes,
/// which the client has no way of noticing.
///
/// @param handle
///     The value the open function stored for this content.
/// @param offset
///     Where to read from, in bytes from the start of the content.
/// @param buffer
///     Where to put them.
/// @param size
///     How many to read.
/// @param arg
///     The value given to Wifi_DlPlayStationSetContentSource().
///
/// @return
///     True if the whole range was read.
typedef bool (*Wifi_DlPlayStationReadFn)(u32 handle, size_t offset, void *buffer,
                                         size_t size, void *arg);

/// Sets where the content a Download Station client asks for comes from.
///
/// Call this before Wifi_DlPlayStart(). Without it a station client is
/// recognised but never answered, which is the same as not offering anything.
/// It has no effect on the transfer of the program itself, which is what gets a
/// station client running in the first place.
///
/// Both functions are needed; passing NULL for either offers nothing.
///
/// @param open
///     Asked whether a name is offered and how big it is.
/// @param read
///     Asked for the bytes to put in each frame.
/// @param arg
///     Passed back to both unchanged.
void Wifi_DlPlayStationSetContentSource(Wifi_DlPlayStationOpenFn open,
                                        Wifi_DlPlayStationReadFn read,
                                        void *arg);

#endif // DSWIFI_DLPLAY_H__
