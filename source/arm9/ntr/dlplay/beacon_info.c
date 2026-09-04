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
static u8 dlplay_fragments[DLPLAY_FRAGMENT_COUNT][DLPLAY_FRAGMENT_SIZE];

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

int Wifi_DlPlayBeaconSetInfo(const Wifi_DlPlayRom *rom, const Wifi_DlPlayInfo *info)
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

    // How many players the program on offer supports, counting the host. It is
    // what a client shows next to the title, and says nothing about how many
    // consoles this host serves at once, which is one.
    //
    // Announcing 1 would tell clients the game is full before anyone joined, so
    // anything below two is treated as unset.
    u8 max_players = (info != NULL) ? info->max_players : 0;

    if ((max_players < 2) || (max_players > DSWIFI_DLPLAY_MAX_PLAYERS))
        max_players = DLPLAY_MAX_PLAYERS;

    dlplay_game_info[DLPLAY_INFO_MAX_PLAYERS] = max_players;

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

    memset(dlplay_fragments, 0, sizeof(dlplay_fragments));

    for (int i = 0; i < DLPLAY_FRAGMENT_COUNT; i++)
    {
        u8 *fragment = dlplay_fragments[i];

        // The game ID is repeated at the start of every fragment, and the
        // retail DS Download Station leaves it zero -- it is already in the
        // fixed part of the element, and clients key their collected fragments
        // on whatever is here. A retail game hosting Download Play does fill it
        // in, so this is the field to put back first if fragments stop being
        // collected.

        fragment[DLPLAY_FRAG_INDEX] = i;

        // Which fragment is the last one. Every fragment carries it, the volatile
        // one included: this used to be left at zero there, so a client reading
        // that fragment was told there were none to collect.
        fragment[DLPLAY_FRAG_TOTAL] = DLPLAY_FRAGMENT_COUNT - 1;

        if (i == (DLPLAY_FRAGMENT_COUNT - 1))
        {
            // The last fragment isn't part of the record. It describes who is in
            // the game, so the fields after the checksum mean something else: a
            // count of players, then a bitmap of which ones are present.
            fragment[DLPLAY_FRAG_SEQUENCE_END] = 2;

            // The count includes the host, which is playing on its own until a
            // client turns up. Wifi_DlPlayBeaconSetPlayers() keeps both fields
            // up to date after that.
            fragment[DLPLAY_FRAG_PLAYER_COUNT] = 1;
            fragment[DLPLAY_FRAG_PLAYER_FLAGS] = DLPLAY_PLAYER_FLAG_HOST;
            fragment[DLPLAY_FRAG_PLAYER_FLAGS + 1] = 0;
        }
        else
        {
            fragment[DLPLAY_FRAG_INDEX2] = i;

            // The record doesn't divide evenly between the fragments, so the
            // last one carries less data than the rest.
            size_t offset = (size_t)i * DLPLAY_FRAG_PAYLOAD_MAX;
            size_t len = DLPLAY_FRAG_PAYLOAD_MAX;

            if (offset >= DLPLAY_GAME_INFO_SIZE)
                len = 0;
            else if ((offset + len) > DLPLAY_GAME_INFO_SIZE)
                len = DLPLAY_GAME_INFO_SIZE - offset;

            fragment[DLPLAY_FRAG_PAYLOAD_LEN] = len;

            if (len > 0)
                memcpy(fragment + DLPLAY_FRAG_PAYLOAD, dlplay_game_info + offset, len);
        }

        Wifi_DlPlayFragmentChecksum(fragment);
    }

    return Wifi_BeaconSetExtraDataFragments(dlplay_fragments, DLPLAY_FRAGMENT_SIZE,
                                            DLPLAY_FRAGMENT_COUNT);
}

const void *Wifi_DlPlayBeaconGetFirstFragment(void)
{
    return dlplay_fragments[0];
}

void Wifi_DlPlayBeaconSetPlayers(u16 clients)
{
    // Number of the last announcement. Clients watch it to notice that the list
    // of players has changed, so it has to move whenever the list does.
    static u8 sequence = 0;

    u8 *fragment = dlplay_fragments[DLPLAY_FRAGMENT_COUNT - 1];

    // Bit 0 is the host, which is always playing.
    u16 now = clients | DLPLAY_PLAYER_FLAG_HOST;

    u16 previous = (u16)(fragment[DLPLAY_FRAG_PLAYER_FLAGS]
                       | (fragment[DLPLAY_FRAG_PLAYER_FLAGS + 1] << 8));

    if (now == previous)
        return;

    u8 count = 0;
    for (int i = 0; i < 16; i++)
    {
        if (now & (1 << i))
            count++;
    }

    fragment[DLPLAY_FRAG_CONNECTED] = ++sequence;

    fragment[DLPLAY_FRAG_PLAYER_COUNT] = count;
    fragment[DLPLAY_FRAG_PLAYER_FLAGS + 0] = now & 0xFF;
    fragment[DLPLAY_FRAG_PLAYER_FLAGS + 1] = (now >> 8) & 0xFF;

    // Which of them changed since the last announcement.
    u16 changed = now ^ previous;

    fragment[DLPLAY_FRAG_PLAYER_CHANGED + 0] = changed & 0xFF;
    fragment[DLPLAY_FRAG_PLAYER_CHANGED + 1] = (changed >> 8) & 0xFF;

    // These fields are inside the part of the fragment that the checksum covers,
    // so it has to be worked out again and the whole table handed to the ARM7
    // once more. Patching bytes in place isn't enough here.
    Wifi_DlPlayFragmentChecksum(fragment);

    Wifi_BeaconSetExtraDataFragments(dlplay_fragments, DLPLAY_FRAGMENT_SIZE,
                                     DLPLAY_FRAGMENT_COUNT);
}
