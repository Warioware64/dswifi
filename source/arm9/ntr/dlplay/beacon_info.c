// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#include <string.h>

#include <nds.h>

#include <dswifi9.h>

#include "arm9/ntr/dlplay/dlplay.h"
#include "common/ieee_defs.h"

// The game information record is too big to fit in one beacon frame, so it is
// split in fragments that are sent in consecutive beacon frames. Clients put it
// back together using the index and the total stored in every fragment.

static u8 dlplay_game_info[DLPLAY_GAME_INFO_SIZE];
static u8 dlplay_fragments[DSWIFI_BEACON_MAX_FRAGMENTS][DLPLAY_FRAGMENT_SIZE];

// How many fragments of the table above are in use, and how many of those
// describe the room rather than carrying a piece of the record. A room of two
// only needs one of the latter, which is the ten fragments this has always sent.
static int dlplay_fragment_count = DLPLAY_FIXED_FRAGMENT_COUNT + 1;
static int dlplay_volatile_count = 1;

// Number of the record being announced. Every fragment carries it, and a client
// that sees it change throws away what it collected and starts again, which is
// what lets the title and the icon be replaced while the host is running.
static u8 dlplay_seq_fixed;

// Who is in the room, what to say about each of them, and the eight bytes the
// application announces alongside. The fragments that describe the room are
// built from these, so they are kept here rather than read back out of them.
static u16 dlplay_player_mask = DLPLAY_PLAYER_FLAG_HOST;
static u16 dlplay_player_changed;
static u8 dlplay_player_sequence;
static Wifi_DlPlayMember dlplay_members[WIFI_MAX_MULTIPLAYER_CLIENTS + 1];
static u8 dlplay_user_volat[DSWIFI_DLPLAY_USER_VOLAT_SIZE];

// Copies a UTF-16LE string to the record, padding the rest with zeros.
static void Wifi_DlPlayWriteString(u8 *dest, const u16 *src, size_t src_len,
                                   size_t max_len)
{
    if (src_len > max_len)
        src_len = max_len;

    for (size_t i = 0; i < max_len; i++)
    {
        u16 c = (src != NULL) && (i < src_len) ? src[i] : 0;

        dest[i * 2] = c & 0xFF;
        dest[(i * 2) + 1] = (c >> 8) & 0xFF;
    }
}

// One's complement sum of the fragment interpreted as big endian 16 bit values,
// stored in big endian order as well. It doesn't cover the start of the
// fragment so that the fields there can be modified while the beacon is being
// transmitted without having to calculate it again.
static void Wifi_DlPlayFragmentChecksum(u8 *fragment)
{
    u32 sum = 0;

    for (int i = DLPLAY_FRAG_INDEX2; i < DLPLAY_FRAGMENT_SIZE; i += 2)
        sum += (fragment[i] << 8) | fragment[i + 1];

    sum = (~(sum + (sum >> 16))) & 0xFFFF;

    fragment[DLPLAY_FRAG_CHECKSUM + 0] = (sum >> 8) & 0xFF;
    fragment[DLPLAY_FRAG_CHECKSUM + 1] = sum & 0xFF;
}

// Hands the table over to the ARM7, which writes one fragment of it into the
// beacon before every transmission.
//
// This restarts the cycle at the first fragment, so it has to be done only when
// something has actually changed. A host that called it on every frame would
// send the first fragment for ever and no client would collect the record.
static int Wifi_DlPlayBeaconPublish(void)
{
    return Wifi_BeaconSetExtraDataFragments(dlplay_fragments, DLPLAY_FRAGMENT_SIZE,
                                            dlplay_fragment_count);
}

// Writes one player into a slot of the list carried by a fragment.
static void Wifi_DlPlayWriteMember(u8 *slot, int aid, const Wifi_DlPlayMember *member)
{
    u8 name_len = member->name_len;

    if (name_len > DSWIFI_DLPLAY_HOST_NAME_LEN)
        name_len = DSWIFI_DLPLAY_HOST_NAME_LEN;

    // The player number is the association ID the client was given. It has to be
    // there: a client stores every record it receives under the number in it,
    // and treats a record numbered zero as an empty slot.
    slot[DLPLAY_MEMBER_OFS_COLOR] = (member->favorite_color & 0x0F) | (aid << 4);
    slot[DLPLAY_MEMBER_OFS_NAME_LEN] = name_len;

    Wifi_DlPlayWriteString(slot + DLPLAY_MEMBER_OFS_NAME, member->name, name_len,
                           DSWIFI_DLPLAY_HOST_NAME_LEN);
}

// Rebuilds every fragment that describes the room from the state above.
//
// Only four players fit in a fragment, so a room bigger than that is described
// by several of them. They are identical apart from the players they carry: a
// client tells one announcement from the next by the sequence number, and would
// throw away the players it had collected if the fragments of one announcement
// disagreed about it.
static void Wifi_DlPlayWriteVolatileFragments(void)
{
    u8 count = 0;

    for (int i = 0; i < 16; i++)
    {
        if (dlplay_player_mask & (1 << i))
            count++;
    }

    for (int v = 0; v < dlplay_volatile_count; v++)
    {
        u8 *fragment = dlplay_fragments[DLPLAY_FIXED_FRAGMENT_COUNT + v];

        memset(fragment, 0, DLPLAY_FRAGMENT_SIZE);

        // Every fragment is numbered, and a client drops a host that sends the
        // same number over and over, so these carry on from the record.
        fragment[DLPLAY_FRAG_INDEX] = DLPLAY_FIXED_FRAGMENT_COUNT + v;
        fragment[DLPLAY_FRAG_SEQ_FIXED] = dlplay_seq_fixed;
        fragment[DLPLAY_FRAG_TOTAL] = DLPLAY_FIXED_FRAGMENT_COUNT;

        // This fragment isn't part of the record. It describes who is in the
        // game, so the fields after the checksum mean something else: a count of
        // players, a bitmap of which ones are present, and a bitmap of which
        // ones changed since the last announcement.
        fragment[DLPLAY_FRAG_SEQUENCE_END] = 2;
        fragment[DLPLAY_FRAG_CONNECTED] = dlplay_player_sequence;

        fragment[DLPLAY_FRAG_PLAYER_COUNT] = count;
        fragment[DLPLAY_FRAG_PLAYER_FLAGS + 0] = dlplay_player_mask & 0xFF;
        fragment[DLPLAY_FRAG_PLAYER_FLAGS + 1] = (dlplay_player_mask >> 8) & 0xFF;
        fragment[DLPLAY_FRAG_PLAYER_CHANGED + 0] = dlplay_player_changed & 0xFF;
        fragment[DLPLAY_FRAG_PLAYER_CHANGED + 1] = (dlplay_player_changed >> 8) & 0xFF;

        // The players themselves, four to a fragment in order of association ID.
        // The host isn't one of them: it is described by the record instead, and
        // a client counts it as present without being told.
        int index = 0;

        for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
        {
            if ((dlplay_player_mask & (1 << aid)) == 0)
                continue;

            int slot = index - (v * DLPLAY_MEMBERS_PER_FRAGMENT);

            index++;

            if ((slot < 0) || (slot >= DLPLAY_MEMBERS_PER_FRAGMENT))
                continue;

            Wifi_DlPlayWriteMember(
                fragment + DLPLAY_FRAG_MEMBERS + (slot * DLPLAY_MEMBER_SIZE),
                aid, &dlplay_members[aid]);
        }

        memcpy(fragment + DLPLAY_FRAG_USER_VOLAT, dlplay_user_volat,
               DSWIFI_DLPLAY_USER_VOLAT_SIZE);

        Wifi_DlPlayFragmentChecksum(fragment);
    }
}

void Wifi_DlPlayBeaconResetPlayers(void)
{
    dlplay_player_mask = DLPLAY_PLAYER_FLAG_HOST;
    dlplay_player_changed = 0;
    dlplay_player_sequence = 0;

    memset(dlplay_members, 0, sizeof(dlplay_members));
    memset(dlplay_user_volat, 0, sizeof(dlplay_user_volat));
}

int Wifi_DlPlayBeaconSetInfo(const Wifi_DlPlayRom *rom, const Wifi_DlPlayInfo *info,
                             u8 max_clients)
{
    const u8 *icon_bitmap = NULL;
    const u8 *icon_palette = NULL;
    const u8 *banner_title_src = NULL;

    if ((info != NULL) && (info->icon_bitmap != NULL))
        icon_bitmap = info->icon_bitmap;
    if ((info != NULL) && (info->icon_palette != NULL))
        icon_palette = info->icon_palette;

    // Fall back to the icon stored in the banner of the ROM
    if ((icon_bitmap == NULL) || (icon_palette == NULL))
    {
        u32 banner = (rom->rom[NDS_HDR_BANNER_OFFSET + 0] << 0)
                   | (rom->rom[NDS_HDR_BANNER_OFFSET + 1] << 8)
                   | (rom->rom[NDS_HDR_BANNER_OFFSET + 2] << 16)
                   | (rom->rom[NDS_HDR_BANNER_OFFSET + 3] << 24);

        if ((banner != 0) &&
            ((banner + NDS_BANNER_ICON_PALETTE + DSWIFI_DLPLAY_ICON_PALETTE_SIZE)
                <= rom->rom_size))
        {
            if (icon_bitmap == NULL)
                icon_bitmap = rom->rom + banner + NDS_BANNER_ICON_BITMAP;
            if (icon_palette == NULL)
                icon_palette = rom->rom + banner + NDS_BANNER_ICON_PALETTE;

            if ((banner + NDS_BANNER_TITLE_ENGLISH + (NDS_BANNER_TITLE_CHARS * 2))
                    <= rom->rom_size)
                banner_title_src = rom->rom + banner + NDS_BANNER_TITLE_ENGLISH;
        }
    }

    // Build the record
    // ----------------

    memset(dlplay_game_info, 0, sizeof(dlplay_game_info));

    if (icon_palette != NULL)
    {
        memcpy(dlplay_game_info + DLPLAY_INFO_ICON_PALETTE, icon_palette,
               DSWIFI_DLPLAY_ICON_PALETTE_SIZE);
    }

    if (icon_bitmap != NULL)
    {
        memcpy(dlplay_game_info + DLPLAY_INFO_ICON_BITMAP, icon_bitmap,
               DSWIFI_DLPLAY_ICON_BITMAP_SIZE);
    }

    // Name of the host console. It defaults to the one in the DS firmware.
    const u16 *host_name = (info != NULL) ? info->host_name : NULL;
    size_t host_name_len = (info != NULL) ? info->host_name_len : 0;

    u16 firmware_name[DSWIFI_DLPLAY_HOST_NAME_LEN];

    if (host_name == NULL)
    {
        host_name_len = PersonalData->nameLen;
        if (host_name_len > DSWIFI_DLPLAY_HOST_NAME_LEN)
            host_name_len = DSWIFI_DLPLAY_HOST_NAME_LEN;

        for (size_t i = 0; i < host_name_len; i++)
            firmware_name[i] = PersonalData->name[i];

        host_name = firmware_name;
    }

    if (host_name_len > DSWIFI_DLPLAY_HOST_NAME_LEN)
        host_name_len = DSWIFI_DLPLAY_HOST_NAME_LEN;

    dlplay_game_info[DLPLAY_INFO_HOST_NAME_LEN] = host_name_len;
    Wifi_DlPlayWriteString(dlplay_game_info + DLPLAY_INFO_HOST_NAME, host_name,
                           host_name_len, DSWIFI_DLPLAY_HOST_NAME_LEN);

    // The host is always player 0. The colour comes from the settings of the
    // console, like the name above.
    dlplay_game_info[DLPLAY_INFO_HOST_PLAYER] =
        (PersonalData->theme & 0x0F) | (0 << 4);

    // How many players the program on offer supports, counting the host. This is
    // the size of the room the caller asked for, already clamped by
    // Wifi_DlPlayPickFrameSize(), so that what a client is shown next to the
    // title matches the number of consoles this host will actually take.
    //
    // A room that takes nobody would be announced as holding one player, which
    // tells clients the game is full before anyone has joined.
    dlplay_game_info[DLPLAY_INFO_MAX_PLAYERS] =
        (max_clients < 1) ? DLPLAY_MAX_PLAYERS : (max_clients + 1);

    // Title and description. If the caller doesn't provide them they are taken
    // from the banner of the ROM, the same place the icon comes from. The banner
    // title is one string of up to 128 characters split into lines, so the first
    // line becomes the title and whatever follows becomes the description.
    const u16 *title = (info != NULL) ? info->title : NULL;
    size_t title_len = (info != NULL) ? info->title_len : 0;
    const u16 *description = (info != NULL) ? info->description : NULL;
    size_t description_len = (info != NULL) ? info->description_len : 0;

    u16 banner_title[NDS_BANNER_TITLE_CHARS];

    if ((title == NULL) && (banner_title_src != NULL))
    {
        // The banner isn't aligned to 16 bits in the ROM, so it is read a byte
        // at a time.
        size_t first_line = NDS_BANNER_TITLE_CHARS;
        size_t total = 0;

        for (size_t i = 0; i < NDS_BANNER_TITLE_CHARS; i++)
        {
            u16 c = banner_title_src[i * 2] | (banner_title_src[(i * 2) + 1] << 8);

            if (c == 0)
                break;

            if ((c == '\n') && (first_line == NDS_BANNER_TITLE_CHARS))
                first_line = i;

            banner_title[i] = c;
            total = i + 1;
        }

        if (first_line > total)
            first_line = total;

        title = banner_title;
        title_len = first_line;

        // Skip the newline that separated them.
        if (total > (first_line + 1))
        {
            description = banner_title + first_line + 1;
            description_len = total - first_line - 1;
        }
    }

    Wifi_DlPlayWriteString(dlplay_game_info + DLPLAY_INFO_TITLE, title, title_len,
                           DSWIFI_DLPLAY_TITLE_LEN);

    Wifi_DlPlayWriteString(dlplay_game_info + DLPLAY_INFO_DESCRIPTION, description,
                           description_len, DSWIFI_DLPLAY_DESC_LEN);

    // Split the record in fragments
    // -----------------------------

    // Room for the record, then one fragment per group of four players. A client
    // only treats the game information as complete once it holds a record for
    // every player announced, so the whole room has to fit in one pass of the
    // table -- there is no way to rotate a window over it, because handing the
    // ARM7 a new table is what restarts the pass.
    dlplay_volatile_count = DLPLAY_VOLATILE_FRAGMENT_COUNT(max_clients);
    dlplay_fragment_count = DLPLAY_FIXED_FRAGMENT_COUNT + dlplay_volatile_count;

    // Tell clients that already collected the previous record to read this one.
    dlplay_seq_fixed++;

    memset(dlplay_fragments, 0, sizeof(dlplay_fragments));

    for (int i = 0; i < DLPLAY_FIXED_FRAGMENT_COUNT; i++)
    {
        u8 *fragment = dlplay_fragments[i];

        // The game ID is repeated at the start of every fragment, and the
        // retail DS Download Station leaves it zero -- it is already in the
        // fixed part of the element, and clients key their collected fragments
        // on whatever is here. A retail game hosting Download Play does fill it
        // in, so this is the field to put back first if fragments stop being
        // collected.

        fragment[DLPLAY_FRAG_INDEX] = i;
        fragment[DLPLAY_FRAG_SEQ_FIXED] = dlplay_seq_fixed;

        // How many fragments the record is split into. Every fragment carries
        // it, the ones that describe the room included: this used to be left at
        // zero there, so a client reading one of those was told there were none
        // to collect.
        fragment[DLPLAY_FRAG_TOTAL] = DLPLAY_FIXED_FRAGMENT_COUNT;

        fragment[DLPLAY_FRAG_INDEX2] = i;

        // The record doesn't divide evenly between the fragments, so the last
        // one carries less data than the rest.
        size_t offset = (size_t)i * DLPLAY_FRAG_PAYLOAD_MAX;
        size_t len = DLPLAY_FRAG_PAYLOAD_MAX;

        if (offset >= DLPLAY_GAME_INFO_SIZE)
            len = 0;
        else if ((offset + len) > DLPLAY_GAME_INFO_SIZE)
            len = DLPLAY_GAME_INFO_SIZE - offset;

        fragment[DLPLAY_FRAG_PAYLOAD_LEN] = len;

        if (len > 0)
            memcpy(fragment + DLPLAY_FRAG_PAYLOAD, dlplay_game_info + offset, len);

        Wifi_DlPlayFragmentChecksum(fragment);
    }

    // The room is described from whatever is already known about it, so that
    // replacing the record while the host is running doesn't announce an empty
    // room to everyone who is in it.
    Wifi_DlPlayWriteVolatileFragments();

    return Wifi_DlPlayBeaconPublish();
}

const void *Wifi_DlPlayBeaconGetFirstFragment(void)
{
    return dlplay_fragments[0];
}

static bool Wifi_DlPlayMemberChanged(const Wifi_DlPlayMember *a,
                                     const Wifi_DlPlayMember *b)
{
    if ((a->favorite_color != b->favorite_color) || (a->name_len != b->name_len))
        return true;

    return memcmp(a->name, b->name, sizeof(a->name)) != 0;
}

void Wifi_DlPlayBeaconSetPlayers(u16 clients, const Wifi_DlPlayMember *members)
{
    // Bit 0 is the host, which is always playing.
    u16 now = clients | DLPLAY_PLAYER_FLAG_HOST;

    // Say nothing when there is nothing to say. Every announcement restarts the
    // cycle of fragments, so one made on every frame would stop the record from
    // ever reaching a client.
    //
    // The names are part of this: a client turns up before it has said who it
    // is, so it joins the room in one announcement and is described in the next.
    bool changed = (now != dlplay_player_mask);

    for (int aid = 1; (aid <= WIFI_MAX_MULTIPLAYER_CLIENTS) && !changed; aid++)
    {
        if ((now & (1 << aid)) == 0)
            continue;

        changed = Wifi_DlPlayMemberChanged(&dlplay_members[aid], &members[aid]);
    }

    if (!changed)
        return;

    // Which of them changed since the last announcement. A client uses this to
    // work out which of the players it had collected to forget, so it is the
    // difference in the list rather than in the names.
    dlplay_player_changed = now ^ dlplay_player_mask;
    dlplay_player_mask = now;

    // Number of the announcement. Clients watch it to notice that the list of
    // players has changed, so it has to move whenever the list does.
    dlplay_player_sequence++;

    for (int aid = 1; aid <= WIFI_MAX_MULTIPLAYER_CLIENTS; aid++)
        dlplay_members[aid] = members[aid];

    Wifi_DlPlayWriteVolatileFragments();
    Wifi_DlPlayBeaconPublish();
}

int Wifi_DlPlayBeaconSetUserVolatData(const void *data)
{
    u8 next[DSWIFI_DLPLAY_USER_VOLAT_SIZE];

    if (data != NULL)
        memcpy(next, data, sizeof(next));
    else
        memset(next, 0, sizeof(next));

    // Unlike the list of players this carries no sequence number, because a
    // client takes it from every announcement it receives rather than collecting
    // it. That also means there is nothing to bump here, only the fragments to
    // write again -- and nothing to write when the bytes are the same.
    if (memcmp(next, dlplay_user_volat, sizeof(next)) == 0)
        return 0;

    memcpy(dlplay_user_volat, next, sizeof(next));

    Wifi_DlPlayWriteVolatileFragments();

    return Wifi_DlPlayBeaconPublish();
}
