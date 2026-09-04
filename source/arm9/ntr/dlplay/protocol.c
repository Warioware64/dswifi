// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#include <string.h>
#include <time.h>

#include <nds.h>

#include <dswifi9.h>
#include <dswifi_dlplay.h>

#include "arm9/ntr/dlplay/dlplay.h"
#include "common/ieee_defs.h"

// Addresses used by the client to load the program. They are the same ones used
// by official software.
#define DLPLAY_HEADER_DEST          0x027FFE00
#define DLPLAY_ARM7_STAGING_DEST    0x022C0000

// A client that says nothing must not be able to stall the transfer. A capture
// of a real console hosting DS Download Play shows it asking a handful of times
// and then carrying on regardless, so each step of the exchange gives the client
// this many frames to answer before the host moves on by itself. These are a
// safety net: the normal path is driven by what the client replies.
#define DLPLAY_REQUEST_FRAMES       90
#define DLPLAY_FILE_INFO_FRAMES     90
#define DLPLAY_START_SEND_FRAMES    60

// Frames to keep telling a client to start the program after it has said it
// will, and the limit for a client that never says anything. A console needs a
// second request before it acts on the first (mb_child.c), so this cannot be
// less than two, and it must not be so long that the client has already gone by
// the time the host says the exchange is over.
#define DLPLAY_BOOT_EXTRA_FRAMES    3
#define DLPLAY_BOOT_FRAMES          60

// How long to keep saying the exchange is over. The client is tearing its
// wireless down through all of it, so this only has to outlast that.
#define DLPLAY_SESSION_END_FRAMES   20

// Limits for a client that stops getting anywhere.
//
// The bounds above move a client on to the next step; these give up on it. That
// matters now that a room holds fifteen: a console that goes away without saying
// so keeps its place, and enough of them leave a host that refuses everybody.
//
// Two different faults, so two counters. A console that is present but has
// nothing to say still answers every frame, so a client that has gone quiet has
// gone; and a client can equally answer every frame while never asking for the
// next block, which the first counter would never catch.
//
// The numbers are generous. Losing a client that was only slow costs it the
// whole download, so these are for consoles that are really not coming back.
#define DLPLAY_SILENT_FRAMES        600  // 10 seconds without a word
#define DLPLAY_COMPLETE_FRAMES      180  // 3 seconds sitting on a finished
                                         // download without asking to start
#define DLPLAY_STATION_IDLE_FRAMES  600  // 10 seconds without a chunk going out
                                         // or a file being asked for. It is not
                                         // 10 seconds per transfer: a 103 KB
                                         // file is 819 chunks, and at the one a
                                         // frame a retail station sends that is
                                         // fourteen seconds, so this counts time
                                         // without progress rather than time
                                         // spent.
#define DLPLAY_APP_IDLE_FRAMES      600  // 10 seconds without a word from a
                                         // program this host already sent

// State of the exchange with the connected client. These are Nintendo's
// MB_COMM_PSTATE_* values (mb.h) for the steps this host goes through, and each
// one decides which message is sent to the client on the next frame.
//
// The states this host doesn't have are the ones that only make sense with more
// than one client, or that need the program on top to make a decision: the host
// serves the first client that connects and refuses the rest at association
// time, so there is nobody to kick and nobody to tell the game is full.
typedef enum {
    // No client is connected.
    DLPLAY_PSTATE_NONE = 0,
    // A client has associated. Tell it that it may ask for the program, and
    // wait for its request.
    DLPLAY_PSTATE_CONNECTED,
    // Its request is in. Offer it the boot information.
    DLPLAY_PSTATE_REQ_ACCEPTED,
    // It has taken the boot information. One more frame and the blocks start.
    DLPLAY_PSTATE_WAIT_TO_SEND,
    // Sending the program.
    DLPLAY_PSTATE_SEND_PROCEED,
    // It has every block.
    DLPLAY_PSTATE_SEND_COMPLETE,
    // Telling it to start the program. It leaves once it has.
    DLPLAY_PSTATE_BOOT_REQUEST,
    // Told to start, and now being let go of: the poll stops and the client is
    // told the exchange is over, which is what a retail station does.
    DLPLAY_PSTATE_SESSION_END,

    // A console running the Download Station program has connected. It doesn't
    // want the blocks of a program: it names the file it wants on port 13, and
    // is answered with a length and then the file itself on ports 14 and 15.
    DLPLAY_PSTATE_STATION,

    // A console running a program this host sent it earlier, which has come back
    // to talk to the application rather than to be sent anything.
    DLPLAY_PSTATE_APP,
} Wifi_DlPlayPState;

static Wifi_DlPlayState dlplay_state = WIFI_DLPLAY_IDLE;
static Wifi_DlPlayRom dlplay_rom;
static bool dlplay_active = false;

// The beacon can only be created once the ARM7 has switched to host mode, which
// takes a few frames, so it isn't done by Wifi_DlPlayStart().
static bool dlplay_beacon_started = false;

// The Download Station content protocol, which starts once a client that is
// already running a station program asks for something on port 13.
//
// The client drives this: it names what it wants and says which stream it wants
// the answer on, is told how big it is, and is then sent it in chunks. The host
// answers straight away rather than waiting to be told the client is ready --
// the kiosk sends the length once and starts sending two frames later.
typedef enum {
    // Nothing has been asked for.
    DLPLAY_SSTATE_IDLE = 0,
    // A request has been answered with the length; a couple of frames pass
    // before the content starts, as they do for the kiosk.
    DLPLAY_SSTATE_SEND_DELAY,
    // Sending the content, chunk by chunk.
    DLPLAY_SSTATE_DATA,
    // Everything has been sent. The client asks for the next thing when it is
    // ready, which is how it asks for a program after taking the menu.
    DLPLAY_SSTATE_DONE,
} Wifi_DlPlaySState;

// Everything about one console's content transfer.
//
// One of these per client rather than one for the host. Station clients ask for
// different files at different times, and every one of these was a single value
// until consoles started arriving in pairs: a second request wiped the first
// mid-transfer, and both consoles were sent both files.
typedef struct {
    Wifi_DlPlaySState state;

    // A request that has arrived but hasn't been acted on yet.
    //
    // Requests are picked up in the packet handler, which runs in an interrupt,
    // and the application answers them by reading a card. So the handler only
    // writes down what was asked for and the main loop does the asking, the same
    // way the blocks of a program are queued from Wifi_DlPlayUpdate().
    bool pending;
    char pending_name[DLPLAY_STATION_NAME_SIZE];
    u16 pending_stream;

    // What is being sent to this console, and where. The port comes from the
    // client, so the size of a chunk is taken from the port rather than from
    // what is in it.
    u32 handle;
    size_t size;
    u8 port;
    u16 stream;
    size_t chunk;

    // The chunk to send next, and whether the end marker has gone out.
    u16 index;
    bool ended;

    // Frames spent in the current state.
    int wait;

    // Packets sent to this console on each of the station ports. Per console,
    // not per host: each one is being sent its own stream and counts what it
    // receives, so a shared counter would look to both of them like frames going
    // missing.
    u16 seq[DLPLAY_PORT_STATION_FILE + 1];
} Wifi_DlPlayStationClient;

// Everything the host knows about one connected console.
//
// Clients are served together rather than one after another, so each keeps its
// own place in the exchange: one can still be sending its request while another
// is taking blocks and a third has been told to start the program. That is how
// Nintendo's host works too -- the same fields, held in arrays indexed by the
// association ID (MB_CommPWork, mb_private.h).
//
// What is deliberately *not* here is which blocks a client has received. That
// bitmap lives on the client, which asks for the lowest one it is missing, so a
// host serving fifteen consoles keeps fifteen small records rather than fifteen
// downloads.
typedef struct {
    // Where this client is in the exchange, one of Wifi_DlPlayPState.
    u8 stage;

    // Frames since this client last got anywhere: a new stage, or another block
    // asked for. Not simply frames in the stage, because a download is meant to
    // take a long time and a bound on that would fire on a healthy one.
    int wait_count;

    // Frames since this client said anything at all. A console that is present
    // and merely has nothing to say still answers every frame, so this only
    // grows for one that has stopped -- which the association alone doesn't
    // reveal, because a console that is switched off never gets to say so.
    int idle_count;

    // Its request for the program, reassembled from the five pieces it arrives
    // in, and the mask of the pieces seen so far.
    //
    // The mask is deliberately not cleared once the request is complete. A
    // client keeps sending pieces until it is answered, and Nintendo's host
    // keeps the assembled request so it can be re-read while it is still asking.
    u8 req_buffer[DLPLAY_REQ_BUFFER_SIZE];
    u8 req_mask;
    Wifi_DlPlayClientRequest request;
    char name_ascii[DSWIFI_DLPLAY_HOST_NAME_LEN + 1];

    // The lowest block this client says it is still missing, and whether it has
    // said anything since the last frame was built.
    int next_block;
    bool block_requested;

    // Zero until the client says it will start the program, then counted up so
    // the host knows when to stop asking and let go of it.
    int boot_accepted;

    // Set when the application has asked for this client to be started. Only
    // read in manual mode, where a client with the whole program waits here
    // until somebody says so.
    bool boot_requested;

    // Its content transfer, while it is a Download Station client.
    Wifi_DlPlayStationClient station;
} Wifi_DlPlayClient;

// Indexed by association ID, which runs from 1 to WIFI_MAX_MULTIPLAYER_CLIENTS.
// Entry zero stands for the host and is never used, so that an AID can be used
// as an index without adjusting it everywhere.
static Wifi_DlPlayClient dlplay_clients[WIFI_MAX_MULTIPLAYER_CLIENTS + 1];

// The block being transmitted, and the one the clients between them are waiting
// for.
//
// There is one stream, not one per client: every console taking the program
// hears the same frame and keeps whichever blocks it is missing, so the host
// sends each block once however many are listening. What the clients ask for is
// folded together with max(), and the cursor either walks forward or drops back
// to it -- the whole retransmission scheme, and the same rule as
// MBi_calc_sendblock() in mb_parent.c. Running ahead is what a client expects:
// GBATEK notes that a host may send blocks out of order and that "the downloader
// should maintain a flag array to know which packets were already received".
static int dlplay_block = 0;
static int dlplay_next_block = 0;
static bool dlplay_block_requested = false;

// The size announced for the frames sent to clients, and the size of the piece
// of the program each one carries.
//
// Chosen when the host starts, because it is announced in beacon frames before
// anyone connects. Bigger frames halve the number needed for a transfer, but
// every client costs air time too, and the two trade against each other -- see
// Wifi_DlPlayPickFrameSize().
static u16 dlplay_cmd_data_size = DLPLAY_CMD_DATA_SIZE;
static u16 dlplay_block_size = DLPLAY_BLOCK_SIZE;

// How many consoles this host will serve at once, not counting itself.
static u8 dlplay_max_clients = 1;

// Bytes handed to the program being sent, and whether the application gave any.
static u8 dlplay_user_param[DSWIFI_DLPLAY_USER_PARAM_SIZE];
static bool dlplay_user_param_set = false;

// Whether the room is closed to new consoles, and whether the host closed it by
// itself because it is full.
static bool dlplay_room_locked = false;
static bool dlplay_room_full = false;

// Who decides when a client starts the program it has downloaded.
static Wifi_DlPlayBootMode dlplay_boot_mode = WIFI_DLPLAY_BOOT_AUTOMATIC;

// Messages to and from the programs this host has already sent.
static Wifi_DlPlayAppHandlerFn dlplay_app_handler = NULL;
static void *dlplay_app_arg = NULL;

// What to put in the next frame for them, if anything.
static u8 dlplay_app_out[DSWIFI_DLPLAY_APP_MAX_SIZE];
static size_t dlplay_app_out_size = 0;
static bool dlplay_app_out_pending = false;


// How the application is asked for the content a client wants.
//
// It answers in two parts because the content doesn't have to fit in memory: a
// kiosk ROM is 16 MB and the demos inside one reach 2.5 MB, against the 4 MB a
// DS has. So the library asks how big something is, and then for the handful of
// bytes it is about to put in each frame.
static Wifi_DlPlayStationOpenFn dlplay_station_open = NULL;
static Wifi_DlPlayStationReadFn dlplay_station_read = NULL;
static void *dlplay_station_arg = NULL;

// What was served last, so that the next frame goes to something else. Several
// consoles can be taking a program or content at once and only one frame goes
// out at a time, so they take it in turns.
static int dlplay_bulk_turn = 0;

// Counter that official hosts store in the last byte of every frame. Clients
// use it to tell a new frame from a repeated one.
static u8 dlplay_packnum = 0;

// True for an association ID this host is willing to talk to.
static bool Wifi_DlPlayAidValid(int aid)
{
    return (aid >= 1) && (aid <= WIFI_MAX_MULTIPLAYER_CLIENTS);
}

// The record for one client, or NULL for an AID that isn't one.
static Wifi_DlPlayClient *Wifi_DlPlayClientByAid(int aid)
{
    if (!Wifi_DlPlayAidValid(aid))
        return NULL;

    return &dlplay_clients[aid];
}

// The lowest connected client, which is what the calls that don't name one mean.
// A host serving nobody has none, and they answer as they did before a client
// connected.
static Wifi_DlPlayClient *Wifi_DlPlayFirstClient(void)
{
    for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
    {
        if (dlplay_clients[aid].stage != DLPLAY_PSTATE_NONE)
            return &dlplay_clients[aid];
    }

    return NULL;
}

// Buffer the frames sent to the client are built in. The biggest message is a
// block of the program, which fills the size announced in beacon frames, and the
// two byte header and the footer go around it.
static u8 dlplay_cmd[DLPLAY_CMD_FRAME_SIZE];

// The block command is the one that has to fill the announced size exactly: the
// command, two unused bytes, the block number, the block itself and one more
// unused byte.
static_assert((5 + DLPLAY_BLOCK_SIZE + DLPLAY_BLOCK_OVERHEAD - 5) == DLPLAY_CMD_DATA_SIZE);
static_assert((2 + DLPLAY_CMD_DATA_SIZE + 2) <= (int)sizeof(dlplay_cmd));
static_assert((2 + DLPLAY_CMD_SIZE_BOOT_INFO + 2) <= (int)sizeof(dlplay_cmd));

// Number of bytes of dlplay_cmd that the message being built actually uses.
static size_t dlplay_cmd_size;

// Counters and captured frames returned by Wifi_DlPlayGetDiag(). Most of this is
// updated from the packet handler, which runs in an interrupt.
static Wifi_DlPlayDiag dlplay_diag;

// Writing 32 bit values one byte at a time keeps them in little endian order
// regardless of the alignment of the destination.
static void Wifi_DlPlayWrite32(u8 *dest, u32 value)
{
    dest[0] = (value >> 0) & 0xFF;
    dest[1] = (value >> 8) & 0xFF;
    dest[2] = (value >> 16) & 0xFF;
    dest[3] = (value >> 24) & 0xFF;
}

// Copies a 32 bit field of the NDS ROM header, which is already stored in the
// order expected by the client.
static void Wifi_DlPlayCopyHeaderField(u8 *dest, const u8 *header, int offset)
{
    memcpy(dest, header + offset, 4);
}

// Starts a new frame. Counting from the start of the body of the multiplayer
// frame, which is where the payload built here lands:
//
//     04h  1  Size in halfwords of everything from 06h on, footer excluded
//     05h  1  Port, plus bit 4 when the list of clients follows the payload
//     06h  ..  The payload itself
//     ..h  2  Footer: the list of clients again
//
// The low nibble of the second byte is the number of the WM port the payload
// belongs to. The transfer of the program rides on port 1, which is why every
// frame this host used to send began 11h; a retail DS Download Station also
// talks on ports 13, 14 and 15, and its frames begin 1Dh, 1Eh and 1Fh.
//
// The Wii program this was derived from writes the size and the port the other
// way round, but it runs behind IOS, which builds the frame itself. Nintendo's
// firmware reads the length from the low byte of the header
// (WMSP_ParsePortPacket), GBATEK documents the same order, and a capture of a
// real station confirms it.
static u8 *Wifi_DlPlayCmdStart(u8 header, size_t payload_size, u16 dest)
{
    memset(dlplay_cmd, 0, sizeof(dlplay_cmd));

    // Every message has to fit, header and footer included. Getting this wrong
    // used to write past the end of the buffer and corrupt whatever came after
    // it, which is a great deal harder to work out than a dropped frame.
    size_t needed = 2 + payload_size
                  + ((header & DLPLAY_FLAGS_HAS_FOOTER) ? 2 : 0);

    if (needed > sizeof(dlplay_cmd))
    {
        dlplay_cmd_size = 0;
        return NULL;
    }

    dlplay_cmd[0] = payload_size / 2;
    dlplay_cmd[1] = header;

    // Footer: the list of clients the message is for. It is only there when bit 4
    // of the flags says so, and it isn't counted by the size field above. Bit 0
    // stands for the host in the rest of the library, and doesn't belong on the
    // air.
    //
    // With more than one console being served this decides who acts on the
    // message, and getting it wrong is not harmless: a client checks the file id
    // on a block of a program and ignores one that isn't for it, but it checks
    // nothing at all on the boot information, on a kick, or on a request to
    // start the program. Send one of those to everybody and the wrong console
    // reboots.
    dlplay_cmd_size = 2 + payload_size;

    if (header & DLPLAY_FLAGS_HAS_FOOTER)
    {
        u16 clients = dest & Wifi_MultiplayerGetClientMask() & ~1;

        dlplay_cmd[2 + payload_size] = clients & 0xFF;
        dlplay_cmd[3 + payload_size] = (clients >> 8) & 0xFF;

        dlplay_cmd_size += 2;
    }

    return dlplay_cmd + 2;
}

// Returns false when the transmit ring is full, so a caller queueing several
// frames at once can stop instead of counting failures.
static bool Wifi_DlPlayCmdSend(void)
{
    // Only send as much as the message uses. Frames used to go out at their full
    // size with the unused tail zeroed, which puts a run of zeros after the
    // message where the client looks for the next entry of the list. It also
    // spent two milliseconds of air time on a message ten bytes long, leaving
    // the client that much less time to prepare its answer.
    //
    // A frame that can't be queued means the client gets nothing to reply to, so
    // the exchange stalls with no other symptom. Count them.
    size_t kept = dlplay_cmd_size;
    if (kept > DSWIFI_DLPLAY_CAPTURE_BYTES)
        kept = DSWIFI_DLPLAY_CAPTURE_BYTES;

    memcpy(dlplay_diag.last_cmd, dlplay_cmd, kept);
    dlplay_diag.last_cmd_stored = kept;
    dlplay_diag.last_cmd_len = dlplay_cmd_size;

    if (Wifi_MultiplayerHostCmdTxFrame(dlplay_cmd, dlplay_cmd_size) < 0)
    {
        dlplay_diag.cmd_tx_failures++;
        return false;
    }

    return true;
}

static void Wifi_DlPlaySendDummy(void);

// Picks up what a station client says, on any port but the multiboot one.
//
// The client names what it wants on port 13, and acknowledges the length it is
// told by answering on the port the content will arrive on. Those two are the
// only things it says that the host has to act on; everything else it sends
// while the content is going out is a keep-alive.
static void Wifi_DlPlayHandleStationReply(Wifi_DlPlayClient *client, u8 port,
                                          const u8 *reply, size_t reply_size)
{
    if (client->stage != DLPLAY_PSTATE_STATION)
        return;

    // Only the request port carries anything the host has to act on. Whatever
    // arrives on a content port while a transfer is running is the client
    // keeping the exchange alive, and a retail station doesn't wait for it.
    if (port != DLPLAY_PORT_STATION_NAME)
        return;

    if (reply_size < DLPLAY_STATION_REQUEST_SIZE)
        return;

    // Eight bytes naming what the client wants, then the stream it wants the
    // answer on. The stream is what decides the port -- the kiosk answers the
    // two requests it gets on ports 14 and 15 because they carry 1 and 2, not
    // because of what they are called.
    const char *name = (const char *)reply;
    u16 stream = reply[DLPLAY_STATION_NAME_SIZE]
               | (reply[DLPLAY_STATION_NAME_SIZE + 1] << 8);

    dlplay_diag.station_stream = stream;
    memcpy(dlplay_diag.station_name, name, DLPLAY_STATION_NAME_SIZE);

    if ((stream < DLPLAY_STATION_STREAM_MIN) || (stream > DLPLAY_STATION_STREAM_MAX))
    {
        // A stream this host has never seen would be served on a port that
        // isn't one. Record it and ignore the request: the diagnostics are the
        // only way anyone would find out.
        dlplay_diag.station_bad_stream++;
        return;
    }

    u8 content_port = DLPLAY_STATION_STREAM_PORT(stream);

    // Asking again for what is already going out happens when a frame is lost,
    // and starting over would throw away everything sent so far.
    if ((client->station.state != DLPLAY_SSTATE_IDLE)
        && (client->station.state != DLPLAY_SSTATE_DONE)
        && (client->station.port == content_port))
        return;

    // Write down what was asked for. Answering it means asking the application,
    // which reads a card, and this runs in an interrupt.
    memcpy(client->station.pending_name, name, DLPLAY_STATION_NAME_SIZE);
    client->station.pending_stream = stream;
    client->station.pending = true;

    // Naming something is how a station client gets anywhere, so it restarts
    // this console's clock -- and only this one's. Resetting every station
    // client kept a stalled console alive on the strength of a healthy one
    // asking for something.
    client->wait_count = 0;
}

// Acts on a request the handler wrote down. Called from the main loop.
static void Wifi_DlPlayStationAcceptRequest(Wifi_DlPlayClient *client, int aid)
{
    int oldIME = enterCriticalSection();

    char name[DLPLAY_STATION_NAME_SIZE];
    u16 stream = client->station.pending_stream;

    memcpy(name, client->station.pending_name, sizeof(name));
    client->station.pending = false;

    leaveCriticalSection(oldIME);

    u8 content_port = DLPLAY_STATION_STREAM_PORT(stream);

    // The application decides what a name means. It knows what is in the menu it
    // gave the client; the library only carries the answer back.
    //
    // It hands back a handle as well as a size, and every read quotes it. With
    // several consoles being served at once the reads interleave, so "the file
    // that was opened last" is not enough to say which one a read is for.
    size_t size = 0;
    u32 handle = 0;

    if ((dlplay_station_open == NULL) || (dlplay_station_read == NULL)
        || !dlplay_station_open(name, DLPLAY_STATION_NAME_SIZE, &size, &handle,
                                dlplay_station_arg)
        || (size == 0))
    {
        dlplay_diag.station_not_found++;
        return;
    }

    client->station.handle = handle;
    client->station.size = size;
    client->station.port = content_port;
    client->station.stream = stream;
    client->station.chunk = (content_port == DLPLAY_PORT_STATION_MENU)
                          ? DLPLAY_STATION_CHUNK_PORT14
                          : DLPLAY_STATION_CHUNK_PORT15;

    client->station.index = 0;
    client->station.ended = false;
    client->station.wait = 0;

    dlplay_diag.station_aid = aid;
    dlplay_diag.station_port_used = content_port;
    dlplay_diag.station_size = size;
    dlplay_diag.station_chunks = 0;
    dlplay_diag.station_last_index = 0;
    dlplay_diag.station_last_chunk_len = 0;
    dlplay_diag.station_end_sent = 0;

    // Answer straight away. The length goes out once, from the state below.
    client->station.state = DLPLAY_SSTATE_SEND_DELAY;
}

// Starts a packet on one of the Download Station ports.
//
// These carry one halfword the multiboot port doesn't: the index of the chunk
// the client should expect next, between the payload and the list of clients.
// The size byte counts the payload alone, so it can't be built with
// Wifi_DlPlayCmdStart() -- in the capture a full menu chunk announces 10h
// halfwords for 32 bytes of payload and is followed by two more bytes before
// the footer.

// Starts a packet on one of the Download Station ports.
//
// The two header bytes are one little endian halfword, as Nintendo's library
// builds it (wm.h): the port in bits 8 to 11, WM_HEADER_DEST_BITMAP in bit 12
// to say the list of clients follows, and the size of the payload in halfwords
// in the low byte. Which is the same halfword Wifi_DlPlayCmdStart() writes for
// the multiboot port, so only the two extra trailing bytes are new here.
static u8 *Wifi_DlPlayStationCmdStart(Wifi_DlPlayClient *client, int aid,
                                      u8 port, size_t payload_size)
{
    memset(dlplay_cmd, 0, sizeof(dlplay_cmd));

    if ((port > DLPLAY_PORT_STATION_FILE) || (payload_size & 1))
    {
        dlplay_cmd_size = 0;
        return NULL;
    }

    size_t needed = 2 + payload_size + 2 + 2;

    if (needed > sizeof(dlplay_cmd))
    {
        dlplay_cmd_size = 0;
        return NULL;
    }

    dlplay_cmd[0] = payload_size / 2;
    dlplay_cmd[1] = DLPLAY_FLAGS_PORT(port);

    // This console's counter for this port, which counts from one.
    u16 seq = ++client->station.seq[port];

    dlplay_cmd[2 + payload_size + 0] = seq & 0xFF;
    dlplay_cmd[2 + payload_size + 1] = (seq >> 8) & 0xFF;

    // Addressed to the one console it is for. This used to go to every client in
    // the room, which is harmless with one console and wrong with two: a station
    // client acts on any content packet it receives, so both were being sent
    // both files, on top of each other.
    u16 clients = (1 << aid) & Wifi_MultiplayerGetClientMask() & ~1;

    dlplay_cmd[4 + payload_size + 0] = clients & 0xFF;
    dlplay_cmd[4 + payload_size + 1] = (clients >> 8) & 0xFF;

    dlplay_cmd_size = 2 + payload_size + 2 + 2;

    return dlplay_cmd + 2;
}

// Tells the client how big the thing it asked for is, on the port it asked on.
//
// Sent once. A retail station announces the length and starts sending two frames
// later without waiting to be told the client has it.
static void Wifi_DlPlayStationSendLength(Wifi_DlPlayClient *client, int aid)
{
    u8 *payload = Wifi_DlPlayStationCmdStart(client, aid,
                                             DLPLAY_PORT_STATION_NAME,
                                             DLPLAY_STATION_LEN_SIZE);
    if (payload == NULL)
        return;

    u32 size = client->station.size;

    payload[0] = size & 0xFF;
    payload[1] = (size >> 8) & 0xFF;
    payload[2] = (size >> 16) & 0xFF;
    payload[3] = (size >> 24) & 0xFF;

    Wifi_DlPlayCmdSend();
}

// Sends one chunk of the content, or the marker that says there are no more.
//
// Returns false when the frame couldn't be queued, so a caller sending several
// at once stops rather than losing them silently.
static bool Wifi_DlPlayStationSendChunk(Wifi_DlPlayClient *client, int aid)
{
    size_t offset = (size_t)client->station.index * client->station.chunk;

    if (offset >= client->station.size)
    {
        // Everything has gone out. The end marker takes the place of a chunk:
        // an index of FFFFh and no data, still followed by the index the client
        // would have been given next.
        u8 *payload = Wifi_DlPlayStationCmdStart(client, aid, client->station.port,
                                                 DLPLAY_STATION_INDEX_SIZE);
        if (payload == NULL)
            return false;

        payload[0] = DLPLAY_STATION_INDEX_END & 0xFF;
        payload[1] = (DLPLAY_STATION_INDEX_END >> 8) & 0xFF;

        if (!Wifi_DlPlayCmdSend())
            return false;

        client->station.ended = true;
        dlplay_diag.station_end_sent = 1;

        return true;
    }

    size_t remaining = client->station.size - offset;
    size_t size = (remaining < client->station.chunk) ? remaining
                                                      : client->station.chunk;

    // The size byte counts halfwords, so an odd tail has to be rounded up. The
    // spare byte is already zero.
    size_t payload_size = DLPLAY_STATION_INDEX_SIZE + ((size + 1) & ~(size_t)1);

    u8 *payload = Wifi_DlPlayStationCmdStart(client, aid, client->station.port,
                                             payload_size);
    if (payload == NULL)
        return false;

    payload[0] = client->station.index & 0xFF;
    payload[1] = (client->station.index >> 8) & 0xFF;

    // Read what goes in this frame, rather than copying it from a buffer the
    // application would have had to hold all of. The handle says which file:
    // several consoles can be taking different content at once, so the reads
    // interleave and "the one opened last" would be the wrong answer.
    if (!dlplay_station_read(client->station.handle, offset,
                             payload + DLPLAY_STATION_INDEX_SIZE, size,
                             dlplay_station_arg))
    {
        dlplay_diag.station_read_failures++;
        client->station.ended = true;
        return false;
    }

    if (!Wifi_DlPlayCmdSend())
        return false;

    dlplay_diag.station_aid = aid;
    dlplay_diag.station_last_index = client->station.index;
    dlplay_diag.station_last_chunk_len = size;
    dlplay_diag.station_last_is_final = ((offset + size) >= client->station.size);

    client->station.index++;
    dlplay_diag.station_chunks++;

    // Sending a chunk is this console getting somewhere, so it restarts the
    // clock that gives up on a stalled one.
    //
    // Without this the bound measured the time since the console last asked for
    // a file, and a console receiving one asks for nothing at all: a 103 KB file
    // is 819 chunks, which at a chunk a frame is fourteen seconds against a ten
    // second bound, so the host gave up on a transfer that was going perfectly.
    client->wait_count = 0;

    return true;
}

// Queues the next few chunks, for the same reason Wifi_DlPlaySendNextBlock()
// queues several blocks: one per video frame is far slower than the hardware
// manages.
// Serves one station client, and says whether it put anything on the air.
//
// Only one console is served per pass, because only one frame goes out per pass.
// Which one is decided by the caller, which takes them in turns.
static bool Wifi_DlPlayStationServe(Wifi_DlPlayClient *client, int aid)
{
    if (client->station.pending)
        Wifi_DlPlayStationAcceptRequest(client, aid);

    client->station.wait++;

    switch (client->station.state)
    {
        case DLPLAY_SSTATE_SEND_DELAY:
            if (client->station.wait == 1)
            {
                Wifi_DlPlayStationSendLength(client, aid);
                return true;
            }

            if (client->station.wait > DLPLAY_STATION_SEND_DELAY)
            {
                client->station.wait = 0;
                client->station.state = DLPLAY_SSTATE_DATA;
            }
            return false;

        case DLPLAY_SSTATE_DATA:
            if (client->station.ended)
            {
                // Everything is out. Stay here: the client asks for the next
                // thing when it is ready, which is how it asks for a program
                // after taking the menu.
                client->station.wait = 0;
                client->station.state = DLPLAY_SSTATE_DONE;
                return false;
            }

            for (int i = 0; i < DLPLAY_STATION_PER_UPDATE; i++)
            {
                if (client->station.ended)
                    break;

                if (!Wifi_DlPlayStationSendChunk(client, aid))
                    return false;
            }

            return true;

        case DLPLAY_SSTATE_IDLE:
        case DLPLAY_SSTATE_DONE:
            break;
    }

    return false;
}

// Tells the client that it may ask for the program. This is the first thing a
// host says to a client, and a client won't ask for anything until it arrives.
//
// The frame used to be described as a request for the client's name. The name
// does come back in the answer, but only because it is part of the request the
// client sends to say which program it wants.
static void Wifi_DlPlaySendStart(u16 dest)
{
    u8 *payload = Wifi_DlPlayCmdStart(DLPLAY_FLAGS_NORMAL, DLPLAY_CMD_SIZE_SHORT, dest);
    if (payload == NULL)
        return;

    payload[0] = DLPLAY_CMD_OP_SENDSTART;

    Wifi_DlPlayCmdSend();
}

// Frame that keeps the exchange going without asking the client for anything.
// A client is only given a slot to reply in when the host transmits, so a state
// with nothing to say still has to send this.
//
// It used to go out with the footerless flags, which GBATEK describes as "can be
// ignored". Every frame a retail DS Download Station sends carries the normal
// flags and the list of clients, including the ones whose command is 00, so this
// does too.
static void Wifi_DlPlaySendDummy(void)
{
    if (Wifi_DlPlayCmdStart(DLPLAY_FLAGS_NORMAL, DLPLAY_CMD_SIZE_SHORT, 0xFFFF) == NULL)
        return;

    Wifi_DlPlayCmdSend();
}

// Tells the client where to load the program and gives it the signature to
// check before it accepts it.
static void Wifi_DlPlaySendFileInfo(u16 dest)
{
    u8 *payload = Wifi_DlPlayCmdStart(DLPLAY_FLAGS_NORMAL, DLPLAY_CMD_SIZE_BOOT_INFO, dest);
    if (payload == NULL)
        return;

    const u8 *hdr = dlplay_rom.rom;

    payload[0x00] = DLPLAY_CMD_OP_DL_FILEINFO;

    Wifi_DlPlayCopyHeaderField(payload + 0x01, hdr, NDS_HDR_ARM9_ENTRY);
    Wifi_DlPlayCopyHeaderField(payload + 0x05, hdr, NDS_HDR_ARM7_ENTRY);

    Wifi_DlPlayWrite32(payload + 0x0D, DLPLAY_HEADER_DEST);
    Wifi_DlPlayWrite32(payload + 0x11, DLPLAY_HEADER_DEST);
    Wifi_DlPlayWrite32(payload + 0x15, NDS_HDR_SIZE);

    // The client copies the ARM9 binary to its final address itself, so both
    // addresses are the same one.
    Wifi_DlPlayCopyHeaderField(payload + 0x1D, hdr, NDS_HDR_ARM9_RAM_ADDRESS);
    Wifi_DlPlayCopyHeaderField(payload + 0x21, hdr, NDS_HDR_ARM9_RAM_ADDRESS);
    Wifi_DlPlayCopyHeaderField(payload + 0x25, hdr, NDS_HDR_ARM9_SIZE);

    // The ARM7 binary is received in main RAM and moved to its final address
    // right before the program is started.
    Wifi_DlPlayWrite32(payload + 0x2D, DLPLAY_ARM7_STAGING_DEST);
    Wifi_DlPlayCopyHeaderField(payload + 0x31, hdr, NDS_HDR_ARM7_RAM_ADDRESS);
    Wifi_DlPlayCopyHeaderField(payload + 0x35, hdr, NDS_HDR_ARM7_SIZE);

    Wifi_DlPlayWrite32(payload + 0x39, 1);

    memcpy(payload + 0x3D, dlplay_rom.rsa, NDS_RSA_SIZE);

    // Bytes for the program being sent, in the 32 that follow the signature.
    //
 
    if (dlplay_user_param_set)
    {
        memcpy(payload + 0xC5, dlplay_user_param,
               DSWIFI_DLPLAY_USER_PARAM_SIZE);
    }

    payload[0xE5] = dlplay_packnum++;

    Wifi_DlPlayCmdSend();
}

// Sends one block of the program. Block 0 is the header of the ROM, the rest
// are chunks of the ARM9 and ARM7 binaries. Returns false if it couldn't be
// queued.
static bool Wifi_DlPlaySendBlock(int block, u16 dest, u8 file_id)
{
    size_t data_size;

    const u8 *src = Wifi_DlPlayRomGetBlock(&dlplay_rom, block, &data_size);

    if (src == NULL)
    {
        // Nothing to send for this block. Keep the exchange going with a frame
        // that says so, because a client that is given nothing to answer has no
        // way of asking for anything else.
        Wifi_DlPlaySendDummy();
        return false;
    }

    // Opcode, the program number, the block number, the data and the counter.
    size_t payload_size = 5 + data_size + 1;

    u8 *payload = Wifi_DlPlayCmdStart(DLPLAY_FLAGS_NORMAL, payload_size, dest);
    if (payload == NULL)
        return false;

    payload[0] = DLPLAY_CMD_OP_DATA;

    // Which of the programs offered by the host this block belongs to. It has to
    // be the number the client asked for: a client compares it against its own
    // request and throws the block away if they differ (mb_child.c). This host
    // only offers one program, but the number still has to come back.
    payload[1] = file_id;
    payload[2] = 0;

    // Little endian, like the rest of the protocol. The Wii reference looks like
    // it stores this field in big endian order, but it runs on a big endian host
    // and passes the value through __builtin_bswap16() first, so what reaches the
    // air is little endian.
    payload[3] = block & 0xFF;
    payload[4] = (block >> 8) & 0xFF;

    memcpy(payload + 5, src, data_size);

    payload[5 + data_size] = dlplay_packnum++;

    return Wifi_DlPlayCmdSend();
}

// Tells the client that it can start the program.
static void Wifi_DlPlaySendBootReq(u16 dest)
{
    u8 *payload = Wifi_DlPlayCmdStart(DLPLAY_FLAGS_NORMAL, DLPLAY_CMD_SIZE_SHORT, dest);
    if (payload == NULL)
        return;

    payload[0] = DLPLAY_CMD_OP_BOOTREQ;

    Wifi_DlPlayCmdSend();
}

// Sends the application's message to the programs this host has already sent.
static void Wifi_DlPlaySendApp(u16 dest)
{
    u8 *payload = Wifi_DlPlayCmdStart(DLPLAY_FLAGS_NORMAL,
                                      1 + dlplay_app_out_size, dest);
    if (payload == NULL)
        return;

    payload[0] = DLPLAY_CMD_OP_APP;
    memcpy(payload + 1, dlplay_app_out, dlplay_app_out_size);

    if (Wifi_DlPlayCmdSend())
        dlplay_app_out_pending = false;
}

// Says that the exchange is over.
//
//
//     03 11 05 ...  bits=0002    boot now
//     00 00         bits=0000    this
//     03 11 01 ...  footer 0000  ready for the next client
//
// It matters because of what the client is doing at that moment. A console that
// is shutting its wireless down to start a program should not still be polled
// sixty times a second, and one that is told nothing has no way to know the
// exchange has ended.
static void Wifi_DlPlaySendSessionEnd(void)
{
    if (Wifi_DlPlayCmdStart(0, 0, 0) == NULL)
        return;

    Wifi_DlPlayCmdSend();
}

// True while a client is a console running the Download Station program, which
// wants files rather than the blocks of a program.
static bool Wifi_DlPlayClientIsStation(const Wifi_DlPlayClient *client)
{
    return client->request.valid && client->request.is_station;
}

// Moves the exchange to a new state. The state the program on top sees follows
// from it, so the two can't disagree, and the wait counter restarts because
// every state gives the client a bounded number of frames to answer in.
// What one stage looks like to the program on top.
static Wifi_DlPlayState Wifi_DlPlayStateOfStage(u8 stage)
{
    switch (stage)
    {
        case DLPLAY_PSTATE_CONNECTED:
            return WIFI_DLPLAY_CONNECTING;

        case DLPLAY_PSTATE_REQ_ACCEPTED:
        case DLPLAY_PSTATE_WAIT_TO_SEND:
            return WIFI_DLPLAY_VERIFYING;

        case DLPLAY_PSTATE_SEND_PROCEED:
            return WIFI_DLPLAY_SENDING;

        case DLPLAY_PSTATE_SEND_COMPLETE:
        case DLPLAY_PSTATE_BOOT_REQUEST:
        case DLPLAY_PSTATE_SESSION_END:
            return WIFI_DLPLAY_BOOTING;

        case DLPLAY_PSTATE_STATION:
            return WIFI_DLPLAY_STATION;

        default:
            return WIFI_DLPLAY_IDLE;
    }
}

// Sums the clients up into the one value the program on top reads.
//
// It reports the client that has got the furthest, so a host serving one console
// says exactly what it said before there could be more than one. Applications
// that care about each client ask for them by association ID.
static void Wifi_DlPlayUpdateSummaryState(void)
{
    Wifi_DlPlayState summary = WIFI_DLPLAY_IDLE;

    for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
    {
        Wifi_DlPlayState state = Wifi_DlPlayStateOfStage(dlplay_clients[aid].stage);

        if (state > summary)
            summary = state;
    }

    dlplay_state = summary;
}

// Moves one client to a new stage. Its wait counter restarts because every stage
// gives the client a bounded number of frames to answer in.
static void Wifi_DlPlaySetClientStage(Wifi_DlPlayClient *client, Wifi_DlPlayPState stage)
{
    client->stage = stage;
    client->wait_count = 0;

    Wifi_DlPlayUpdateSummaryState();
}

// Sends the next few blocks of the program.
//
// One frame per call would tie the transfer to the video frame, which is far
// slower than the hardware manages: a cycle at the announced size takes under
// three milliseconds, and the ARM7 empties the transmit ring from its own
// transmit-complete interrupt rather than waiting to be asked. Queueing several
// blocks at once lets it send them back to back.
static void Wifi_DlPlaySendNextBlock(u16 dest, u8 file_id)
{
    // Clients ask for blocks from an interrupt handler, so gather what they have
    // asked for and clear the flag in one go.
    //
    // One number stands for all of them, and it is the highest they have asked
    // for rather than the lowest. A client asks for the lowest block it is still
    // missing and takes anything else useful that goes past, so following the
    // one furthest along and letting the stream come round again serves everyone
    // -- while following the one furthest behind would hold the rest still.
    // MBi_CommParentRecvData() folds the requests together with max() the same
    // way.
    int oldIME = enterCriticalSection();

    bool requested = dlplay_block_requested;
    int next = 0;

    for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
    {
        const Wifi_DlPlayClient *client = &dlplay_clients[aid];

        if ((client->stage == DLPLAY_PSTATE_SEND_PROCEED)
            && (client->next_block > next))
            next = client->next_block;
    }

    dlplay_block_requested = false;

    leaveCriticalSection(oldIME);

    dlplay_next_block = next;

    // Go back to what the clients asked for when the stream has run further
    // ahead than they can ask it to come back from. Everything sent since then
    // was either received, in which case they say so and the cursor moves on
    // again, or lost, in which case it has to be sent again anyway.
    //
    // "What the clients asked for" is one number for all of them: they take the
    // same stream and each keeps the blocks it is missing, so the cursor follows
    // whichever of them is furthest along and the ones behind pick up the rest as
    // it comes round. That is MBi_calc_sendblock() in mb_parent.c.
    if (requested)
    {
        if ((dlplay_block < next) || (dlplay_block > (next + DLPLAY_SEND_THRESHOLD)))
            dlplay_block = next;
    }

    int sent = 0;

    for (int i = 0; i < DLPLAY_BLOCKS_PER_UPDATE; i++)
    {
        // Never get further ahead than a client can ask the host to come back
        // from. Past the end of the program there is nothing to send either.
        if (dlplay_block > (next + DLPLAY_SEND_THRESHOLD))
            break;

        // Past the end there is nothing to send. A console admitted part way
        // through doesn't need the stream restarted for it: once the ones ahead
        // of it finish they stop asking, the number above drops to what the
        // newcomer wants, and the cursor follows it back.
        if (dlplay_block >= dlplay_rom.total_blocks)
            break;

        // The transmit ring is full. Stop rather than dropping frames: the ARM7
        // is already behind, and what isn't sent now goes out on the next pass.
        if (!Wifi_DlPlaySendBlock(dlplay_block, dest, file_id))
            break;

        dlplay_block++;
        sent++;
    }

    // A pass can send nothing at all: the window may be full while the clients
    // say nothing, or every block may already be out. It still has to transmit
    // something, because a client is only given a slot to answer in when the
    // host sends, and one that gets no frame has no way to ask for anything.
    if (sent == 0)
        Wifi_DlPlaySendDummy();
}

// Sends the one message the state of the exchange calls for. Nintendo's host
// picks a message the same way and in the same order of priority, and sends a
// frame with nothing in it when no state has anything to say
// (MBi_CommParentSendData, mb_parent.c).
//
// Every state has to send something. A client only gets a slot to answer in when
// the host transmits, so a state that stays quiet is a state the client can
// never talk its way out of.
// Gives up on a client that is no longer getting anywhere.
//
// Dropping it is the point: an association is one of fifteen, and a console that
// has stopped keeps its place until somebody takes it away. The record is left
// for the update loop to clear, which is where every other departure is noticed.
static void Wifi_DlPlayGiveUpOnClient(int aid, Wifi_DlPlayGaveUpReason reason)
{
    if (dlplay_diag.gave_up[reason] < 0xFF)
        dlplay_diag.gave_up[reason]++;

    dlplay_diag.gave_up_last_aid = aid;

    Wifi_MultiplayerKickClientByAID(aid);
}

// Says on the air whether the room is taking anyone else, and stops the hardware
// accepting one if it isn't.
//
// Both halves matter. Bit 0 of the beacon's attribute byte is "accepting
// entries" (ieee_defs.h), and clearing it is what takes a room out of the
// Download Play menu of every console scanning for one -- but a console that
// already has the room listed will still try, so the association has to be
// refused as well.
static void Wifi_DlPlayApplyRoomState(void)
{
    bool open = !dlplay_room_locked && !dlplay_room_full;

    u8 attribute = DLPLAY_BEACON_TYPE_SENDING;

    if (!open)
        attribute &= ~DLPLAY_ATTR_ACCEPTING_ENTRIES;

    Wifi_BeaconPatchVendorByte(FIE_NINTENDO_OFS_BEACON_TYPE, attribute);
    Wifi_MultiplayerAllowNewClients(open);
}

static void Wifi_DlPlaySendForState(void)
{
    // Every client's own timer, and the sets of them wanting each kind of
    // message. One frame goes out per pass, so the clients are gathered into
    // groups first and one group is served.
    u16 bmp_boot = 0;
    u16 bmp_start = 0;
    u16 bmp_fileinfo = 0;
    u16 bmp_send = 0;
    u16 bmp_end = 0;
    u16 bmp_app = 0;

    u8 send_file_id = 0;
    u16 bmp_station = 0;

    for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
    {
        Wifi_DlPlayClient *client = &dlplay_clients[aid];

        if (client->stage == DLPLAY_PSTATE_NONE)
            continue;

        client->wait_count++;
        client->idle_count++;

        // A console that has stopped answering is gone whatever stage it was in.
        if (client->idle_count > DLPLAY_SILENT_FRAMES)
        {
            Wifi_DlPlayGiveUpOnClient(aid, WIFI_DLPLAY_GAVE_UP_SILENT);
            continue;
        }

        // And one that answers without getting anywhere it was supposed to.
        //
        // Deliberately not while it is being sent a program. Consoles share one
        // stream and it follows whichever of them is furthest ahead, so a console
        // that joined part way through receives nothing at all until the ones in
        // front of it finish -- that is the design, not a fault, and bounding it
        // gave up on consoles that were downloading perfectly. A console that has
        // really gone is caught by the silence above, which is the honest test.
        // Nor while the application is holding a finished console on purpose.
        // DLPLAY_PSTATE_SEND_COMPLETE is exactly where WIFI_DLPLAY_BOOT_MANUAL
        // parks a console that has the whole program, and nothing resets
        // wait_count once it is there, so bounding that stage gave the host
        // three seconds to call Wifi_DlPlayBootAll() and then destroyed the very
        // console it was waiting to start. The bound is what automatic mode
        // wants -- there, a console that keeps asking and is never answered
        // really has stalled -- so it stays for that mode only. Silence above
        // remains the honest test in both.
        bool holding_for_boot = (dlplay_boot_mode == WIFI_DLPLAY_BOOT_MANUAL)
                                && (client->stage == DLPLAY_PSTATE_SEND_COMPLETE);

        if ((!holding_for_boot
             && (client->stage == DLPLAY_PSTATE_SEND_COMPLETE)
             && (client->wait_count > DLPLAY_COMPLETE_FRAMES))
            || ((client->stage == DLPLAY_PSTATE_STATION)
                && (client->wait_count > DLPLAY_STATION_IDLE_FRAMES))
            || ((client->stage == DLPLAY_PSTATE_APP)
                && (client->wait_count > DLPLAY_APP_IDLE_FRAMES)))
        {
            Wifi_DlPlayGiveUpOnClient(aid, WIFI_DLPLAY_GAVE_UP_STALLED);
            continue;
        }

        switch (client->stage)
        {
            case DLPLAY_PSTATE_BOOT_REQUEST:
                // Keep asking until the client says it will start, and for a few
                // frames after so that it gets the second request it acts on.
                // Then stop: this used to be sent every frame until the client
                // disappeared -- thousands of them -- with the client still being
                // polled the whole time it was trying to shut its wireless down.
                //
                // The count is also bounded for a client that never answers, so
                // a host is never left stuck on one.
                if (((client->boot_accepted > 0)
                     && (client->boot_accepted > DLPLAY_BOOT_EXTRA_FRAMES))
                    || (client->wait_count > DLPLAY_BOOT_FRAMES))
                {
                    Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_SESSION_END);
                    bmp_end |= 1 << aid;
                    break;
                }

                if (client->boot_accepted > 0)
                    client->boot_accepted++;

                bmp_boot |= 1 << aid;
                break;

            case DLPLAY_PSTATE_SESSION_END:
                // Bounded, because the client is on its way out and there is
                // nothing to wait for. Saying it once and then leaving the
                // client alone is what a retail station does; repeating it until
                // the client vanishes is what this used to do.
                if (client->wait_count <= DLPLAY_SESSION_END_FRAMES)
                    bmp_end |= 1 << aid;
                break;

            case DLPLAY_PSTATE_CONNECTED:
                // Keep telling the client it may ask for the program. If it
                // never does, offer it the boot information anyway: a capture of
                // a real host shows it moving on rather than waiting for ever.
                if (client->wait_count > DLPLAY_REQUEST_FRAMES)
                {
                    Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_REQ_ACCEPTED);
                    bmp_fileinfo |= 1 << aid;
                    break;
                }

                bmp_start |= 1 << aid;
                break;

            case DLPLAY_PSTATE_REQ_ACCEPTED:
                if (client->wait_count > DLPLAY_FILE_INFO_FRAMES)
                {
                    Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_WAIT_TO_SEND);
                    break;
                }

                bmp_fileinfo |= 1 << aid;
                break;

            case DLPLAY_PSTATE_WAIT_TO_SEND:
                // The client repeats that it has the boot information, and that
                // second answer is what starts the transfer. Start anyway if it
                // doesn't come.
                if (client->wait_count > DLPLAY_START_SEND_FRAMES)
                {
                    Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_SEND_PROCEED);
                    bmp_send |= 1 << aid;
                    send_file_id = client->request.file_id;
                }
                break;

            case DLPLAY_PSTATE_SEND_PROCEED:
                bmp_send |= 1 << aid;
                send_file_id = client->request.file_id;
                break;

            case DLPLAY_PSTATE_STATION:
                bmp_station |= 1 << aid;
                break;

            case DLPLAY_PSTATE_APP:
                bmp_app |= 1 << aid;
                break;

            default:
                break;
        }
    }

    // One message per pass, in the order host picks it
    // telling a client to start the
    // program comes before everything, then admitting new ones, then the boot
    // information, and the blocks only when nobody is waiting on any of that.
    //
    // Control traffic starving the stream is deliberate. It is why a console
    // that turns up part way through doesn't have to wait for a download to
    // finish before it is spoken to.
    if (bmp_end)
    {
        // Stop polling these and tell them the exchange is over, the way a
        // retail station does. Dropping a client is what clears it from the poll
        // bitmap the ARM7 puts in every frame, so it stops being asked to answer
        // while it shuts its wireless down.
        //
        // Nothing moves a client on from here: losing it is what does that,
        // further down, and that already counts as a finished transfer rather
        // than a lost one.
        for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
        {
            if (bmp_end & (1 << aid))
                Wifi_MultiplayerKickClientByAID(aid);
        }

        Wifi_DlPlaySendSessionEnd();
        return;
    }

    if (bmp_boot)
    {
        Wifi_DlPlaySendBootReq(bmp_boot);
        return;
    }

    if (bmp_start)
    {
        Wifi_DlPlaySendStart(bmp_start);
        return;
    }

    if (bmp_fileinfo)
    {
        Wifi_DlPlaySendFileInfo(bmp_fileinfo);
        return;
    }

    // Everything below here is bulk: the program being sent block by block, the
    // content each station client is taking chunk by chunk, and any message for
    // the programs already delivered. They take the frame in turns.
    //
    // Priority would be wrong for these, and was: sending the program came
    // first, so a download held the air until it finished and a station client
    // asking for a file in the meantime was never answered at all -- its request
    // sat unread while the console gave up and left. Control messages keep their
    // priority above this because they are one small frame each and something is
    // waiting on them; nothing here is ever finished quickly enough to be worth
    // waiting for.
    //
    // The rotation runs over 0 for the block stream, 1 to 15 for the station
    // client with that association ID, and 16 for a message to delivered
    // programs, starting after whoever went last.
    #define DLPLAY_BULK_SLOTS   (WIFI_MAX_MULTIPLAYER_CLIENTS + 2)
    #define DLPLAY_BULK_BLOCKS  0
    #define DLPLAY_BULK_APP     (WIFI_MAX_MULTIPLAYER_CLIENTS + 1)

    for (int i = 0; i < DLPLAY_BULK_SLOTS; i++)
    {
        int slot = (dlplay_bulk_turn + 1 + i) % DLPLAY_BULK_SLOTS;

        if (slot == DLPLAY_BULK_BLOCKS)
        {
            if (bmp_send == 0)
                continue;

            dlplay_bulk_turn = slot;
            Wifi_DlPlaySendNextBlock(bmp_send, send_file_id);
            return;
        }

        if (slot == DLPLAY_BULK_APP)
        {
            if ((bmp_app == 0) || !dlplay_app_out_pending)
                continue;

            dlplay_bulk_turn = slot;
            Wifi_DlPlaySendApp(bmp_app);
            return;
        }

        if ((bmp_station & (1 << slot)) == 0)
            continue;

        // A station client waiting out one of its own pauses says so by sending
        // nothing, and the turn passes on without being spent on it.
        if (Wifi_DlPlayStationServe(&dlplay_clients[slot], slot))
        {
            dlplay_bulk_turn = slot;
            return;
        }
    }

    Wifi_DlPlaySendDummy();
}

// Forgets one client. Everything shared -- the block stream and the content
// protocol -- is left alone, because other clients may still be using it.
static void Wifi_DlPlayResetOneClient(Wifi_DlPlayClient *client)
{
    memset(client, 0, sizeof(*client));
    client->stage = DLPLAY_PSTATE_NONE;
}

// Forgets everybody, and everything they shared.
static void Wifi_DlPlayResetClient(void)
{
    for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
        Wifi_DlPlayResetOneClient(&dlplay_clients[aid]);

    dlplay_block = 0;
    dlplay_next_block = 0;
    dlplay_block_requested = false;

    dlplay_diag.request_pieces = 0;
    dlplay_diag.block_requested = 0;
    dlplay_diag.blocks_received = 0;

    // Each console's content transfer went with it, in Wifi_DlPlayResetOneClient().
    // What stays is the content source: it is set once, before the host is
    // started, and outlives any number of clients.
    dlplay_bulk_turn = 0;
}

// Turns the assembled request into something the rest of the library can use.
static void Wifi_DlPlayDecodeRequest(Wifi_DlPlayClient *client)
{
    const u8 *req = client->req_buffer;

    client->request.game_id = req[DLPLAY_REQ_OFS_GAME_ID + 0]
                           | (req[DLPLAY_REQ_OFS_GAME_ID + 1] << 8)
                           | (req[DLPLAY_REQ_OFS_GAME_ID + 2] << 16)
                           | ((u32)req[DLPLAY_REQ_OFS_GAME_ID + 3] << 24);

    client->request.favorite_color = req[DLPLAY_REQ_OFS_COLOR] & 0xF;
    client->request.player_no = req[DLPLAY_REQ_OFS_COLOR] >> 4;

    u8 name_len = req[DLPLAY_REQ_OFS_NAME_LEN];
    if (name_len > DSWIFI_DLPLAY_HOST_NAME_LEN)
        name_len = DSWIFI_DLPLAY_HOST_NAME_LEN;

    client->request.name_len = name_len;

    for (int i = 0; i < DSWIFI_DLPLAY_HOST_NAME_LEN; i++)
    {
        size_t offset = DLPLAY_REQ_OFS_NAME + (i * 2);

        client->request.name[i] = req[offset] | (req[offset + 1] << 8);
    }

    client->request.version = req[DLPLAY_REQ_OFS_VERSION]
                           | (req[DLPLAY_REQ_OFS_VERSION + 1] << 8);
    client->request.file_id = req[DLPLAY_REQ_OFS_FILE_ID];

    for (int i = 0; i < name_len; i++)
    {
        u16 c = client->request.name[i];
        client->name_ascii[i] = (c < 0x80) ? (char)c : '?';
    }
    client->name_ascii[name_len] = '\0';

    client->request.valid = true;
}

// Stores one piece of the request the client sends to ask for the program.
//
// The request is 29 bytes and a reply frame holds eight, so it arrives in five
// pieces of six bytes, each one preceded by the block type and the number of the
// piece. Clients start at piece 1 and cycle 1, 2, 3, 4, 0, so the pieces can
// turn up in any order and the host has to wait for all of them.
//
// This used to be read as a name sent in five frames, which happened to recover
// the right characters because the name really does start at byte 6 of the
// request, and threw away everything else in it, including the number of the
// program being asked for.
static void Wifi_DlPlayHandleFileRequest(Wifi_DlPlayClient *client,
                                         const u8 *reply, size_t size)
{
    if (size < (2 + DLPLAY_REQ_PIECE_SIZE))
        return;

    u8 piece = reply[1];

    // Nintendo's own host tests this bound with the wrong comparison and lets a
    // sixth piece write past the end of its buffer. Don't copy that.
    if (piece >= DLPLAY_REQ_PIECE_COUNT)
        return;

    memcpy(client->req_buffer + ((size_t)piece * DLPLAY_REQ_PIECE_SIZE),
           reply + 2, DLPLAY_REQ_PIECE_SIZE);

    client->req_mask |= 1 << piece;
    dlplay_diag.request_pieces = client->req_mask;

    if (client->req_mask != DLPLAY_REQ_ALL_PIECES)
        return;

    if (!client->request.valid)
        Wifi_DlPlayDecodeRequest(client);

    // A console already running the Download Station client reports its name as
    // ten spaces. That is the only thing telling it apart from a console asking
    // to be sent a program, and it is what a real host uses: the Wii sender
    // compares the name against ten spaces at exactly this point.
    client->request.is_station =
        (client->request.name_len == DSWIFI_DLPLAY_HOST_NAME_LEN);

    for (int i = 0; client->request.is_station
                    && (i < DSWIFI_DLPLAY_HOST_NAME_LEN); i++)
    {
        if (client->request.name[i] != DLPLAY_STATION_NAME_CHAR)
            client->request.is_station = false;
    }

    // The client is asking for the program. Nothing here refuses it: the host
    // offers exactly one program, so there is nothing else to offer and no
    // reason to kick. The number of the program it asked for is sent back with
    // every block whatever it is, which is what the client checks, so a client
    // that asks for something unexpected is answered rather than dropped.
    if (client->stage == DLPLAY_PSTATE_CONNECTED)
        Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_REQ_ACCEPTED);
}


// Locates the start of the payload of a frame received from the client.
//
// The multiplayer layer has already skipped the two bytes that follow the IEEE
// header, which a client running Nintendo's software uses for the size and the
// flags of its reply, so what is left is the eight byte message itself:
//
//     00h  1  Block type (00h nothing to say, 07h asking for the program,
//                         08h boot information taken, 09h wants a block,
//                         0Ah has them all, 0Bh starting the program)
//     01h  7  Arguments, laid out differently for each type
//
// There used to be a guess here that skipped another two bytes when they looked
// like a header. They aren't one, and skipping them lost the first two arguments
// of every reply.
static const u8 *Wifi_DlPlayReplyPayload(const u8 *reply, size_t size, size_t *out_size)
{
    *out_size = size;
    return reply;
}

static void Wifi_DlPlayCapture(int base, int len)
{
    if (len <= 0)
        return;

    size_t size = (size_t)len;
    if (size > DSWIFI_DLPLAY_CAPTURE_BYTES)
        size = DSWIFI_DLPLAY_CAPTURE_BYTES;

    int slot = dlplay_diag.capture_next;

    Wifi_RxRawReadPacket(base, size, dlplay_diag.capture[slot]);
    dlplay_diag.capture_len[slot] = size;

    dlplay_diag.capture_next = (slot + 1) % DSWIFI_DLPLAY_CAPTURE_FRAMES;
    dlplay_diag.capture_total++;
}

static void Wifi_DlPlayFromClientHandler(Wifi_MPPacketType type, int aid,
                                         int base, int len)
{
    if (type != WIFI_MPTYPE_REPLY)
    {
        dlplay_diag.drops[WIFI_DLPLAY_DROP_NOT_REPLY]++;
        return;
    }

    // Capture before any check. A frame that is the wrong length or comes from
    // an unexpected client is exactly the kind that needs looking at, so it has
    // to be recorded before it is thrown away.
    Wifi_DlPlayCapture(base, len);

    if (!dlplay_active)
    {
        dlplay_diag.drops[WIFI_DLPLAY_DROP_INACTIVE]++;
        return;
    }

    // Which console this came from. An association ID outside the range is not
    // one this host handed out, so there is nothing to say to it.
    Wifi_DlPlayClient *client = Wifi_DlPlayClientByAid(aid);

    if (client == NULL)
    {
        dlplay_diag.drops[WIFI_DLPLAY_DROP_BAD_AID]++;
        return;
    }

    // It answered, so it is still there. Anything counts, including a reply that
    // says nothing: that is what a console sends between every step, and a host
    // that only counted useful ones would give up on a client waiting patiently.
    client->idle_count = 0;

    u8 frame[16];

    if ((len < 0) || (len > (int)sizeof(frame)))
    {
        dlplay_diag.drops[WIFI_DLPLAY_DROP_BAD_LEN]++;
        return;
    }

    // A client with nothing to say can send a message with no body at all. That
    // is a dummy like any other, not something going wrong, and it used to be
    // counted as a frame too short to read.
    if (len == 0)
    {
        dlplay_diag.replies_handled++;
        dlplay_diag.replies[WIFI_DLPLAY_REPLY_DUMMY]++;
        return;
    }

    Wifi_RxRawReadPacket(base, len, frame);

    size_t reply_size;
    const u8 *reply = Wifi_DlPlayReplyPayload(frame, len, &reply_size);

    dlplay_diag.replies_handled++;

    // Which WM port the client is talking on. It is the byte the multiplayer
    // layer skipped over, right in front of the payload, and it is the only
    // thing that says whether this is part of the transfer of a program or of
    // the station protocol that follows one.
    u8 port_byte;

    Wifi_RxRawReadPacket(base - 1, 1, &port_byte);

    u8 port = port_byte & 0xF;

    // Port zero counts as the multiboot port.
    //
    // A console running Nintendo's software puts a WM port header in the two
    // bytes before the payload, which is where this comes from. A client running
    // this library puts its association ID and a pad byte there instead, so the
    // byte read as a port is zero and has nothing to do with WM ports at all.
    // Treating that as "not multiboot" sent every reply from one of our own
    // clients to the station handler, which ignores it.
    if ((port != DLPLAY_PORT_BOOT) && (port != 0))
    {
        // Record what was asked for and on which port. This is what says whether
        // a station client is talking to us at all, and it stays useful once the
        // content is being served because it names the last thing it said.
        dlplay_diag.station_port = port;
        dlplay_diag.station_frames++;

        size_t kept = reply_size;
        if (kept > DSWIFI_DLPLAY_CAPTURE_BYTES)
            kept = DSWIFI_DLPLAY_CAPTURE_BYTES;

        memcpy(dlplay_diag.station_last, reply, kept);
        dlplay_diag.station_last_len = kept;

        Wifi_DlPlayHandleStationReply(client, port, reply, reply_size);
        return;
    }

    // Only the type is needed to tell the messages apart. Each one checks that
    // its own arguments are there before reading them.

    switch (reply[0])
    {
        case DLPLAY_REPLY_OP_DUMMY:
            // The client is listening and has nothing to say. This is what it
            // sends between every step of the exchange, so it is the normal
            // state of a healthy connection, not a fault.
            dlplay_diag.replies[WIFI_DLPLAY_REPLY_DUMMY]++;
            break;

        case DLPLAY_REPLY_OP_FILEREQ:
            // One piece of the request asking for the program. The state only
            // moves on once every piece has arrived, inside the handler.
            dlplay_diag.replies[WIFI_DLPLAY_REPLY_FILEREQ]++;
            Wifi_DlPlayHandleFileRequest(client, reply, reply_size);
            break;

        case DLPLAY_REPLY_OP_ACCEPT_FILEINFO:
            // The client has taken the boot information and knows where to load
            // the program. It keeps sending this until the blocks start, and
            // Nintendo's host uses the second one to begin, so this does too.
            dlplay_diag.replies[WIFI_DLPLAY_REPLY_ACCEPT_FILEINFO]++;

            // A console running the Download Station program takes the boot
            // information like any other and only then goes its own way, asking
            // for files instead of waiting for blocks. This is the point where
            // the Wii sender compares the client's name as well.
            if (Wifi_DlPlayClientIsStation(client))
            {
                if (client->stage == DLPLAY_PSTATE_REQ_ACCEPTED)
                    Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_STATION);

                break;
            }

            if (client->stage == DLPLAY_PSTATE_REQ_ACCEPTED)
                Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_WAIT_TO_SEND);
            else if (client->stage == DLPLAY_PSTATE_WAIT_TO_SEND)
                Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_SEND_PROCEED);
            break;

        case DLPLAY_REPLY_OP_CONTINUE:
            // The client names the lowest block it is still missing, and how
            // many it holds. The block number is a 16 bit value right after the
            // type; the count that follows it is only useful as a progress
            // report. This used to read the count as the block number, which
            // only agrees with the truth while nothing is ever lost.
            dlplay_diag.replies[WIFI_DLPLAY_REPLY_CONTINUE]++;

            if (reply_size >= 5)
            {
                int requested = reply[1] | (reply[2] << 8);

                // What this client wants. The stream follows all of them
                // together, which the sender works out when it builds the next
                // frame.
                //
                // Progress is the count of blocks it holds going up, not the
                // block number going up.
                //
                // A client asks for the next block it is missing *after the one
                // it last received*, and wraps round to the start for what it
                // missed on the way -- so a healthy client routinely asks for a
                // lower number than last time. Reading that as "no progress"
                // gave up on consoles that were downloading perfectly, and did
                // it to both of them whenever two shared the stream, because
                // wrapping is exactly what a second console does when it joins
                // part way through. The count only ever goes up.
                u16 received = reply[3] | (reply[4] << 8);

                if (received > client->request.blocks_received)
                    client->wait_count = 0;

                client->request.blocks_received = received;
                client->next_block = requested;

                dlplay_block_requested = true;

                dlplay_diag.block_requested = requested;
                dlplay_diag.blocks_received = client->request.blocks_received;
            }

            if (client->stage == DLPLAY_PSTATE_WAIT_TO_SEND)
                Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_SEND_PROCEED);
            break;

        case DLPLAY_REPLY_OP_STOPREQ:
            // The client has every block. It keeps sending this until it is told
            // to start the program.
            dlplay_diag.replies[WIFI_DLPLAY_REPLY_STOPREQ]++;

            if (client->stage == DLPLAY_PSTATE_SEND_PROCEED)
            {
                Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_SEND_COMPLETE);
            }
            else if (client->stage == DLPLAY_PSTATE_SEND_COMPLETE)
            {
                // It has the whole program and is asking what to do. Starting it
                // straight away is the old behaviour and the default; a host
                // that wants everyone to start together holds them here instead
                // until it asks, which is Nintendo's req2child[] in miniature.
                if ((dlplay_boot_mode == WIFI_DLPLAY_BOOT_AUTOMATIC)
                    || client->boot_requested)
                    Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_BOOT_REQUEST);
            }
            break;

        case DLPLAY_REPLY_OP_APP:
            // A console running a program this host sent earlier. It isn't
            // asking for anything, so it stops being part of a transfer and
            // belongs to the application from here on.
            if (client->stage != DLPLAY_PSTATE_APP)
                Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_APP);

            client->wait_count = 0;

            if (dlplay_app_handler != NULL)
            {
                // Interrupt context, like every other reply. Whatever the
                // application does here is subject to the same restrictions as
                // any packet handler.
                dlplay_app_handler(aid, reply + 1, reply_size - 1, dlplay_app_arg);
            }
            break;

        case DLPLAY_REPLY_OP_BOOTREQ_ACCEPTED:
            // Deliberately no state change, the way Nintendo's host does it. The
            // client answers the request to boot and then answers it again, and
            // it is the second request that makes it leave and start the
            // program. The transfer ends when it disconnects.
            dlplay_diag.replies[WIFI_DLPLAY_REPLY_BOOTREQ_ACCEPTED]++;

            // It does decide when the host stops asking, though.
            if (client->boot_accepted == 0)
                client->boot_accepted = 1;
            break;

        default:
            dlplay_diag.drops[WIFI_DLPLAY_DROP_UNKNOWN_OPCODE]++;
            break;
    }
}

const Wifi_DlPlayDiag *Wifi_DlPlayGetDiag(void)
{
    return &dlplay_diag;
}

// Picks how many consoles to serve and how big a frame to serve them with.
//
// The two trade against each other. Nintendo checks the air time a
// configuration needs before allowing it (MBi_IsCommSizeValid, mb_wm_base.c):
//
//     330 + 4 * (frame + 38) + clients * (112 + 4 * (reply + 32))  <  5600 us
//
// A frame of 510 bytes costs 2522 us before any client is counted, and each
// client another 272, so only eleven fit. The 256 a retail station announces
// costs 1506 and leaves room for all fifteen. Bigger frames halve the number
// needed for a transfer, so small rooms keep them and large ones give them up.
//
// This is decided here because beacon frames carry the size, and they go out
// before anyone connects.
static void Wifi_DlPlayPickFrameSize(const Wifi_DlPlayInfo *info)
{
    u8 players = (info != NULL) ? info->max_players : 0;

    if ((players < 2) || (players > DSWIFI_DLPLAY_MAX_PLAYERS))
        players = DLPLAY_MAX_PLAYERS;

    dlplay_max_clients = players - 1;

    if (dlplay_max_clients > DLPLAY_LARGE_ROOM_CLIENTS)
    {
        dlplay_cmd_data_size = DLPLAY_CMD_DATA_SIZE_SMALL;
        dlplay_block_size = DLPLAY_CMD_DATA_SIZE_SMALL - DLPLAY_BLOCK_OVERHEAD;
    }
    else
    {
        dlplay_cmd_data_size = DLPLAY_CMD_DATA_SIZE;
        dlplay_block_size = DLPLAY_BLOCK_SIZE;
    }
}

int Wifi_DlPlayStart(const void *rom, size_t rom_size, const Wifi_DlPlayInfo *info)
{
    if (dlplay_active)
        return -1;

    // How many consoles to serve, and how big a frame to serve them with. This
    // comes first because it decides how the program is cut up.
    Wifi_DlPlayPickFrameSize(info);
    Wifi_DlPlayRomSetBlockSize(dlplay_block_size);

    dlplay_room_locked = false;
    dlplay_room_full = false;
    dlplay_boot_mode = WIFI_DLPLAY_BOOT_AUTOMATIC;
    dlplay_app_out_pending = false;
    dlplay_app_out_size = 0;

    dlplay_user_param_set = (info != NULL) && (info->user_param != NULL);

    if (dlplay_user_param_set)
    {
        memcpy(dlplay_user_param, info->user_param,
               DSWIFI_DLPLAY_USER_PARAM_SIZE);
    }

    if (Wifi_DlPlayRomParse(&dlplay_rom, rom, rom_size) != 0)
        return -1;

    Wifi_DlPlayResetClient();
    memset(&dlplay_diag, 0, sizeof(dlplay_diag));
    dlplay_packnum = 0;
    dlplay_state = WIFI_DLPLAY_IDLE;

    if (Wifi_MultiplayerHostMode(dlplay_max_clients, dlplay_cmd_data_size + 4,
                                 DLPLAY_REPLY_DATA_SIZE) != 0)
        return -1;

    Wifi_MultiplayerFromClientSetPacketHandler(Wifi_DlPlayFromClientHandler);
    Wifi_MultiplayerAllowNewClients(true);

    // This hands the fragments of the game information record over to the ARM7,
    // which doesn't need the hardware to be in host mode yet.
    if (Wifi_DlPlayBeaconSetInfo(&dlplay_rom, info) != 0)
    {
        // Undo the host mode setup above. Returning without doing it would leave
        // the library accepting clients for a transfer that will never start, and
        // Wifi_DlPlayStop() refuses to clean up because dlplay_active is false.
        Wifi_MultiplayerAllowNewClients(false);
        Wifi_MultiplayerFromClientSetPacketHandler(NULL);
        return -1;
    }

    dlplay_beacon_started = false;
    dlplay_active = true;

    return 0;
}

// Creates the beacon frame that announces the program. It can only be done once
// the ARM7 has switched to host mode.
// Returns the value announced as "stream code" (TGID). Clients use it to tell
// one session of a host from the next, so it has to change every time the host
// starts. Official software seeds it from the clock and increments it on every
// call, which is what this does. rand() isn't used because nothing in the
// library calls srand(), so it would return the same value on every boot.
static u16 Wifi_DlPlayNextStreamCode(void)
{
    static bool seeded = false;
    static u16 stream_code;

    if (!seeded)
    {
        time_t now = time(NULL);
        struct tm *t = gmtime(&now);

        if (t != NULL)
            stream_code = (u16)(t->tm_sec + (t->tm_min << 8));

        seeded = true;
    }

    return stream_code++;
}

static int Wifi_DlPlayStartBeacon(void)
{
    // Official hosts use a value that changes every time they start. It goes in
    // two places: the stream code of the vendor element, and the SSID.
    u16 stream_code = Wifi_DlPlayNextStreamCode();

    // The SSID of a DS Download Play host isn't text. Its first six bytes are the
    // game ID and the stream code, and the rest is zero.
    u8 ssid[DLPLAY_SSID_SIZE];

    memset(ssid, 0, sizeof(ssid));

    ssid[0] = (DLPLAY_GAME_ID >> 0) & 0xFF;
    ssid[1] = (DLPLAY_GAME_ID >> 8) & 0xFF;
    ssid[2] = (DLPLAY_GAME_ID >> 16) & 0xFF;
    ssid[3] = (DLPLAY_GAME_ID >> 24) & 0xFF;
    ssid[4] = stream_code & 0xFF;
    ssid[5] = (stream_code >> 8) & 0xFF;

    Wifi_BeaconVendorInfo vendor =
    {
        .fixed_id = DLPLAY_FIXED_ID,
        .game_id = DLPLAY_GAME_ID,
        // A capture of a real console hosting DS Download Play has 10 here,
        // while its stream code is something else entirely, so this field is not
        // a copy of it as this code used to assume.
        .stepping_offset = 10,
        .stream_code = stream_code,
        .beacon_type = DLPLAY_BEACON_TYPE_IDLE,
        // These are the sizes of the payload of the frames, without the two
        // headers that the protocol adds to them.
        .cmd_data_size = dlplay_cmd_data_size,
        .reply_data_size = DLPLAY_REPLY_DATA_SIZE,
        // The beacon starts with the first fragment of the game information
        // record. The ARM7 replaces it with the next one before every beacon
        // frame is transmitted.
        .extra_data = Wifi_DlPlayBeaconGetFirstFragment(),
        .extra_data_size = DLPLAY_FRAGMENT_SIZE,
        .extra_data_layout = DSWIFI_BEACON_LAYOUT_RAW,
        // A retail game hosting DS Download Play announces no SSID at all. The
        // one built above still matters: it is what the client sends back in its
        // association request, and what the host answers probe requests for.
        .omit_ssid = true,
    };

    return Wifi_BeaconStartRawSsid(ssid, sizeof(ssid), &vendor);
}

void Wifi_DlPlayStop(void)
{
    if (!dlplay_active)
        return;

    dlplay_active = false;

    Wifi_MultiplayerFromClientSetPacketHandler(NULL);
    Wifi_MultiplayerAllowNewClients(false);
    Wifi_IdleMode();

    dlplay_state = WIFI_DLPLAY_IDLE;
}

void Wifi_DlPlayUpdate(void)
{
    if (!dlplay_active)
        return;

    if (!dlplay_beacon_started)
    {
        // Wait until the ARM7 has switched to host mode before announcing the
        // program, or the beacon frame would be discarded.
        if (!Wifi_LibraryModeReady())
            return;

        if (Wifi_DlPlayStartBeacon() != 0)
        {
            dlplay_state = WIFI_DLPLAY_ERROR;
            dlplay_active = false;
            return;
        }

        dlplay_beacon_started = true;
    }

    // Take up whoever has associated since the last pass, and forget whoever has
    // gone. The multiplayer layer keeps the list; this only has to notice the
    // difference.
    u16 associated = Wifi_MultiplayerGetClientMask() & ~1;
    bool booted = false;
    bool left = false;

    for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
    {
        Wifi_DlPlayClient *client = &dlplay_clients[aid];
        bool here = (associated & (1 << aid)) != 0;

        if (here && (client->stage == DLPLAY_PSTATE_NONE))
        {
            Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_CONNECTED);
            continue;
        }

        if (!here && (client->stage != DLPLAY_PSTATE_NONE))
        {
            // Gone. Having been told to start the program, that is the expected
            // end of its transfer rather than a client lost part way through.
            if ((client->stage == DLPLAY_PSTATE_BOOT_REQUEST)
                || (client->stage == DLPLAY_PSTATE_SESSION_END))
                booted = true;

            Wifi_DlPlayResetOneClient(client);
            Wifi_DlPlayUpdateSummaryState();

            left = true;
        }
    }

    // Tell the room who is in it. A client only counts as a player once it has
    // asked to join and said who it is, because the beacon carries a name and a
    // colour for everyone it lists, and announcing someone without those makes
    // clients throw away the game information and wait for details that never
    // arrive.
    u16 players = 0;

    for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
    {
        if (dlplay_clients[aid].request.valid)
            players |= 1 << aid;
    }

    Wifi_DlPlayBeaconSetPlayers(players);

    if (associated == 0)
    {
        // Nobody is here. A console that has just been sent the Download Station
        // program reconnects a moment later to ask it for content, so say so
        // rather than treating the boot as the end of everything.
        if (left)
        {
            Wifi_DlPlayResetClient();
            Wifi_DlPlayUpdateSummaryState();

            if (booted)
                dlplay_state = WIFI_DLPLAY_BOOTING;

            Wifi_BeaconPatchVendorByte(FIE_NINTENDO_OFS_BEACON_TYPE,
                                       DLPLAY_BEACON_TYPE_IDLE);
        }

        dlplay_room_full = false;
        Wifi_DlPlayApplyRoomState();

        return;
    }

    // Close the room by itself once it is full, and open it again when somebody
    // leaves. An explicit lock from the application outranks both.
    dlplay_room_full = (Wifi_MultiplayerGetNumClients() >= dlplay_max_clients);

    Wifi_DlPlayApplyRoomState();

    Wifi_DlPlaySendForState();
}

Wifi_DlPlayState Wifi_DlPlayGetState(void)
{
    return dlplay_state;
}

void Wifi_DlPlayGetProgress(int *current, int *total)
{
    // How far the room has got, which is the console furthest behind rather than
    // the block being transmitted: the stream runs ahead of every client, and
    // one that is still catching up hasn't finished because the others have.
    if (current != NULL)
    {
        int lowest = -1;

        for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
        {
            const Wifi_DlPlayClient *client = &dlplay_clients[aid];

            if (client->stage == DLPLAY_PSTATE_NONE)
                continue;

            if ((lowest < 0) || (client->next_block < lowest))
                lowest = client->next_block;
        }

        *current = (lowest < 0) ? 0 : lowest;
    }

    if (total != NULL)
        *total = dlplay_active ? dlplay_rom.total_blocks : 0;
}

// A request nobody has filled in, handed back for an association ID that isn't
// serving anyone, so that a caller always has something to read.
static const Wifi_DlPlayClientRequest dlplay_no_request;

int Wifi_DlPlayAppReply(const void *data, size_t size)
{
    if ((data == NULL) || (size == 0) || (size > DSWIFI_DLPLAY_REPLY_MAX_SIZE))
        return -1;

    // The opcode that tells the host this isn't part of a transfer. A console
    // running a program the host sent has no reason to speak that protocol
    // again, and this is the only thing distinguishing it from one that is.
    u8 reply[1 + DSWIFI_DLPLAY_REPLY_MAX_SIZE];

    reply[0] = DLPLAY_REPLY_OP_APP;
    memcpy(reply + 1, data, size);

    return Wifi_MultiplayerClientReplyTxFrame(reply, 1 + size);
}

int Wifi_DlPlayAppReadMessage(int base, int len, void *buffer, size_t size)
{
    // What the host sends is a WM port packet: two bytes of header, then the
    // message. The multiplayer layer hands a client everything from the header
    // on, so both have to be stepped over here rather than by the caller.
    if (len < 3)
        return -1;

    u8 header[3];

    Wifi_RxRawReadPacket(base, sizeof(header), header);

    if (header[2] != DLPLAY_CMD_OP_APP)
        return -1;

    size_t available = len - 3;

    if (available > size)
        available = size;

    if (available > 0)
        Wifi_RxRawReadPacket(base + 3, available, buffer);

    return available;
}

void Wifi_DlPlaySetAppHandler(Wifi_DlPlayAppHandlerFn fn, void *arg)
{
    dlplay_app_handler = fn;
    dlplay_app_arg = arg;
}

int Wifi_DlPlayAppSend(const void *data, size_t size)
{
    if ((data == NULL) || (size == 0) || (size > DSWIFI_DLPLAY_APP_MAX_SIZE))
        return -1;

    // One message at a time. The host sends what it has on the next frame that
    // isn't needed by a transfer, so an application that produces them faster
    // than that would only be overwriting what hasn't gone out yet.
    if (dlplay_app_out_pending)
        return -1;

    memcpy(dlplay_app_out, data, size);
    dlplay_app_out_size = size;
    dlplay_app_out_pending = true;

    return 0;
}

void Wifi_DlPlaySetBootMode(Wifi_DlPlayBootMode mode)
{
    dlplay_boot_mode = mode;
}

bool Wifi_DlPlayBootClient(int aid)
{
    Wifi_DlPlayClient *client = Wifi_DlPlayClientByAid(aid);

    if (client == NULL)
        return false;

    // Only a console that has the whole program can be started. Asking earlier
    // would tell it to run something it hasn't finished receiving.
    // A station client and a console already running what it was sent both
    // belong to another layer, and neither is waiting to be started.
    if (client->stage != DLPLAY_PSTATE_SEND_COMPLETE)
        return false;

    client->boot_requested = true;
    Wifi_DlPlaySetClientStage(client, DLPLAY_PSTATE_BOOT_REQUEST);

    return true;
}

bool Wifi_DlPlayBootAll(void)
{
    bool any = false;

    for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
    {
        if (Wifi_DlPlayBootClient(aid))
            any = true;
    }

    return any;
}

void Wifi_DlPlayLockRoom(bool locked)
{
    dlplay_room_locked = locked;

    if (dlplay_active)
        Wifi_DlPlayApplyRoomState();
}

bool Wifi_DlPlayRoomLocked(void)
{
    return dlplay_room_locked || dlplay_room_full;
}

u16 Wifi_DlPlayGetClientMask(void)
{
    u16 mask = 0;

    for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
    {
        if (dlplay_clients[aid].stage != DLPLAY_PSTATE_NONE)
            mask |= 1 << aid;
    }

    return mask;
}

const char *Wifi_DlPlayGetClientNameByAID(int aid)
{
    const Wifi_DlPlayClient *client = Wifi_DlPlayClientByAid(aid);

    if ((client == NULL) || !client->request.valid)
        return NULL;

    return client->name_ascii;
}

const Wifi_DlPlayClientRequest *Wifi_DlPlayGetClientRequestByAID(int aid)
{
    const Wifi_DlPlayClient *client = Wifi_DlPlayClientByAid(aid);

    if (client == NULL)
        return &dlplay_no_request;

    return &client->request;
}

Wifi_DlPlayState Wifi_DlPlayGetClientState(int aid)
{
    const Wifi_DlPlayClient *client = Wifi_DlPlayClientByAid(aid);

    if (client == NULL)
        return WIFI_DLPLAY_IDLE;

    return Wifi_DlPlayStateOfStage(client->stage);
}

void Wifi_DlPlayGetClientProgress(int aid, int *current, int *total)
{
    const Wifi_DlPlayClient *client = Wifi_DlPlayClientByAid(aid);

    if (current != NULL)
        *current = (client != NULL) ? client->request.blocks_received : 0;

    if (total != NULL)
        *total = dlplay_active ? dlplay_rom.total_blocks : 0;
}

const char *Wifi_DlPlayGetClientName(void)
{
    const Wifi_DlPlayClient *client = Wifi_DlPlayFirstClient();

    if ((client == NULL) || !client->request.valid)
        return NULL;

    return client->name_ascii;
}

const Wifi_DlPlayClientRequest *Wifi_DlPlayGetClientRequest(void)
{
    const Wifi_DlPlayClient *client = Wifi_DlPlayFirstClient();

    if (client == NULL)
        return &dlplay_no_request;

    return &client->request;
}

void Wifi_DlPlayStationSetContentSource(Wifi_DlPlayStationOpenFn open,
                                        Wifi_DlPlayStationReadFn read, void *arg)
{
    // Both or neither: a name that can be sized but not read would be announced
    // to the client and then never arrive.
    if ((open == NULL) || (read == NULL))
    {
        dlplay_station_open = NULL;
        dlplay_station_read = NULL;
        dlplay_station_arg = NULL;
        return;
    }

    dlplay_station_open = open;
    dlplay_station_read = read;
    dlplay_station_arg = arg;
}
