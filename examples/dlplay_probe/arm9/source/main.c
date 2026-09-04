// SPDX-License-Identifier: CC0-1.0
//
// SPDX-FileContributor: Antonio Niño Díaz, 2025

// Connects to a console that is hosting DS Download Play and writes down every
// frame it sends, to probe_log.txt on the card.
//
// This exists because the host in this repository sends frames that a real
// console associates with and then ignores. Its beacon has been matched against
// a real host field by field with the sniffer, but the frames of the transfer
// itself have never been seen, and reading them off a working host settles what
// they should look like far better than piecing it together from documentation.
//


#include <stdio.h>
#include <string.h>

#include <nds.h>
#include <fat.h>
#include <filesystem.h>

#include <dswifi9.h>

#include "log.h"

#define MAX_APS         16
#define FRAME_BYTES     64
#define FRAME_SLOTS     16

typedef struct {
    u16 len;
    u16 stored;
    // The reply time and the client bitmap, which sit in the four bytes before
    // the payload and which the multiplayer layer hands over already skipped.
    // The host works its own out from the size it announces, and nobody has ever
    // compared it against what a real one sends.
    u16 reply_time;
    u16 client_bits;
    u8 data[FRAME_BYTES];
} CapturedFrame;

static CapturedFrame frames[FRAME_SLOTS];
static volatile int frames_head;
static volatile int frames_tail;
static volatile unsigned int frames_seen;
static volatile unsigned int frames_lost;

// Block types of the messages exchanged with the host. These are Nintendo's
// MBCommType values, the same ones the host uses.
#define OP_DUMMY                0
#define OP_SENDSTART            1
#define OP_DL_FILEINFO          3
#define OP_DATA                 4
#define OP_BOOTREQ              5
#define OP_FILEREQ              7
#define OP_ACCEPT_FILEINFO      8
#define OP_CONTINUE             9
#define OP_STOPREQ              10
#define OP_BOOTREQ_ACCEPTED     11

// Size in bytes of the part of the program that each block carries. It is the
// size the host announces in its beacon frames, less the six bytes of the
// command that carries it.
#define BLOCK_SIZE              (0x100 - 6)

// The request that asks for the program, and the pieces it is split into.
#define REQ_SIZE                29
#define REQ_PIECE_SIZE          6
#define REQ_PIECE_COUNT         5

#define PROBE_GAME_ID           0x00400120

#define MAX_BLOCKS              4096

// What this client is doing, mirroring MB_COMM_CSTATE_* on a real console.
typedef enum {
    // Waiting to be told it may ask for the program.
    CSTATE_CONNECT = 0,
    // Asking for it, one piece of the request per frame.
    CSTATE_REQUESTING,
    // The boot information has arrived and been taken.
    CSTATE_ACCEPTED,
    // Receiving blocks.
    CSTATE_RECEIVING,
    // Every block is in.
    CSTATE_COMPLETE,
    // Told to start the program.
    CSTATE_BOOTING,
} ClientState;

static volatile int client_state = CSTATE_CONNECT;

// Which piece of the request goes out next. A console starts at 1 and cycles
// 1, 2, 3, 4, 0, so this does too.
static volatile u8 request_piece = 1;

// Blocks that have arrived, and how many there are in total once the boot
// information says so.
static u8 block_received[MAX_BLOCKS / 8];
static volatile int blocks_total;
static volatile int blocks_got;
static volatile int block_next;

// Counts of what the host has sent, by kind, so a run can be read at a glance.
static volatile unsigned int cmd_count[16];

static bool BlockWasReceived(int block)
{
    if ((block < 0) || (block >= MAX_BLOCKS))
        return true;

    return (block_received[block >> 3] & (1 << (block & 7))) != 0;
}

static void BlockMarkReceived(int block)
{
    if ((block < 0) || (block >= MAX_BLOCKS))
        return;

    if (BlockWasReceived(block))
        return;

    block_received[block >> 3] |= 1 << (block & 7);
    blocks_got++;

    // The lowest block that is still missing is the one to ask for next.
    while ((block_next < blocks_total) && BlockWasReceived(block_next))
        block_next++;
}

// Reads a 32 bit value stored the way the protocol stores them.
static u32 ReadU32(const u8 *data)
{
    return data[0] | (data[1] << 8) | (data[2] << 16) | ((u32)data[3] << 24);
}

// Number of blocks needed to send "size" bytes. Each part of the program starts
// on a block of its own, so they are counted separately.
static int BlockCount(u32 size)
{
    return (size + (BLOCK_SIZE - 1)) / BLOCK_SIZE;
}

// Builds the 29 byte request that asks the host for the program, and copies one
// six byte piece of it out. 
static void BuildRequestPiece(u8 *dest, u8 piece)
{
    static const u16 name[] = { 'P', 'R', 'O', 'B', 'E' };

    u8 request[REQ_PIECE_SIZE * REQ_PIECE_COUNT];

    memset(request, 0, sizeof(request));

    request[0] = (PROBE_GAME_ID >> 0) & 0xFF;
    request[1] = (PROBE_GAME_ID >> 8) & 0xFF;
    request[2] = (PROBE_GAME_ID >> 16) & 0xFF;
    request[3] = (PROBE_GAME_ID >> 24) & 0xFF;

    // Favourite colour in the low four bits, player number in the high four.
    request[4] = 3;
    request[5] = sizeof(name) / sizeof(name[0]);

    for (unsigned int i = 0; i < (sizeof(name) / sizeof(name[0])); i++)
    {
        request[6 + (i * 2) + 0] = name[i] & 0xFF;
        request[6 + (i * 2) + 1] = name[i] >> 8;
    }

    request[26] = 1; // Version of the protocol a console speaks
    request[28] = 0; // The host offers one program, and it is number zero

    memcpy(dest, request + ((size_t)piece * REQ_PIECE_SIZE), REQ_PIECE_SIZE);
}

// Fills in the eight byte answer that the current state calls for.
static void BuildReply(u8 *reply)
{
    memset(reply, 0, 8);

    switch (client_state)
    {
        case CSTATE_REQUESTING:
            reply[0] = OP_FILEREQ;
            reply[1] = request_piece;
            BuildRequestPiece(reply + 2, request_piece);

            request_piece = (request_piece + 1) % REQ_PIECE_COUNT;
            break;

        case CSTATE_ACCEPTED:
            reply[0] = OP_ACCEPT_FILEINFO;
            break;

        case CSTATE_RECEIVING:
            reply[0] = OP_CONTINUE;
            reply[1] = block_next & 0xFF;
            reply[2] = (block_next >> 8) & 0xFF;
            reply[3] = blocks_got & 0xFF;
            reply[4] = (blocks_got >> 8) & 0xFF;
            break;

        case CSTATE_COMPLETE:
            reply[0] = OP_STOPREQ;
            break;

        case CSTATE_BOOTING:
            reply[0] = OP_BOOTREQ_ACCEPTED;
            break;

        default:
            // Nothing to say yet. A console sends this until the host tells it
            // that it may ask for the program.
            reply[0] = OP_DUMMY;
            break;
    }
}

// Reacts to one message from the host, the way MBi_CommChildRecvData does.
static void HandleHostCommand(const u8 *data, int len)
{
    if (len < 3)
        return;

    u8 command = data[2];

    cmd_count[command & 0xF]++;

    switch (command)
    {
        case OP_SENDSTART:
            if (client_state == CSTATE_CONNECT)
                client_state = CSTATE_REQUESTING;
            break;

        case OP_DL_FILEINFO:
        {
            if (client_state != CSTATE_REQUESTING)
                break;

            // The boot information says where each part of the program goes and
            // how big it is, which is everything needed to know how many blocks
            // there are. The sizes sit where GBATEK's RSA frame puts them.
            if (len < (3 + 0x38))
                break;

            const u8 *info = data + 3;

            blocks_total = BlockCount(ReadU32(info + 0x14))
                         + BlockCount(ReadU32(info + 0x24))
                         + BlockCount(ReadU32(info + 0x34));

            if ((blocks_total <= 0) || (blocks_total > MAX_BLOCKS))
            {
                blocks_total = 0;
                break;
            }

            client_state = CSTATE_ACCEPTED;
            break;
        }

        case OP_DATA:
        {
            if (client_state == CSTATE_ACCEPTED)
                client_state = CSTATE_RECEIVING;

            if (client_state != CSTATE_RECEIVING)
                break;

            if (len < 7)
                break;

            BlockMarkReceived(data[5] | (data[6] << 8));

            if ((blocks_total > 0) && (blocks_got >= blocks_total))
                client_state = CSTATE_COMPLETE;

            break;
        }

        case OP_BOOTREQ:
            if (client_state == CSTATE_COMPLETE)
                client_state = CSTATE_BOOTING;
            break;

        default:
            break;
    }
}

static const char *StateName(int state)
{
    switch (state)
    {
        case CSTATE_CONNECT:
            return "waiting for the host";
        case CSTATE_REQUESTING:
            return "asking for the program";
        case CSTATE_ACCEPTED:
            return "boot information taken";
        case CSTATE_RECEIVING:
            return "receiving";
        case CSTATE_COMPLETE:
            return "have every block";
        default:
            return "booting";
    }
}

// Frames from another console taking part in the same session, kept apart from
// the host's so the two logs can be read side by side.
//
// A console that is only listening never sees multiplayer frames: the hardware
// appears to process them only while it is itself in the exchange, which is why
// wifi_sniffer captures beacons and nothing else. Joining the session as a
// second client is the way in.
typedef struct {
    u16 stored;
    u8 data[16];
} PeerFrame;

#define PEER_SLOTS      32

static PeerFrame peers[PEER_SLOTS];
static volatile int peers_head;
static volatile int peers_tail;
static volatile unsigned int peers_seen;

static u8 own_mac[6];

// Every frame the hardware hands over, including the ones the multiplayer layer
// has no use for. This is where another client's answers turn up.
static void RawPacketHandler(int base, int len)
{
    if (len < 26)
        return;

    u8 head[26];

    Wifi_RxRawReadPacket(base, sizeof(head), head);

    u16 fc = head[0] | (head[1] << 8);
    u16 type = fc & 0x00FC;

    // Data + CF-Ack, with or without a body: a client answering the host. The
    // host's own acknowledgement shares the subtype, so it is told apart by
    // direction.
    if (((type != 0x0018) && (type != 0x0058)) || !(fc & 0x0100))
        return;

    // Our own answers are already known.
    if (memcmp(head + 10, own_mac, 6) == 0)
        return;

    peers_seen++;

    int next = (peers_head + 1) % PEER_SLOTS;
    if (next == peers_tail)
        return;

    PeerFrame *frame = &peers[peers_head];

    int stored = len;
    if (stored > (int)sizeof(frame->data))
        stored = sizeof(frame->data);

    Wifi_RxRawReadPacket(base, stored, frame->data);
    frame->stored = stored;

    peers_head = next;
}

// Runs in the wireless interrupt, so it only copies bytes.
static void FromHostPacketHandler(Wifi_MPPacketType type, int base, int len)
{
    if (type != WIFI_MPTYPE_CMD)
        return;

    frames_seen++;

    int next = (frames_head + 1) % FRAME_SLOTS;

    if (next == frames_tail)
    {
        frames_lost++;
        return;
    }

    CapturedFrame *frame = &frames[frames_head];

    int stored = len;
    if (stored > FRAME_BYTES)
        stored = FRAME_BYTES;
    if (stored < 0)
        stored = 0;

    if (stored > 0)
        Wifi_RxRawReadPacket(base, stored, frame->data);

    // The four bytes in front of the payload, which the handler is given a
    // pointer past.
    u8 head[4];

    Wifi_RxRawReadPacket(base - 4, sizeof(head), head);

    frame->reply_time = head[0] | (head[1] << 8);
    frame->client_bits = head[2] | (head[3] << 8);

    frame->len = len;
    frame->stored = stored;

    frames_head = next;

    // Act on it here rather than in the main loop. A console has "a few hundred
    // clock cycles" to prepare its answer after a command arrives, and getting
    // the state right at that moment is what the host is being tested on.
    HandleHostCommand(frame->data, stored);
}

// Frames repeat, so only the ones that say something new are written out.
static u8 previous[FRAME_BYTES];
static u16 previous_stored;
static bool previous_valid;

static bool IsWorthLogging(const CapturedFrame *frame)
{
    if (previous_valid && (previous_stored == frame->stored)
        && (memcmp(previous, frame->data, frame->stored) == 0))
        return false;

    memcpy(previous, frame->data, frame->stored);
    previous_stored = frame->stored;
    previous_valid = true;

    return true;
}

static void LogFrame(const CapturedFrame *frame)
{
    char hex[(FRAME_BYTES * 3) + 1];
    int pos = 0;

    for (int i = 0; i < frame->stored; i++)
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", frame->data[i]);

    LogPrintf("[cmd] len=%u time=%04x bits=%04x %s\n", frame->len,
              frame->reply_time, frame->client_bits, hex);

    // The first two bytes are the size in halfwords and the flags, and the
    // command follows them.
    if (frame->stored >= 3)
    {
        LogPrintf("      size=%u halfwords, flags=%02x, command=%02x\n",
                  frame->data[0], frame->data[1], frame->data[2]);
    }
}

static Wifi_AccessPoint hosts[MAX_APS];

// Lets the user pick one of the consoles that are hosting.
static bool SelectHost(int *out)
{
    int chosen = 0;

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();

        u16 keys = keysDown();

        if (keys & KEY_START)
            return false;

        int found = Wifi_GetNumAP();
        if (found > MAX_APS)
            found = MAX_APS;

        int listed = 0;
        for (int i = 0; i < found; i++)
        {
            if (Wifi_GetAPData(i, &hosts[listed]) == WIFI_RETURN_OK)
                listed++;
        }

        if (listed > 0)
        {
            if (keys & KEY_UP)
                chosen = (chosen == 0) ? (listed - 1) : (chosen - 1);
            if (keys & KEY_DOWN)
                chosen = (chosen + 1) % listed;
            if (keys & KEY_A)
            {
                *out = chosen;
                return true;
            }
        }

        if (chosen >= listed)
            chosen = 0;

        consoleClear();
        printf("DS Download Play probe\n");
        printf("======================\n\n");
        printf("Consoles hosting a game:\n\n");

        for (int i = 0; i < listed; i++)
        {
            printf("%c Ch %2d  RSSI %3d\n", (i == chosen) ? '>' : ' ',
                   hosts[i].channel, hosts[i].rssi);
        }

        if (listed == 0)
            printf("  Searching...\n");

        printf("\nStart Download Play on the\n");
        printf("other console and pick its\n");
        printf("game, so that it starts\n");
        printf("looking for someone.\n");
        printf("\nUP/DOWN: Pick  A: Connect\n");
        printf("START: Exit\n");
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    consoleDemoInit();

    printf("DS Download Play probe\n");
    printf("======================\n\n");

    if (!nitroFSInit(NULL))
    {
        if (!fatInitDefault())
        {
            printf("Failed to initialize storage.\n");
            goto wait_exit;
        }
    }

    if (!LogInit())
    {
        printf("Can't write to the card.\n");
        goto wait_exit;
    }

    LogPrintf("DS Download Play probe log\n");

    if (!Wifi_InitDefault(INIT_ONLY))
    {
        printf("Can't initialize WiFi.\n");
        goto wait_exit;
    }

    // Announce the same reply size a real client does, so that the host sizes
    // its frames the way it would for one.
    Wifi_MultiplayerClientMode(8);

    while (!Wifi_LibraryModeReady())
        swiWaitForVBlank();

    // Only consoles hosting a game are of interest, not access points.
    Wifi_ScanModeFilter(WSCAN_LIST_NDS_HOSTS);

    int chosen;
    if (!SelectHost(&chosen))
        goto stop;

    LogPrintf("connecting to a host on channel %d\n", hosts[chosen].channel);

    Wifi_GetData(WIFIGETDATA_MACADDRESS, sizeof(own_mac), own_mac);

    Wifi_MultiplayerFromHostSetPacketHandler(FromHostPacketHandler);
    Wifi_RawSetPacketHandler(RawPacketHandler);
    Wifi_ConnectOpenAP(&hosts[chosen]);

    unsigned int written = 0;
    int last_state = -1;

    // Answering drives a host through a transfer, which is what this is for
    // most of the time. Listening in on a host that is already serving another
    // console needs the opposite: join, say nothing, and write down what the
    // other console says.
    bool listen_only = true;

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();

        if (keysDown() & KEY_START)
            break;

        int status = Wifi_AssocStatus();

        // Queue the answer the current state calls for. The hardware sends it
        // in this console's slot of the next command, and sends an empty frame
        // instead if nothing has been queued, which is exactly what the host
        // has been seeing from a real console.
        if (keysDown() & KEY_SELECT)
        {
            listen_only = !listen_only;
            LogPrintf("[mode] %s\n", listen_only ? "listening only" : "answering");
        }

        if ((status == ASSOCSTATUS_ASSOCIATED) && !listen_only)
        {
            u8 reply[8];

            BuildReply(reply);
            Wifi_MultiplayerClientReplyTxFrame(reply, sizeof(reply));
        }

        // Whatever the other console in the session is telling the host.
        while (peers_tail != peers_head)
        {
            PeerFrame *frame = &peers[peers_tail];

            char hex[(16 * 3) + 1];
            int pos = 0;

            for (int i = 0; i < frame->stored; i++)
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", frame->data[i]);

            LogPrintf("[peer] len=%u %s\n", frame->stored, hex);

            peers_tail = (peers_tail + 1) % PEER_SLOTS;
        }

        if (client_state != last_state)
        {
            last_state = client_state;
            LogPrintf("[state] %s\n", StateName(last_state));
        }

        while (frames_tail != frames_head)
        {
            CapturedFrame *frame = &frames[frames_tail];

            if (IsWorthLogging(frame))
            {
                LogFrame(frame);
                written++;
            }

            frames_tail = (frames_tail + 1) % FRAME_SLOTS;
        }

        LogFlush();

        consoleClear();
        printf("DS Download Play probe\n");
        printf("======================\n\n");
        printf("Log:     %s\n", LogPath());
        printf("Status:  %s\n\n", ASSOCSTATUS_STRINGS[status]);
        printf("Frames:  %u\n", frames_seen);
        printf("Written: %u\n", written);
        printf("Lost:    %u\n", frames_lost);
        printf("\nMode:    %s\n", listen_only ? "listening" : "answering");
        printf("Peer:    %u frames\n", peers_seen);
        printf("Doing:   %s\n", StateName(client_state));
        printf("Blocks:  %d / %d\n", blocks_got, blocks_total);
        printf("Want:    %d\n", block_next);

        // What the host has actually sent, by kind. A host that never gets past
        // one of these is stuck at that step.
        printf("Got: start=%u info=%u data=%u\n",
               cmd_count[OP_SENDSTART], cmd_count[OP_DL_FILEINFO],
               cmd_count[OP_DATA]);
        printf("     boot=%u idle=%u\n",
               cmd_count[OP_BOOTREQ], cmd_count[OP_DUMMY]);

        printf("\nSELECT: Mode  START: Stop\n");
    }

stop:
    LogPrintf("[stop] %u frames seen, %d of %d blocks, state %s\n",
              frames_seen, blocks_got, blocks_total, StateName(client_state));
    LogPrintf("[stop] %u frames from another console\n", peers_seen);
    LogPrintf("[stop] start=%u info=%u data=%u boot=%u idle=%u\n",
              cmd_count[OP_SENDSTART], cmd_count[OP_DL_FILEINFO],
              cmd_count[OP_DATA], cmd_count[OP_BOOTREQ], cmd_count[OP_DUMMY]);
    LogClose();

    Wifi_IdleMode();

    return 0;

wait_exit:
    printf("\nPress START to exit.\n");
    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START)
            break;
    }

    return 0;
}
