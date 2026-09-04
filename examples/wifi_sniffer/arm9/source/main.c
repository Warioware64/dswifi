// SPDX-License-Identifier: CC0-1.0
//
// SPDX-FileContributor: Antonio Niño Díaz, 2025

// Records the wireless frames of other consoles to a file on the card.
//
// This exists for the DS Download Play work. Building a host that a retail
// console will talk to means matching what a real host puts on the air, and
// reading Nintendo's own beacon back is a great deal more reliable than working
// it out from documentation. Run a console that hosts DS Download Play next to
// this one and the beacon it sends is written out byte for byte.

#include <stdio.h>
#include <string.h>

#include <nds.h>
#include <fat.h>
#include <filesystem.h>

#include <dswifi9.h>

#include "log.h"

// Bytes kept from each frame. Enough for the whole body of a beacon carrying a
// DS Download Play game information element.
#define FRAME_BYTES     320

// Frames waiting to be written out. The handler runs in an interrupt and only
// copies, so this has to absorb whatever arrives between two passes of the main
// loop.
#define FRAME_SLOTS     8

typedef struct {
    u16 len;
    u16 stored;
    u8 data[FRAME_BYTES];
} CapturedFrame;

static CapturedFrame frames[FRAME_SLOTS];
static volatile int frames_head;
static volatile int frames_tail;
static volatile unsigned int frames_seen;
static volatile unsigned int frames_lost;

// Frames of each kind that have been seen, so it is obvious on screen whether a
// capture is picking up the exchange or only the beacons.
static unsigned int kind_count[8];

// Called from the wireless interrupt for every frame the ARM7 forwards. It only
// copies bytes: no printf, no malloc, no file access.
static void PacketHandler(int packetID, int packetlength)
{
    frames_seen++;

    int next = (frames_head + 1) % FRAME_SLOTS;

    if (next == frames_tail)
    {
        frames_lost++;
        return;
    }

    CapturedFrame *frame = &frames[frames_head];

    int stored = packetlength;
    if (stored > FRAME_BYTES)
        stored = FRAME_BYTES;

    Wifi_RxRawReadPacket(packetID, stored, frame->data);
    frame->len = packetlength;
    frame->stored = stored;

    frames_head = next;
}

// Called from the main loop for every frame, whether or not it gets written out.
static void CountFrame(const CapturedFrame *frame);

// What this can and cannot see
// ============================
//
// The receiver filters by address before any of the bits in W_RXFILTER apply
// (GBATEK, "W_RXFILTER"):
//
//     DA = our MAC                       always received
//     DA = broadcast, BSSID = W_BSSID    always received
//     DA = broadcast, BSSID = other      received when RXFILTER bit 0 is set
//
// A frame addressed to another console is never received, whatever the filter
// says. That rules out watching the authentication and association exchange
// between two other consoles: those frames are unicast.
//
// What can be watched is everything a DS Download Play session sends to a group
// address: the beacons, the probe requests, and the whole multiplayer exchange,
// which goes to 03:09:BF:00:00:00 (the host polling), 03:09:BF:00:00:10 (a
// client answering) and 03:09:BF:00:00:03 (the host acknowledging). The client's
// answers are the interesting part, and they are all reachable.

// Nintendo's vendor information element, which is what makes a beacon
// interesting here. Returns its offset in the frame, or -1.
static int FindNintendoElement(const u8 *frame, int len)
{
    // 24 byte header, then timestamp, beacon interval and capabilities.
    int i = 24 + 12;

    while ((i + 2) <= len)
    {
        int id = frame[i];
        int size = frame[i + 1];

        if ((i + 2 + size) > len)
            break;

        if ((id == 0xDD) && (size >= 4) && (frame[i + 2] == 0x00)
            && (frame[i + 3] == 0x09) && (frame[i + 4] == 0xBF))
            return i;

        i += 2 + size;
    }

    return -1;
}

// What a frame is, as far as this is concerned. The order matters: everything
// from KIND_HANDSHAKE on is rare and written out every time, while the rest
// repeats constantly and is only written when its contents change.
enum {
    KIND_NONE = 0,
    KIND_BEACON,
    KIND_CMD,
    KIND_REPLY,
    KIND_ACK,
    KIND_HANDSHAKE,
};

static int ClassifyFrame(const u8 *d, int len)
{
    u16 fc = d[0] | (d[1] << 8);
    u16 type = fc & 0x00FC;

    switch (type)
    {
        case 0x0080: // Beacon
            // Only the ones announcing a game are of any interest.
            return (FindNintendoElement(d, len) >= 0) ? KIND_BEACON : KIND_NONE;

        case 0x0000: // Association request
        case 0x0010: // Association response
        case 0x0020: // Reassociation request
        case 0x0030: // Reassociation response
        case 0x00A0: // Disassociation
        case 0x00B0: // Authentication
        case 0x00C0: // Deauthentication
            // The whole handshake, both directions. This is what a console does
            // before it will say a word over multiplayer, and it is the part
            // that has never been watched.
            return KIND_HANDSHAKE;

        case 0x0028: // Data + CF-Poll: the host polling its clients
            return KIND_CMD;

        case 0x0018: // Data + CF-Ack: a client answering, with or without data
        case 0x0058: // CF-Ack: an answer with nothing in it
            // The host's own acknowledgement goes out with the same subtype as a
            // reply, so they are told apart by direction: a reply is toDS.
            return (fc & 0x0100) ? KIND_REPLY : KIND_ACK;

        default:
            return KIND_NONE;
    }
}

static void CountFrame(const CapturedFrame *frame)
{
    if (frame->stored < 24)
        return;

    int kind = ClassifyFrame(frame->data, frame->stored);

    if ((kind > 0) && (kind < 8))
        kind_count[kind]++;
}

static const char *KindName(int kind)
{
    switch (kind)
    {
        case KIND_BEACON:
            return "BEACON";
        case KIND_CMD:
            return "CMD";
        case KIND_REPLY:
            return "REPLY";
        case KIND_ACK:
            return "ACK";
        default:
            return "MGMT";
    }
}

// The channel a beacon says it is on, from its DS parameter set element, or 0.
static int BeaconChannel(const u8 *frame, int len)
{
    int i = 24 + 12;

    while ((i + 2) <= len)
    {
        int id = frame[i];
        int size = frame[i + 1];

        if ((i + 2 + size) > len)
            break;

        if ((id == 0x03) && (size == 1))
            return frame[i + 2];

        i += 2 + size;
    }

    return 0;
}

static void LogFrame(const CapturedFrame *frame)
{
    const u8 *d = frame->data;

    if (frame->stored < 24)
        return;

    u16 fc = d[0] | (d[1] << 8);
    const u8 *src = d + 10;

    int kind = ClassifyFrame(d, frame->stored);

    if (kind == KIND_NONE)
        return;

    int vendor = FindNintendoElement(d, frame->stored);

    if (kind == KIND_BEACON)
    {
        int ch = BeaconChannel(d, frame->stored);

        if (ch != 0)
            LogPrintf("[host] channel %d\n", ch);
    }

    LogPrintf("\n%s fc=%04x len=%u from %02x:%02x:%02x:%02x:%02x:%02x\n",
              KindName(kind), fc, frame->len,
              src[0], src[1], src[2], src[3], src[4], src[5]);

    if (vendor >= 0)
        LogPrintf("  Nintendo element at %d, %d bytes:\n", vendor, d[vendor + 1]);

    int from = ((kind == KIND_BEACON) && (vendor >= 0)) ? vendor : 0;

    for (int i = from; i < frame->stored; i += 16)
    {
        char hex[(16 * 3) + 1];
        int pos = 0;

        for (int j = i; (j < (i + 16)) && (j < frame->stored); j++)
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", d[j]);

        LogPrintf("  %04x  %s\n", i, hex);
    }
}

// Frames repeat constantly, so only the first one from each console is written
// out, plus any later one whose contents differ.
#define SOURCES_MAX     8

typedef struct {
    u8 mac[6];
    u8 kind;
    u16 stored;
    u8 data[FRAME_BYTES];
    bool used;
} KnownSource;

static KnownSource sources[SOURCES_MAX];

static bool IsWorthLogging(const CapturedFrame *frame)
{
    if (frame->stored < 24)
        return false;

    int kind = ClassifyFrame(frame->data, frame->stored);

    if (kind == KIND_NONE)
        return false;

    // The handshake happens once and every frame of it counts.
    if (kind == KIND_HANDSHAKE)
        return true;

    const u8 *src = frame->data + 10;

    KnownSource *first_free = NULL;

    for (int i = 0; i < SOURCES_MAX; i++)
    {
        KnownSource *s = &sources[i];

        if (!s->used)
        {
            if (first_free == NULL)
                first_free = s;
            continue;
        }

        if ((s->kind != kind) || (memcmp(s->mac, src, 6) != 0))
            continue;

        // Seen before. Only interesting if it says something new.
        if ((s->stored == frame->stored)
            && (memcmp(s->data, frame->data, frame->stored) == 0))
            return false;

        memcpy(s->data, frame->data, frame->stored);
        s->stored = frame->stored;
        return true;
    }

    if (first_free == NULL)
        return true;

    memcpy(first_free->mac, src, 6);
    memcpy(first_free->data, frame->data, frame->stored);
    first_free->stored = frame->stored;
    first_free->kind = kind;
    first_free->used = true;

    return true;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    consoleDemoInit();

    printf("DSWiFi wireless sniffer\n");
    printf("=======================\n\n");

    if (!nitroFSInit(NULL))
    {
        if (!fatInitDefault())
        {
            printf("Failed to initialize storage.\n");
            printf("\nPress START to exit.\n");
            goto wait_exit;
        }
    }

    if (!LogInit())
    {
        printf("Can't write to the card.\n");
        printf("\nPress START to exit.\n");
        goto wait_exit;
    }

    LogPrintf("DSWiFi wireless sniffer log\n");

    if (!Wifi_InitDefault(INIT_ONLY))
    {
        printf("Can't initialize WiFi.\n");
        printf("\nPress START to exit.\n");
        goto wait_exit;
    }

    Wifi_RawSetPacketHandler(PacketHandler);

    // Scan mode keeps the receiver on. Without promiscuous mode it rotates
    // through the channels looking for hosts, which finds them but catches only
    // a sample of what any one of them says; with it, the receiver stays on one
    // channel and accepts every frame, which is what watching an exchange
    // between two other consoles needs.
    Wifi_ScanMode();

    unsigned int written = 0;
    int channel = 0;

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();

        if (keysDown() & KEY_START)
            break;

        // Lock onto one channel to follow a session, or go back to hopping to
        // find one. DS Download Play hosts use 1, 7 and 13.
        if (keysDown() & KEY_A)
        {
            static const int channels[] = { 0, 1, 7, 13 };

            channel = channels[(channel == 0) ? 1
                             : (channel == 1) ? 2
                             : (channel == 7) ? 3 : 0];

            if (channel == 0)
            {
                Wifi_SetPromiscuousMode(0);
            }
            else
            {
                Wifi_SetChannel(channel);
                Wifi_SetPromiscuousMode(1);
            }

            LogPrintf("\n[channel] %s\n",
                      (channel == 0) ? "hopping, beacons only" : "locked");
            if (channel != 0)
                LogPrintf("[channel] %d, capturing everything\n", channel);
        }

        while (frames_tail != frames_head)
        {
            CapturedFrame *frame = &frames[frames_tail];

            CountFrame(frame);

            if (IsWorthLogging(frame))
            {
                LogFrame(frame);
                written++;
            }

            frames_tail = (frames_tail + 1) % FRAME_SLOTS;
        }

        LogFlush();

        consoleClear();
        printf("DSWiFi wireless sniffer\n");
        printf("=======================\n\n");
        printf("Log:     %s\n\n", LogPath());
        printf("Seen:    %u\n", frames_seen);
        printf("Written: %u\n", written);
        printf("Lost:    %u\n", frames_lost);

        // If these stay at zero while a transfer is running, the capture isn't
        // reaching the exchange and there is no point letting it run.
        printf("Bcn:%u Cmd:%u Rep:%u Ack:%u\n",
               kind_count[KIND_BEACON], kind_count[KIND_CMD],
               kind_count[KIND_REPLY], kind_count[KIND_ACK]);
        if (LogDropped() != 0)
            printf("Log lost %u chars\n", LogDropped());

        if (channel == 0)
            printf("\nChannel: hopping (beacons)\n");
        else
            printf("\nChannel: %d (everything)\n", channel);

        printf("\nStart the other console now.\n");
        printf("\nA: Channel  START: Stop\n");
    }

    LogPrintf("\n[stop] %u frames seen, %u written\n", frames_seen, written);
    LogPrintf("[stop] beacons %u, cmd %u, reply %u, ack %u, mgmt %u\n",
              kind_count[KIND_BEACON], kind_count[KIND_CMD],
              kind_count[KIND_REPLY], kind_count[KIND_ACK],
              kind_count[KIND_HANDSHAKE]);
    LogClose();

    Wifi_IdleMode();

    return 0;

wait_exit:
    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START)
            break;
    }

    return 0;
}
