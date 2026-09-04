// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#include <string.h>

#include <nds.h>

#include "arm9/ntr/dlplay/dlplay.h"

// The fields of the NDS ROM header are stored in little endian order, the same
// order used by the console, but they aren't aligned, so they can't be read
// directly with a pointer.

// Bytes of the program carried by each frame.
//
// Not a constant: the size of a frame is chosen when the host starts, from how
// many consoles it means to serve, so the program has to be cut up to match. See
// Wifi_DlPlayPickFrameSize().
static size_t rom_block_size = DLPLAY_BLOCK_SIZE;

void Wifi_DlPlayRomSetBlockSize(size_t size)
{
    rom_block_size = size;
}

static u32 Wifi_DlPlayRead32(const u8 *src)
{
    return (src[0] << 0) | (src[1] << 8) | (src[2] << 16) | (src[3] << 24);
}

// All zeros, used for ROMs that don't have a signature.
static const u8 dlplay_rsa_none[NDS_RSA_SIZE];

bool Wifi_DlPlayRomIsSigned(const void *rom, size_t rom_size)
{
    const u8 *data = rom;

    if (rom_size < (NDS_HDR_USED_ROM_SIZE + 4))
        return false;

    // The signature is stored right after the part of the ROM that is used, and
    // it always starts with the string "ac".
    u32 used_size = Wifi_DlPlayRead32(data + NDS_HDR_USED_ROM_SIZE);

    if ((used_size + NDS_RSA_SIZE) > rom_size)
        return false;

    return (data[used_size] == 'a') && (data[used_size + 1] == 'c');
}

// Returns the number of blocks needed to send "size" bytes.
static int Wifi_DlPlayBlockCount(u32 size)
{
    return (size + (rom_block_size - 1)) / rom_block_size;
}

int Wifi_DlPlayRomParse(Wifi_DlPlayRom *out, const void *rom, size_t rom_size)
{
    const u8 *data = rom;

    if ((rom == NULL) || (rom_size < NDS_HDR_SIZE))
        return -1;

    u32 arm9_offset = Wifi_DlPlayRead32(data + NDS_HDR_ARM9_ROM_OFFSET);
    u32 arm9_size   = Wifi_DlPlayRead32(data + NDS_HDR_ARM9_SIZE);
    u32 arm7_offset = Wifi_DlPlayRead32(data + NDS_HDR_ARM7_ROM_OFFSET);
    u32 arm7_size   = Wifi_DlPlayRead32(data + NDS_HDR_ARM7_SIZE);

    if ((arm9_size == 0) || (arm7_size == 0))
        return -1;

    // Make sure that both binaries are actually inside the ROM
    if ((arm9_offset > rom_size) || ((rom_size - arm9_offset) < arm9_size))
        return -1;
    if ((arm7_offset > rom_size) || ((rom_size - arm7_offset) < arm7_size))
        return -1;

    memset(out, 0, sizeof(Wifi_DlPlayRom));

    out->rom = data;
    out->rom_size = rom_size;

    // The header of the ROM is sent like any other part of the program. It is
    // bigger than one block, so it takes as many as it needs and the last one
    // is short, the same way the binaries do.
    out->segment[DLPLAY_SEGMENT_HEADER].data = data;
    out->segment[DLPLAY_SEGMENT_HEADER].size = NDS_HDR_SIZE;
    out->segment[DLPLAY_SEGMENT_ARM9].data = data + arm9_offset;
    out->segment[DLPLAY_SEGMENT_ARM9].size = arm9_size;
    out->segment[DLPLAY_SEGMENT_ARM7].data = data + arm7_offset;
    out->segment[DLPLAY_SEGMENT_ARM7].size = arm7_size;

    int block = 0;

    for (int i = 0; i < DLPLAY_SEGMENT_COUNT; i++)
    {
        out->segment[i].first_block = block;
        out->segment[i].blocks = Wifi_DlPlayBlockCount(out->segment[i].size);
        block += out->segment[i].blocks;
    }

    out->total_blocks = block;

    // The client loads the program in main RAM, so it can't be bigger than it.
    if (((u32)out->total_blocks * rom_block_size) > DSWIFI_DLPLAY_MAX_PROGRAM_SIZE)
        return -1;

    if (Wifi_DlPlayRomIsSigned(rom, rom_size))
        out->rsa = data + Wifi_DlPlayRead32(data + NDS_HDR_USED_ROM_SIZE);
    else
        out->rsa = dlplay_rsa_none;

    return 0;
}

const u8 *Wifi_DlPlayRomGetBlock(const Wifi_DlPlayRom *rom, int block, size_t *size)
{
    *size = 0;

    if ((block < 0) || (block >= rom->total_blocks))
        return NULL;

    for (int i = 0; i < DLPLAY_SEGMENT_COUNT; i++)
    {
        int index = block - rom->segment[i].first_block;

        if ((index < 0) || (index >= rom->segment[i].blocks))
            continue;

        u32 offset = (u32)index * rom_block_size;
        u32 remaining = rom->segment[i].size - offset;

        // Only the last block of a part is short, and it is sent at its real
        // length rather than padded, which is what a real host does.
        *size = (remaining > rom_block_size) ? rom_block_size : remaining;

        return rom->segment[i].data + offset;
    }

    return NULL;
}
