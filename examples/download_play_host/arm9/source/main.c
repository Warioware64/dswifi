// SPDX-License-Identifier: CC0-1.0
//
// SPDX-FileContributor: Antonio Niño Díaz, 2025

// This example turns the console into a DS Download Play host. It looks for NDS
// ROMs in the storage of the console, and sends the selected one to any console
// that picks it from its Download Play menu.
//
// Note that consoles refuse to boot programs that aren't signed by Nintendo, so
// homebrew sent this way only boots on consoles with FlashMe.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>

#include <nds.h>
#include <fat.h>
#include <filesystem.h>

#include <dswifi9.h>
#include <dswifi_dlplay.h>
#include <dswifi_version.h>

#include "log.h"

// Bumped by hand whenever a test ROM is cut, so that the screen says which
// build actually booted.
#define BUILD_TAG   "station-mc-6"

// Players the program on offer is announced as supporting, counting the host.
#define STATION_MAX_PLAYERS 8

#define MAX_ROMS        32
#define MAX_NAME_LEN    64

static char rom_names[MAX_ROMS][MAX_NAME_LEN];
static int num_roms = 0;

static const char *search_paths[] = { "/", "/nds", "/roms" };

static void ScanDirectory(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL)
        return;

    struct dirent *entry;

    while ((num_roms < MAX_ROMS) && ((entry = readdir(dir)) != NULL))
    {
        size_t len = strlen(entry->d_name);
        if (len < 5)
            continue;

        // Only regular files with the .nds extension
        if (strcasecmp(entry->d_name + len - 4, ".nds") != 0)
            continue;

        snprintf(rom_names[num_roms], MAX_NAME_LEN, "%s%s%s", path,
                 (strcmp(path, "/") == 0) ? "" : "/", entry->d_name);
        num_roms++;
    }

    closedir(dir);
}

// Reads a whole file to a newly allocated buffer.
static void *LoadFile(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if ((size <= 0) || (size > DSWIFI_DLPLAY_MAX_PROGRAM_SIZE))
    {
        fclose(f);
        return NULL;
    }

    void *buffer = malloc(size);
    if (buffer == NULL)
    {
        fclose(f);
        return NULL;
    }

    if (fread(buffer, 1, size, f) != (size_t)size)
    {
        free(buffer);
        fclose(f);
        return NULL;
    }

    fclose(f);

    *out_size = size;
    return buffer;
}

// The status display clears itself every frame, so the log from the ARM7 gets a
// console of its own on the main screen. Without consoleArm7Setup() libnds throws
// away every ARM7 log line, which is why the wireless log has never been visible
// in this example.
static PrintConsole arm7_console;

// Copies the ARM7 log into the log file as it is drawn. This runs from the
// interrupt that drains the log, so it only appends to a buffer. Returning false
// leaves the console to draw the character as usual.
static bool Arm7ConsoleTee(void *con, char c)
{
    (void)con;

    LogPutChar(c);
    return false;
}

// consoleInit() makes the console it sets up the current one, so the caller has
// to pass in the console to go back to. Asking consoleSelect() for the "previous"
// console here would just return arm7_console itself, and every status line would
// end up on the top screen next to the wireless log.
static void SetupArm7Console(PrintConsole *status_console)
{
    videoSetMode(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);

    consoleInit(&arm7_console, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);

    if (consoleArm7Setup(&arm7_console, 1024) == 0)
    {
        arm7_console.PrintChar = Arm7ConsoleTee;

        printf("ARM7 wireless log\n");
        printf("-----------------\n");
    }

    consoleSelect(status_console);
}

static const char *RotateSkipName(int reason)
{
    switch (reason)
    {
        case DSWIFI_BEACON_ROTATE_OK:
            return "ok";
        case DSWIFI_BEACON_ROTATE_NO_VENDOR_IE:
            return "no vendor IE";
        case DSWIFI_BEACON_ROTATE_BEACON_OFF:
            return "beacon off";
        case DSWIFI_BEACON_ROTATE_NO_FRAGMENTS:
            return "no fragments";
        default:
            return "size mismatch";
    }
}

static const char *MpDropName(int reason)
{
    switch (reason)
    {
        case DSWIFI_MP_DROP_NO_HANDLER:
            return "NoHandler";
        case DSWIFI_MP_DROP_TOO_SHORT:
            return "Short";
        case DSWIFI_MP_DROP_FRAME_TYPE:
            return "FrameType";
        case DSWIFI_MP_DROP_REPLY_MAC:
            return "ReplyMAC";
        case DSWIFI_MP_DROP_DEST_MAC:
            return "DestMAC";
        default:
            return "AIDMismatch";
    }
}

static const char *DropName(Wifi_DlPlayDropReason reason)
{
    switch (reason)
    {
        case WIFI_DLPLAY_DROP_NOT_REPLY:
            return "NotReply";
        case WIFI_DLPLAY_DROP_INACTIVE:
            return "Inactive";
        case WIFI_DLPLAY_DROP_BAD_AID:
            return "BadAID";
        case WIFI_DLPLAY_DROP_BAD_LEN:
            return "BadLen";
        default:
            return "BadOp";
    }
}

static const char *ReplyName(Wifi_DlPlayReplyKind kind)
{
    switch (kind)
    {
        case WIFI_DLPLAY_REPLY_DUMMY:
            return "dum";
        case WIFI_DLPLAY_REPLY_FILEREQ:
            return "req";
        case WIFI_DLPLAY_REPLY_ACCEPT_FILEINFO:
            return "acc";
        case WIFI_DLPLAY_REPLY_CONTINUE:
            return "cont";
        case WIFI_DLPLAY_REPLY_STOPREQ:
            return "stop";
        default:
            return "boot";
    }
}

// Prints the counters and the last captured reply frame. This is what says how
// far a client got: whether it associated, whether it ever replied, and what its
// replies actually look like on the wire.
static void PrintDiagnostics(void)
{
    printf("Ready:%d Rx:%u Tx:%u Lost:%u\n",
           Wifi_LibraryModeReady() ? 1 : 0,
           (unsigned int)Wifi_GetStats(WSTAT_RXPACKETS),
           (unsigned int)Wifi_GetStats(WSTAT_TXPACKETS),
           (unsigned int)Wifi_GetStats(WSTAT_RXQUEUEDLOST));

    // Beacons sent against fragments written. If the first climbs and the second
    // doesn't, clients only ever see one piece of the game info and can never
    // put it back together.
    int pre_tbtt, writes, skip;
    Wifi_BeaconGetRotateStatus(&pre_tbtt, &writes, &skip);

    printf("Beacon: %d sent, %d frags, %s\n", pre_tbtt, writes,
           RotateSkipName(skip));

    // Clients that authenticated but never associated show up here but not in
    // the connected count, which separates "association failed" from "nobody
    // ever showed up".
    Wifi_ConnectedClient clients[4];
    int num = Wifi_MultiplayerGetClients(4, clients);

    printf("Clients: %d assoc, %d seen\n", Wifi_MultiplayerGetNumClients(), num);

    // Only the first two fit on screen. The log has all of them.
    for (int i = 0; (i < num) && (i < 2); i++)
    {
        const u8 *mac = (const u8 *)clients[i].macaddr;

        printf(" %02x%02x%02x%02x%02x%02x aid%d %s\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
               clients[i].association_id,
               (clients[i].state == WIFI_CLIENT_ASSOCIATED) ? "assoc" : "auth");
    }

    // The decisive numbers. "nak" is transfers the hardware gave up on, which for
    // a CMD frame means the replies it polled for never came, and "Rep" counts
    // replies before anything can filter them.
    Wifi_MultiplayerHostCounters mp_cnt;
    Wifi_MultiplayerGetHostCounters(&mp_cnt);

    const Wifi_DlPlayDiag *diag = Wifi_DlPlayGetDiag();

    printf("CMD:%u+%ur done:%u nak:%u\n", mp_cnt.cmd_armed, mp_cnt.cmd_retry,
           mp_cnt.cmd_done, mp_cnt.cmd_failed);
    printf("Rep:%u empty:%u TxQFail:%u\n", mp_cnt.reply_rx, mp_cnt.reply_empty,
           diag->cmd_tx_failures);

    const Wifi_MultiplayerRxDiag *mp = Wifi_MultiplayerGetRxDiag();

    printf("MP ok:%u", mp->accepted);
    for (int i = 0; i < DSWIFI_MP_DROP_COUNT; i++)
    {
        if (mp->drops[i] != 0)
            printf(" %s=%u", MpDropName(i), mp->drops[i]);
    }
    printf("\n");

    printf("DL ok:%u", diag->replies_handled);
    for (int i = 0; i < WIFI_DLPLAY_DROP_COUNT; i++)
    {
        if (diag->drops[i] != 0)
            printf(" %s=%u", DropName(i), diag->drops[i]);
    }
    printf("\n");

    // What the client is actually saying. A client that only ever sends dummies
    // is listening and synchronised but hasn't taken anything the host offered,
    // which reads the same way as a client that isn't there unless the kinds are
    // counted apart.
    printf("Say:");
    for (int i = 0; i < WIFI_DLPLAY_REPLY_KIND_COUNT; i++)
    {
        if (diag->replies[i] != 0)
            printf(" %s=%u", ReplyName(i), diag->replies[i]);
    }
    printf("\n");

    const Wifi_DlPlayClientRequest *req = Wifi_DlPlayGetClientRequest();

    if (req->valid)
    {
        printf("Req: gid%08x f%u v%u c%u\n", (unsigned int)req->game_id,
               req->file_id, req->version, req->favorite_color);
    }
    else if (diag->request_pieces != 0)
    {
        printf("Req: pieces %02x of 1f\n", diag->request_pieces);
    }

    if (diag->replies[WIFI_DLPLAY_REPLY_CONTINUE] != 0)
        printf("Want:%u got:%u\n", diag->block_requested, diag->blocks_received);

    // Anything here means a console running the Download Station program is
    // talking to us on its own ports, asking for files by name.
    if (diag->station_frames != 0)
    {
        printf("Stn: %u on port %u\n", diag->station_frames, diag->station_port);
        printf("  \"%.8s\"\n", (const char *)diag->station_last);
    }

    if (diag->capture_total == 0)
        return;

    // Newest entry, one before the one that will be written next.
    int slot = (diag->capture_next + DSWIFI_DLPLAY_CAPTURE_FRAMES - 1)
               % DSWIFI_DLPLAY_CAPTURE_FRAMES;

    printf("Last of %u:", diag->capture_total);
    for (int i = 0; i < diag->capture_len[slot]; i++)
        printf(" %02x", diag->capture[slot][i]);
    printf("\n");
}

static const char *StateName(Wifi_DlPlayState state);

// Writes a line to the log whenever something the diagnostics show has changed.
// Logging every frame would bury the interesting moments and fill the buffer, so
// this only records transitions.
static void LogDiagnostics(Wifi_DlPlayState state, int current, int total)
{
    static bool first = true;
    static Wifi_DlPlayState last_state;
    static int last_clients = -1;
    static int last_seen = -1;
    static u16 last_capture_total;
    static u16 last_replies;
    static u16 last_drops[WIFI_DLPLAY_DROP_COUNT];
    static int last_block = -1;
    static int last_pre_tbtt = 0;
    static int last_skip = -1;
    static u8 last_cmd[DSWIFI_DLPLAY_CAPTURE_BYTES];
    static u8 last_cmd_stored;
    static int last_cmd_armed = 0;
    static int last_reply_rx = -1;
    static u16 last_mp_drops[DSWIFI_MP_DROP_COUNT];
    static u16 last_mp_accepted;
    static u16 last_tx_fail;
    static u16 last_station_frames;
    static u16 last_station_chunks;
    static u8 last_station_end;
    static u8 last_station_read_fail;
    static u8 last_gave_up[WIFI_DLPLAY_GAVE_UP_COUNT];
    static Wifi_DlPlayState last_client_states[16];
    static unsigned int cmd_skipped;
    static unsigned int last_logged_dropped;
    static u16 last_reply_kinds[WIFI_DLPLAY_REPLY_KIND_COUNT];
    static u8 last_request_pieces;
    static bool last_request_valid;
    static int last_block_requested = -1;

    const Wifi_DlPlayDiag *diag = Wifi_DlPlayGetDiag();

    Wifi_ConnectedClient clients[4];
    int seen = Wifi_MultiplayerGetClients(4, clients);
    int connected = Wifi_MultiplayerGetNumClients();

    if (first)
    {
        first = false;
        last_state = state;
        LogPrintf("[start] state=%s ready=%d\n", StateName(state),
                  Wifi_LibraryModeReady() ? 1 : 0);
    }

    // Report the beacon once a second. It is the only thing that moves while
    // nobody is connected, and it is what the current gate depends on.
    int pre_tbtt, writes, skip;
    Wifi_BeaconGetRotateStatus(&pre_tbtt, &writes, &skip);

    if ((pre_tbtt - last_pre_tbtt >= 60) || (skip != last_skip))
    {
        LogPrintf("[beacon] %d sent, %d fragments written, last=%s\n",
                  pre_tbtt, writes, RotateSkipName(skip));
        last_pre_tbtt = pre_tbtt;
        last_skip = skip;
    }

    // Say so in the file when lines have been lost. Without this a gap looks
    // like nothing having happened, which is how a transfer that overflowed the
    // buffer used to read.
    if (LogDropped() != last_logged_dropped)
    {
        last_logged_dropped = LogDropped();
        LogPrintf("[log] %u characters lost\n", last_logged_dropped);
    }

    // Each console's own progress through the exchange. The summary line below
    // reports the one that has got furthest, which says nothing about a client
    // that is lagging or stuck.
    for (int aid = 1; aid <= 15; aid++)
    {
        Wifi_DlPlayState client_state = Wifi_DlPlayGetClientState(aid);

        if (client_state == last_client_states[aid])
            continue;

        const char *name = Wifi_DlPlayGetClientNameByAID(aid);

        LogPrintf("[client %d] %s -> %s%s%s\n", aid,
                  StateName(last_client_states[aid]), StateName(client_state),
                  (name != NULL) ? " " : "", (name != NULL) ? name : "");

        last_client_states[aid] = client_state;
    }

    if (state != last_state)
    {
        LogPrintf("[state] %s -> %s\n", StateName(last_state), StateName(state));
        last_state = state;
    }

    if ((connected != last_clients) || (seen != last_seen))
    {
        LogPrintf("[clients] %d associated, %d known\n", connected, seen);

        for (int i = 0; i < seen; i++)
        {
            const u8 *mac = (const u8 *)clients[i].macaddr;

            LogPrintf("  %02x:%02x:%02x:%02x:%02x:%02x aid=%d %s\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                      clients[i].association_id,
                      (clients[i].state == WIFI_CLIENT_ASSOCIATED) ? "associated"
                                                                   : "authenticated");
        }

        last_clients = connected;
        last_seen = seen;
    }

    for (int i = 0; i < WIFI_DLPLAY_DROP_COUNT; i++)
    {
        if (diag->drops[i] != last_drops[i])
        {
            LogPrintf("[drop] %s now %u\n", DropName(i), diag->drops[i]);
            last_drops[i] = diag->drops[i];
        }
    }

    // Every frame of a working transfer is answered, so reporting each one
    // fills the log long before anything interesting happens. The first few
    // still matter: they are what says whether a client engaged at all.
    if ((diag->replies_handled != last_replies)
        && ((diag->replies_handled <= 16) || ((diag->replies_handled % 100) == 0)))
    {
        LogPrintf("[reply] handled %u\n", diag->replies_handled);
        last_replies = diag->replies_handled;
    }

    // What kind of message the client sent, rather than just how many. The first
    // of each kind is the interesting one: it is the moment the client stops
    // listening politely and takes another step. Once a transfer is running one
    // arrives per frame, so they are thinned out after that.
    for (int i = 0; i < WIFI_DLPLAY_REPLY_KIND_COUNT; i++)
    {
        u16 count = diag->replies[i];

        if (count == last_reply_kinds[i])
            continue;

        if ((count <= 8) || ((count % 100) == 0))
            LogPrintf("[say] %s now %u\n", ReplyName(i), count);

        last_reply_kinds[i] = count;
    }

    // What a console running the Download Station program is asking for. The
    // name of the file is in the first bytes of the request, so the log shows
    // which one it wants before anything is built to serve it.
    if (diag->station_frames != last_station_frames)
    {
        char hex[(DSWIFI_DLPLAY_CAPTURE_BYTES * 3) + 1];
        int pos = 0;

        for (int i = 0; i < diag->station_last_len; i++)
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", diag->station_last[i]);

        LogPrintf("[station] %u frames, port %u, \"%.8s\"\n",
                  diag->station_frames, diag->station_port,
                  (const char *)diag->station_last);
        LogPrintf("[station] %s\n", hex);

        // What was made of that request: the stream it named, the port that
        // means, and whether the application had anything under that name. A
        // request that is refused leaves the port at zero, which is the
        // difference between "nothing was asked for" and "nothing was offered".
        LogPrintf("[station] aid %u name=\"%.8s\" stream=%u port=%u\n",
                  diag->station_aid, (const char *)diag->station_name,
                  diag->station_stream, diag->station_port_used);
        LogPrintf("[station] size=%u bad=%u missing=%u\n",
                  (unsigned int)diag->station_size, diag->station_bad_stream,
                  diag->station_not_found);

        last_station_frames = diag->station_frames;
    }

    // Chunks going out, reported the way blocks of the program are, plus the two
    // that say how a transfer ended.
    //
    // The short last chunk and the end marker used to be reported only because
    // they changed the frame being sent, and a run where neither appeared left
    // no way to tell an unfinished transfer from a client that walked away at
    // the end. These say it outright.
    if (diag->station_chunks != last_station_chunks)
    {
        // A short chunk is not the test for the last one: a full chunk is 30
        // bytes on one port and 126 on the other, so the length alone says
        // nothing. The library says outright.
        bool last = diag->station_last_is_final != 0;

        if ((diag->station_chunks != 0)
            && (last || ((diag->station_chunks % 32) == 0)))
        {
            LogPrintf("[station] aid %u chunk %u index %u len %u of %u bytes%s\n",
                      diag->station_aid, diag->station_chunks,
                      diag->station_last_index, diag->station_last_chunk_len,
                      (unsigned int)diag->station_size, last ? " (last)" : "");
        }

        last_station_chunks = diag->station_chunks;
    }

    // Only when it happens, not when the counter is cleared for the next file.
    if ((diag->station_end_sent != last_station_end) && diag->station_end_sent)
    {
        LogPrintf("[station] aid %u end marker sent after %u chunks\n",
                  diag->station_aid, diag->station_chunks);
    }

    last_station_end = diag->station_end_sent;

    // Consoles the host stopped waiting for. Nothing reported these, so a client
    // dropped by a timeout looked exactly like one that walked away -- which is
    // what made a bound firing on a healthy transfer take two runs to find.
    for (int i = 0; i < WIFI_DLPLAY_GAVE_UP_COUNT; i++)
    {
        if (diag->gave_up[i] == last_gave_up[i])
            continue;

        LogPrintf("[gaveup] aid %u %s (now %u)\n", diag->gave_up_last_aid,
                  (i == WIFI_DLPLAY_GAVE_UP_SILENT) ? "stopped answering"
                                                    : "stopped getting anywhere",
                  diag->gave_up[i]);

        last_gave_up[i] = diag->gave_up[i];
    }

    if (diag->station_read_failures != last_station_read_fail)
    {
        LogPrintf("[station] %u reads refused by the content source\n",
                  diag->station_read_failures);
        last_station_read_fail = diag->station_read_failures;
    }

    if (diag->request_pieces != last_request_pieces)
    {
        LogPrintf("[req] pieces %02x of 1f\n", diag->request_pieces);
        last_request_pieces = diag->request_pieces;
    }

    const Wifi_DlPlayClientRequest *req = Wifi_DlPlayGetClientRequest();

    if (req->valid != last_request_valid)
    {
        last_request_valid = req->valid;

        if (req->valid)
        {
            const char *name = Wifi_DlPlayGetClientName();

            LogPrintf("[req] game=%08x file=%u version=%u color=%u player=%u\n",
                      (unsigned int)req->game_id, req->file_id, req->version,
                      req->favorite_color, req->player_no);
            LogPrintf("[req] name=\"%s\" (%u chars)%s\n",
                      (name != NULL) ? name : "", req->name_len,
                      req->is_station ? " -- Download Station client" : "");
        }
    }

    // How the transfer is really going: the block the client is asking for and
    // how many it says it holds, which is the only honest measure of progress.
    // It moves every frame, so report the first few and then every 64th.
    if (diag->block_requested != last_block_requested)
    {
        if ((diag->block_requested <= 4) || ((diag->block_requested % 64) == 0))
        {
            LogPrintf("[want] block %u, client has %u\n", diag->block_requested,
                      diag->blocks_received);
        }

        last_block_requested = diag->block_requested;
    }

    // Report the frame being sent whenever it changes. It is what the client is
    // reacting to, or failing to, and it is worth being able to read it back
    // rather than trusting that the code builds what it is meant to.
    if ((diag->last_cmd_stored != last_cmd_stored)
        || (memcmp(diag->last_cmd, last_cmd, diag->last_cmd_stored) != 0))
    {
        // Every block of a program is a different frame, so reporting each one
        // is a line per frame and fills the buffer long before the transfer
        // ends. What matters is when the host starts saying a different kind of
        // thing.
        //
        // The size and the port say that much on their own. The third byte only
        // joins them on the multiboot port, where it is the command: on a
        // station port it is the low byte of the chunk number, which changes
        // every frame -- so including it there defeated the throttle completely
        // and put a line per frame in the log for the whole of every content
        // transfer, which is enough writing to the card to cost the host frames.
        bool kind_changed = (diag->last_cmd_stored < 2)
                         || (last_cmd_stored < 2)
                         || (diag->last_cmd[0] != last_cmd[0])
                         || (diag->last_cmd[1] != last_cmd[1]);

        bool multiboot = (diag->last_cmd_stored >= 3) && (last_cmd_stored >= 3)
                      && ((diag->last_cmd[1] & 0xF) == 1);

        if (multiboot && (diag->last_cmd[2] != last_cmd[2]))
            kind_changed = true;

        cmd_skipped++;

        if (kind_changed || (cmd_skipped >= 60))
        {
            char hex[(DSWIFI_DLPLAY_CAPTURE_BYTES * 3) + 1];
            int pos = 0;

            for (int i = 0; i < diag->last_cmd_stored; i++)
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", diag->last_cmd[i]);

            LogPrintf("[cmd] len=%u %s\n", diag->last_cmd_len, hex);
            cmd_skipped = 0;
        }

        memcpy(last_cmd, diag->last_cmd, diag->last_cmd_stored);
        last_cmd_stored = diag->last_cmd_stored;
    }

    if (diag->cmd_tx_failures != last_tx_fail)
    {
        LogPrintf("[txfail] %u frames could not be queued\n", diag->cmd_tx_failures);
        last_tx_fail = diag->cmd_tx_failures;
    }

    // The transmit and receive counters of the multiplayer layer. A reply that is
    // received but discarded looks nothing like one that never arrived, and only
    // these tell them apart.
    Wifi_MultiplayerHostCounters mp_cnt;
    Wifi_MultiplayerGetHostCounters(&mp_cnt);

    if ((mp_cnt.cmd_armed - last_cmd_armed >= 300)
        || (mp_cnt.reply_rx - (unsigned int)last_reply_rx >= 100)
        || (((int)mp_cnt.reply_rx != last_reply_rx) && (mp_cnt.reply_rx <= 16)))
    {
        LogPrintf("[mp] cmd %u sent + %u retried, %u done, %u failed, "
                  "%u replies, %u empty\n",
                  mp_cnt.cmd_armed, mp_cnt.cmd_retry, mp_cnt.cmd_done,
                  mp_cnt.cmd_failed, mp_cnt.reply_rx, mp_cnt.reply_empty);
        last_cmd_armed = mp_cnt.cmd_armed;
        last_reply_rx = mp_cnt.reply_rx;
    }

    const Wifi_MultiplayerRxDiag *mp = Wifi_MultiplayerGetRxDiag();

    if ((mp->accepted != last_mp_accepted)
        && ((mp->accepted <= 16) || ((mp->accepted % 100) == 0)))
    {
        LogPrintf("[mp] %u frames accepted\n", mp->accepted);
        last_mp_accepted = mp->accepted;
    }

    for (int i = 0; i < DSWIFI_MP_DROP_COUNT; i++)
    {
        // Only report every 100th one. A client that answers every frame with an
        // empty reply produces one of these per frame, which would bury
        // everything else in the log.
        if (mp->drops[i] < last_mp_drops[i] + 100)
            continue;

        last_mp_drops[i] = mp->drops[i];

        char hex[(DSWIFI_MP_DIAG_BYTES * 3) + 1];
        int pos = 0;

        for (int j = 0; j < mp->last_drop_len; j++)
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", mp->last_drop_frame[j]);

        LogPrintf("[mpdrop] %s now %u, last frame: %s\n",
                  MpDropName(i), mp->drops[i], hex);
    }

    // The first frames a client sends are what says how far it got, so they are
    // copied out of the capture buffer as they arrive. Once a transfer is really
    // running every frame is answered, and writing them all out buries
    // everything else, so stop after the opening exchange.
    //
    // The total is read once because it is bumped from an interrupt.
    u16 capture_total = diag->capture_total;

    if (capture_total > 32)
    {
        if (last_capture_total <= 32)
            LogPrintf("[capture] transfer running, no longer recording\n");

        last_capture_total = capture_total;
    }

    while (last_capture_total != capture_total)
    {
        u16 behind = capture_total - last_capture_total;

        // Anything older than the buffer has already been overwritten.
        if (behind > DSWIFI_DLPLAY_CAPTURE_FRAMES)
        {
            LogPrintf("[capture] %u frames not recorded\n",
                      behind - DSWIFI_DLPLAY_CAPTURE_FRAMES);
            last_capture_total = capture_total - DSWIFI_DLPLAY_CAPTURE_FRAMES;
            continue;
        }

        int slot = (diag->capture_next + DSWIFI_DLPLAY_CAPTURE_FRAMES - behind)
                   % DSWIFI_DLPLAY_CAPTURE_FRAMES;

        char hex[(DSWIFI_DLPLAY_CAPTURE_BYTES * 3) + 1];
        int len = diag->capture_len[slot];
        int pos = 0;

        for (int i = 0; i < len; i++)
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", diag->capture[slot][i]);

        LogPrintf("[capture] #%u len=%d %s\n",
                  (unsigned int)(last_capture_total + 1), len, hex);

        last_capture_total++;
    }

    if ((state == WIFI_DLPLAY_SENDING) && (current != last_block))
    {
        // Blocks go by quickly, so only note every 32nd one plus the last.
        if (((current & 31) == 0) || (current == total))
            LogPrintf("[block] %d / %d\n", current, total);

        last_block = current;
    }
}

static const char *StateName(Wifi_DlPlayState state)
{
    switch (state)
    {
        case WIFI_DLPLAY_IDLE:
            return "Waiting for a client";
        case WIFI_DLPLAY_CONNECTING:
            return "Client connected";
        case WIFI_DLPLAY_VERIFYING:
            return "Checking signature";
        case WIFI_DLPLAY_SENDING:
            return "Sending program";
        case WIFI_DLPLAY_BOOTING:
            return "Client is booting";
        case WIFI_DLPLAY_STATION:
            return "Download Station client";
        default:
            return "Error";
    }
}

// Lets the user pick one of the ROMs that have been found.
//
// Returns the index of the entry chosen, -1 if the user backed out, or -2 to
// ask for the Download Station instead, which the first of the two calls offers.
#define SELECT_CANCELLED    (-1)
#define SELECT_STATION      (-2)

static int SelectRom(const char *prompt, bool offer_station)
{
    int selected = 0;

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();

        uint16_t keys = keysDown();

        if (keys & KEY_UP)
            selected = (selected == 0) ? (num_roms - 1) : (selected - 1);
        if (keys & KEY_DOWN)
            selected = (selected + 1) % num_roms;
        if (keys & KEY_A)
            return selected;
        if (keys & KEY_X && offer_station)
            return SELECT_STATION;
        if (keys & KEY_START)
            return SELECT_CANCELLED;

        consoleClear();
        printf("DSWiFi Download Play host\n");
        printf("=========================\n\n");
        printf("%s\n\n", prompt);

        // Only show a few entries at a time so that they fit on the screen
        int first = selected - 8;
        if (first < 0)
            first = 0;

        for (int i = first; (i < num_roms) && (i < (first + 14)); i++)
            printf("%c %.28s\n", (i == selected) ? '>' : ' ', rom_names[i]);

        printf("\nUP/DOWN: Select  A: Choose\n");
        if (offer_station)
            printf("X: Download Station mode\n");
        printf("START: Exit\n");
    }
}

// Finds the menu blob a Download Station program displays.
//
// It isn't a ROM, so it isn't in the list, and its name isn't ours to choose:
// the station program asks for "demomenu" by name. It is looked for in the same
// places the ROMs are.
// Reading a Download Station container


#define NDS_HDR_FNT_OFFSET      0x40
#define NDS_HDR_FNT_SIZE        0x44
#define NDS_HDR_FAT_OFFSET      0x48
#define NDS_HDR_FAT_SIZE        0x4C

// Bytes kept from the file being served. Refilled when a read falls outside it,
// which for the menu is once every few hundred chunks and for a program once
// every few dozen. A card read blocks for long enough to disturb the wireless
// frame timing, so the point is to do very few of them.
#define STATION_WINDOW_SIZE     (8 * 1024)

typedef struct {
    // The container, kept open for as long as the host runs.
    FILE *file;

    // Where the filesystem tables are, from the ROM header.
    u32 fnt_offset;
    u32 fnt_size;
    u32 fat_offset;
    u32 fat_size;

    // The directory holding the content, found once when the ROM is opened.
    bool valid;
    u16 ds_demo_dir;

    // The window of whichever file was read last. It is shared by every console,
    // so it carries the handle of the file it holds.
    u8 window[STATION_WINDOW_SIZE];
    u32 window_handle;
    u32 window_offset;
    u32 window_len;
} StationRom;

static bool RomReadAt(StationRom *rom, u32 offset, void *buffer, size_t size)
{
    if (fseek(rom->file, offset, SEEK_SET) != 0)
        return false;

    return fread(buffer, 1, size, rom->file) == size;
}

static u16 RomReadU16(StationRom *rom, u32 offset)
{
    u8 v[2];

    if (!RomReadAt(rom, offset, v, sizeof(v)))
        return 0;

    return v[0] | (v[1] << 8);
}

static u32 RomReadU32(StationRom *rom, u32 offset)
{
    u8 v[4];

    if (!RomReadAt(rom, offset, v, sizeof(v)))
        return 0;

    return v[0] | (v[1] << 8) | (v[2] << 16) | ((u32)v[3] << 24);
}

// Looks for a name in one directory of the filesystem.
//
// A directory is eight bytes in the name table: the offset of its list of names,
// the id of its first file, and its parent. The list is a length byte, the name,
// and for a subdirectory the two byte id of what it points at; bit 7 of the
// length says which it is, and a zero byte ends the list. File ids count up from
// the first as the list is walked, and the allocation table gives each one a
// start and an end.
static bool RomFindInDir(StationRom *rom, u16 dir_id, const char *name,
                         size_t name_len, bool want_dir,
                         u16 *out_dir, u32 *out_offset, u32 *out_size)
{
    u32 entry = rom->fnt_offset + ((dir_id & 0xFFF) * 8);

    if ((entry + 8) > (rom->fnt_offset + rom->fnt_size))
        return false;

    u32 pos = rom->fnt_offset + RomReadU32(rom, entry);
    u16 file_id = RomReadU16(rom, entry + 4);

    u32 end = rom->fnt_offset + rom->fnt_size;

    while (pos < end)
    {
        u8 type;

        if (!RomReadAt(rom, pos, &type, 1))
            return false;

        pos++;

        if (type == 0)
            return false;

        bool is_dir = (type & 0x80) != 0;
        size_t len = type & 0x7F;

        char entry_name[128];

        if (len >= sizeof(entry_name))
            return false;

        if (!RomReadAt(rom, pos, entry_name, len))
            return false;

        pos += len;

        u16 sub_id = 0;

        if (is_dir)
        {
            sub_id = RomReadU16(rom, pos);
            pos += 2;
        }

        if ((is_dir == want_dir) && (len == name_len)
            && (memcmp(entry_name, name, name_len) == 0))
        {
            if (is_dir)
            {
                if (out_dir != NULL)
                    *out_dir = sub_id;

                return true;
            }

            u32 fat = rom->fat_offset + (file_id * 8);
            u32 start = RomReadU32(rom, fat);

            if (out_offset != NULL)
                *out_offset = start;

            if (out_size != NULL)
                *out_size = RomReadU32(rom, fat + 4) - start;

            return true;
        }

        if (!is_dir)
            file_id++;
    }

    return false;
}

// Opens a ROM and says whether it is a station container: one with both the
// directory of content and the directory holding the program to send.
static bool StationRomOpen(StationRom *rom, const char *path)
{
    memset(rom, 0, sizeof(*rom));

    rom->file = fopen(path, "rb");
    if (rom->file == NULL)
        return false;

    rom->fnt_offset = RomReadU32(rom, NDS_HDR_FNT_OFFSET);
    rom->fnt_size = RomReadU32(rom, NDS_HDR_FNT_SIZE);
    rom->fat_offset = RomReadU32(rom, NDS_HDR_FAT_OFFSET);
    rom->fat_size = RomReadU32(rom, NDS_HDR_FAT_SIZE);

    if ((rom->fnt_size == 0) || (rom->fat_size == 0))
    {
        fclose(rom->file);
        rom->file = NULL;
        return false;
    }

    if (!RomFindInDir(rom, 0, "ds_demo", 7, true, &rom->ds_demo_dir, NULL, NULL)
        || !RomFindInDir(rom, 0, "mb", 2, true, NULL, NULL, NULL))
    {
        fclose(rom->file);
        rom->file = NULL;
        return false;
    }

    rom->valid = true;
    return true;
}

// Reads the program the container sends by Download Play into memory. It is the
// one thing that does have to be resident, because it is sent to every client.
static void *StationRomLoadProgram(StationRom *rom, size_t *out_size)
{
    u16 mb_dir;
    u32 offset = 0;
    u32 size = 0;

    if (!RomFindInDir(rom, 0, "mb", 2, true, &mb_dir, NULL, NULL))
        return NULL;

    if (!RomFindInDir(rom, mb_dir, "ds_demo_client.srl", 18, false, NULL,
                      &offset, &size))
        return NULL;

    if ((size == 0) || (size > DSWIFI_DLPLAY_MAX_PROGRAM_SIZE))
        return NULL;

    void *buffer = malloc(size);
    if (buffer == NULL)
        return NULL;

    if (!RomReadAt(rom, offset, buffer, size))
    {
        free(buffer);
        return NULL;
    }

    *out_size = size;
    return buffer;
}

// The two halves of the content source DSWiFi asks. The first resolves a name
// the way a kiosk does, the second hands over the bytes of each frame.
static bool StationRomOpenContent(const char *name, size_t name_len,
                                  size_t *size, u32 *handle, void *arg)
{
    StationRom *rom = arg;

    if (!rom->valid)
        return false;

    u32 offset = 0;
    u32 content_size = 0;

    if (!RomFindInDir(rom, rom->ds_demo_dir, name, name_len, false, NULL,
                      &offset, &content_size))
        return false;

    // Where the file starts in the container. Several consoles can be taking
    // different files at once, so this travels with every read rather than
    // being remembered as "the one opened last".
    *handle = offset;
    *size = content_size;

    return true;
}

static bool StationRomReadContent(u32 handle, size_t offset, void *buffer,
                                  size_t size, void *arg)
{
    StationRom *rom = arg;

    if (!rom->valid)
        return false;

    // Refill when what is being asked for isn't in the window, which now
    // includes a read for a different file: the window is one buffer shared by
    // every console, so two of them taking different content make it change
    // hands. That costs a card read each time they alternate, which is the
    // simple price of not keeping one window per console.
    if ((rom->window_len == 0)
        || (handle != rom->window_handle)
        || (offset < rom->window_offset)
        || ((offset + size) > (rom->window_offset + rom->window_len)))
    {
        u32 len = STATION_WINDOW_SIZE;

        if (!RomReadAt(rom, handle + offset, rom->window, len))
        {
            // A short read at the end of a file is not a failure. Try again for
            // just what was asked for.
            if (!RomReadAt(rom, handle + offset, rom->window, size))
            {
                rom->window_len = 0;
                return false;
            }

            len = size;
        }

        rom->window_handle = handle;
        rom->window_offset = offset;
        rom->window_len = len;
    }

    memcpy(buffer, rom->window + (offset - rom->window_offset), size);
    return true;
}

// What each console running a program this host sent has last said.
//
// The handler runs in an interrupt and must not call anything that takes a lock,
// so it does nothing but copy. The main loop prints it.
static char app_messages[16][DSWIFI_DLPLAY_APP_MAX_SIZE + 1];
static volatile u16 app_message_mask;

static void AppHandler(int aid, const void *data, size_t size, void *arg)
{
    (void)arg;

    if ((aid < 1) || (aid > 15))
        return;

    if (size > DSWIFI_DLPLAY_APP_MAX_SIZE)
        size = DSWIFI_DLPLAY_APP_MAX_SIZE;

    memcpy(app_messages[aid], data, size);
    app_messages[aid][size] = '\0';

    app_message_mask |= 1 << aid;
}


typedef struct {
    const void *menu;
    size_t menu_size;
    const void *file;
    size_t file_size;

} StationContent;

// Which of the two this host offers. The handle is the index, so two consoles
// taking different ones never read from each other's.
#define STATION_HANDLE_MENU 0
#define STATION_HANDLE_FILE 1

static bool StationOpen(const char *name, size_t name_len, size_t *size,
                        u32 *handle, void *arg)
{
    const StationContent *content = arg;

    // The name is a fixed eight byte field, not a string: compare all of it.
    if ((name_len == 8) && (memcmp(name, "demomenu", 8) == 0))
    {
        if (content->menu == NULL)
            return false;

        *handle = STATION_HANDLE_MENU;
        *size = content->menu_size;

        return true;
    }

    if (content->file == NULL)
        return false;

    *handle = STATION_HANDLE_FILE;
    *size = content->file_size;

    return true;
}

static bool StationRead(u32 handle, size_t offset, void *buffer, size_t size,
                        void *arg)
{
    const StationContent *content = arg;

    const void *data = (handle == STATION_HANDLE_MENU) ? content->menu
                                                       : content->file;
    size_t total = (handle == STATION_HANDLE_MENU) ? content->menu_size
                                                   : content->file_size;

    if ((data == NULL) || ((offset + size) > total))
        return false;

    memcpy(buffer, (const u8 *)data + offset, size);
    return true;
}

static void *LoadStationMenu(size_t *out_size)
{
    for (size_t i = 0; i < sizeof(search_paths) / sizeof(search_paths[0]); i++)
    {
        char path[MAX_NAME_LEN];

        snprintf(path, sizeof(path), "%s%sdemomenu.bin", search_paths[i],
                 (search_paths[i][strlen(search_paths[i]) - 1] == '/') ? "" : "/");

        void *menu = LoadFile(path, out_size);
        if (menu != NULL)
            return menu;
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    // Turn a crash into something readable. Without this an exception on the
    // ARM9 leaves the screen frozen while the ARM7 carries on beaconing, which
    // looks exactly like a hang and says nothing about where it happened.
    defaultExceptionHandler();

    PrintConsole *status_console = consoleDemoInit();

    printf("Initializing storage...\n");

    // This also initializes FAT if the console has a way to access a card.
    if (!nitroFSInit(NULL))
    {
        if (!fatInitDefault())
        {
            printf("Failed to initialize storage.\n");
            printf("\nPress START to exit.\n");
            goto wait_exit;
        }
    }

    for (size_t i = 0; i < sizeof(search_paths) / sizeof(search_paths[0]); i++)
        ScanDirectory(search_paths[i]);

    if (num_roms == 0)
    {
        printf("No NDS ROMs found.\n\n");
        printf("Copy some to the root of the\n");
        printf("card, or to /nds or /roms.\n");
        printf("\nPress START to exit.\n");
        goto wait_exit;
    }

    // Two ways to run. The plain one sends a program and is done with it. In
    // Download Station mode the program sent is a station client, which boots,
    // connects again, and then asks the host for the content to offer -- so a
    // second program has to be chosen for it to hand out.
    int selected = SelectRom("Select a program:", true);
    if (selected == SELECT_CANCELLED)
        return 0;

    bool station_mode = (selected == SELECT_STATION);
    int station_selected = -1;

    static StationRom station_rom;
    bool container = false;

    if (station_mode)
    {
        selected = SelectRom("Station ROM or program:", false);
        if (selected == SELECT_CANCELLED)
            return 0;

        // A Download Station keeps everything it serves in its own ROM. If the
        // one picked is such a container there is nothing else to ask for: the
        // program to send and the content both come out of it. Anything else
        // falls back to being told the three pieces separately.
        container = StationRomOpen(&station_rom, rom_names[selected]);

        if (!container)
        {
            station_selected = SelectRom("Program it hands out:", false);
            if (station_selected == SELECT_CANCELLED)
                return 0;
        }
    }

    consoleClear();
    printf("Loading %.28s...\n", rom_names[selected]);

    size_t rom_size = 0;
    void *rom;

    if (container)
    {
        // The program a container sends is the one inside it, not the container.
        rom = StationRomLoadProgram(&station_rom, &rom_size);

        if (rom == NULL)
        {
            printf("No /mb/ds_demo_client.srl in\n");
            printf("that ROM, or it is too big.\n");
            printf("\nPress START to exit.\n");
            goto wait_exit;
        }
    }
    else
    {
        rom = LoadFile(rom_names[selected], &rom_size);
    }

    if (rom == NULL)
    {
        printf("Failed to load the program.\n");
        printf("\nPress START to exit.\n");
        goto wait_exit;
    }

    bool signed_rom = Wifi_DlPlayRomIsSigned(rom, rom_size);

    // Content for a Download Station client, offered only in that mode.
    //
    // The client asks for both by name once it is running, so what they are
    // called on the air isn't ours to choose. The menu is the blob the station
    // program displays; the file is the program the user picked above, which the
    // station hands out to whoever chooses it from that menu.
    size_t station_menu_size = 0;
    size_t station_file_size = 0;

    void *station_menu = NULL;
    void *station_file = NULL;

    static StationContent station_content;

    if (station_mode && container)
    {
        // Everything comes out of the container, read as it is sent. Nothing to
        // load: the demos in a retail kiosk reach 2.5 MB and the ROM itself is
        // 16 MB, against the 4 MB this console has.
        Wifi_DlPlayStationSetContentSource(StationRomOpenContent,
                                           StationRomReadContent, &station_rom);
    }
    else if (station_mode)
    {
        printf("Loading station content...\n");

        station_menu = LoadStationMenu(&station_menu_size);
        station_file = LoadFile(rom_names[station_selected], &station_file_size);

        if (station_menu == NULL)
        {
            printf("\nNo demomenu.bin found.\n");
            printf("The station program needs it\n");
            printf("to show anything. Copy it\n");
            printf("next to the ROMs.\n");
            printf("\nPress START to exit.\n");
            goto wait_exit;
        }

        if (station_file == NULL)
        {
            printf("\nFailed to load %.20s\n", rom_names[station_selected]);
            printf("\nPress START to exit.\n");
            goto wait_exit;
        }

        station_content.menu = station_menu;
        station_content.menu_size = station_menu_size;
        station_content.file = station_file;
        station_content.file_size = station_file_size;

        Wifi_DlPlayStationSetContentSource(StationOpen, StationRead,
                                           &station_content);
    }

    printf("DSWiFi %d.%d.%d [%s]\n",
           DSWIFI_MAJOR, DSWIFI_MINOR, DSWIFI_REVISION, BUILD_TAG);
    printf("Initializing WiFi...\n");

    // Do this before starting the library so that the log covers the setup too.
    bool logging = LogInit();

    SetupArm7Console(status_console);

    if (logging)
    {
        LogPrintf("DSWiFi Download Play host log (%s)\n", LogPath());
        LogPrintf("program=%s size=%u signed=%d\n", rom_names[selected],
                  (unsigned int)rom_size, signed_rom ? 1 : 0);
        if (station_mode && container)
        {
            LogPrintf("station mode: container %s\n", rom_names[selected]);
            LogPrintf("  program %u bytes from /mb/ds_demo_client.srl\n",
                      (unsigned int)rom_size);
        }
        else if (station_mode)
        {
            LogPrintf("station mode: menu=%u file=%s size=%u\n",
                      (unsigned int)station_menu_size,
                      rom_names[station_selected],
                      (unsigned int)station_file_size);
        }
        else
        {
            LogPrintf("station mode: off\n");
        }

        // Where the icon and the title in the beacon come from. A banner offset
        // of zero, or one that points past the end of the file, leaves the
        // Download Play menu of the client showing an empty entry.
        const u8 *hdr = rom;
        u32 banner = hdr[0x68] | (hdr[0x69] << 8) | (hdr[0x6A] << 16) | (hdr[0x6B] << 24);

        LogPrintf("banner offset=0x%x", (unsigned int)banner);

        if ((banner == 0) || ((banner + 0x240) > rom_size))
        {
            LogPrintf(" (no banner, icon and title will be empty)\n");
        }
        else
        {
            unsigned int icon_nonzero = 0;
            for (int i = 0; i < 0x220; i++)
            {
                if (((const u8 *)rom)[banner + 0x20 + i] != 0)
                    icon_nonzero++;
            }

            LogPrintf(" icon nonzero bytes=%u\n", icon_nonzero);
        }
        LogFlush();
    }

    if (!Wifi_InitDefault(INIT_ONLY))
    {
        printf("Can't initialize WiFi.\n");
        printf("\nThe ARM7 didn't come up (it\n");
        printf("stopped at stage %d). This\n", Wifi_GetInitFailStage());
        printf("happens when the program was\n");
        printf("started by Download Play.\n");
        printf("\nPress START to exit.\n");
        goto wait_exit;
    }

    // A real console announces a full record: an icon, a title and a description.
    // Ours comes from the banner of the ROM, but plenty of homebrew has no
    // banner at all, and then the client is offered an entry with nothing in it.
    // Fall back to the file name so that there is always something to show.
    u16 title[DSWIFI_DLPLAY_TITLE_LEN];
    u8 title_len = 0;

    const char *name = strrchr(rom_names[selected], '/');
    name = (name != NULL) ? (name + 1) : rom_names[selected];

    while ((title_len < DSWIFI_DLPLAY_TITLE_LEN) && (name[title_len] != '\0'))
    {
        title[title_len] = (u8)name[title_len];
        title_len++;
    }

    static const char *desc = "Sent with DSWiFi";

    u16 description[DSWIFI_DLPLAY_DESC_LEN];
    u8 description_len = 0;

    while ((description_len < DSWIFI_DLPLAY_DESC_LEN) && (desc[description_len] != '\0'))
    {
        description[description_len] = (u8)desc[description_len];
        description_len++;
    }


    static char user_param[DSWIFI_DLPLAY_USER_PARAM_SIZE];

    snprintf(user_param, sizeof(user_param), "sent by %.14s", rom_names[selected]);

    Wifi_DlPlayInfo info =
    {
        // The icon and the name of the host are taken from the ROM and from the
        // settings of the console.
        .icon_bitmap = NULL,
        .icon_palette = NULL,
        .host_name = NULL,
        .title = title,
        .title_len = title_len,
        .description = description,
        .description_len = description_len,

        // How many players the program on offer supports, counting the host.
        // This is what a client shows next to the title in its Download Play
        // menu; it doesn't change how many consoles this host serves at once,
        // which is one.
        .max_players = STATION_MAX_PLAYERS,

        // Handed to the program being sent, which finds it at 0x027FFBE0. The
        // dlplay_child example prints it on arrival.
        .user_param = user_param,
    };

    // Anything a console running this program says comes back here.
    Wifi_DlPlaySetAppHandler(AppHandler, NULL);

    if (Wifi_DlPlayStart(rom, rom_size, &info) != 0)
    {
        printf("Failed to start the host.\n");
        printf("\nThe program may be too big, or\n");
        printf("it may not be able to run\n");
        printf("without its filesystem.\n");
        printf("\nPress START to exit.\n");
        goto wait_exit;
    }

    int flush_timer = 0;

    // Frames between redraws of the status display.
    //
    // Clearing the screen and printing thirty formatted lines takes far longer
    // than the vertical blank it starts in, so the rest of it lands while the
    // display controller is reading the same memory back out: the text tears and
    // flickers, which looks like the video memory has been corrupted. Redrawing
    // a few times a second is enough to read, and it leaves the console free to
    // feed the transfer.
    #define STATUS_REDRAW_FRAMES    8

    int redraw_timer = 0;
    Wifi_DlPlayState last_drawn_state = WIFI_DLPLAY_ERROR;

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();

        uint16_t keys = keysDown();

        if (keys & KEY_START)
            break;

        // Close the room to consoles that aren't in it yet. The ones being
        // served carry on; this is what a retail host does when a game is about
        // to start.
        if (keys & KEY_L)
            Wifi_DlPlayLockRoom(!Wifi_DlPlayRoomLocked());

        // Start everyone holding a finished program. Only does anything in
        // manual mode, which this example leaves off.
        if (keys & KEY_R)
            Wifi_DlPlayBootAll();

        Wifi_DlPlayUpdate();

        // Say something to the programs this host has already delivered. They
        // answer in their own reply slots, and what they said is shown with the
        // rest of each console's row.
        static unsigned int app_tick;

        if (((app_tick++) % 30) == 0)
        {
            char message[24];

            snprintf(message, sizeof(message), "tick %u", app_tick / 30);
            Wifi_DlPlayAppSend(message, strlen(message));
        }

        int current, total;
        Wifi_DlPlayGetProgress(&current, &total);

        Wifi_DlPlayState state = Wifi_DlPlayGetState();

        // Always redraw when the state changes, so nothing important waits.
        redraw_timer++;

        bool redraw = (state != last_drawn_state)
                   || (redraw_timer >= STATUS_REDRAW_FRAMES);

        if (!redraw)
        {
            LogDiagnostics(state, current, total);
            goto after_draw;
        }

        redraw_timer = 0;
        last_drawn_state = state;

        consoleClear();
        printf("DSWiFi Download Play host\n");
        printf("Program: %.22s\n", rom_names[selected]);
        printf("Size:    %u KB\n", (unsigned int)(rom_size / 1024));
        printf("Signed:  %s\n\n", signed_rom ? "Yes" : "No (needs FlashMe)");

        printf("State:   %s\n", StateName(state));

        // A row per console. They take the same stream and keep whichever blocks
        // they are missing, so they are at different points in the transfer and
        // one line for all of them would say very little.
        u16 clients = Wifi_DlPlayGetClientMask();

        if (clients == 0)
        {
            printf("Clients: none\n");
        }
        else
        {
            for (int aid = 1; aid <= 15; aid++)
            {
                if ((clients & (1 << aid)) == 0)
                    continue;

                const char *name = Wifi_DlPlayGetClientNameByAID(aid);

                int done, blocks;
                Wifi_DlPlayGetClientProgress(aid, &done, &blocks);

                printf("%2d %-10.10s %-9s", aid, (name != NULL) ? name : "?",
                       StateName(Wifi_DlPlayGetClientState(aid)));

                if (Wifi_DlPlayGetClientState(aid) == WIFI_DLPLAY_SENDING)
                    printf(" %d/%d", done, blocks);
                else if (app_message_mask & (1 << aid))
                    printf(" %.10s", app_messages[aid]);

                printf("\n");
            }
        }

        if (state == WIFI_DLPLAY_SENDING)
            printf("Stream:  block %d / %d\n", current, total);

        printf("\n");
        PrintDiagnostics();

        if (logging && (LogDropped() != 0))
            printf("Log lost %u chars\n", LogDropped());

        printf("\nSTART: Stop\n");

        LogDiagnostics(state, current, total);

after_draw:
        // Writing to the card blocks for long enough to upset the multiplayer
        // frame timing, so an idle host writes once a second and a transfer
        // waits until it is over.
        //
        // Serving a station is the exception. Its frames are tens of bytes
        // rather than the 514 a block of a program takes, so there is room, and
        // a freeze during this phase would otherwise take every line since the
        // last idle moment with it -- which is exactly what happened to the runs
        // that were meant to explain one.
        flush_timer++;

        bool due = (state == WIFI_DLPLAY_IDLE) ? (flush_timer >= 60)
                 : (state == WIFI_DLPLAY_STATION) ? (flush_timer >= 120)
                 : false;

        if (due)
        {
            flush_timer = 0;
            LogFlush();
        }
    }

    Wifi_DlPlayStop();

    LogPrintf("[stop] user stopped the host\n");
    LogClose();

    free(rom);

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
