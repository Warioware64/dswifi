// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

// Internal definitions shared by the DS Download Play host implementation.
//
// The protocol was reverse engineered from the Wii homebrew "wii-ds-rom-sender"
// by FIX94, which implements the same role from a Wii.

#ifndef DSWIFI_ARM9_NTR_DLPLAY_H__
#define DSWIFI_ARM9_NTR_DLPLAY_H__

#include <stddef.h>

#include <nds/ndstypes.h>

#include <dswifi_dlplay.h>

// Offsets of the fields of the NDS ROM header that are needed to boot a program
// on a client console.
#define NDS_HDR_ARM9_ROM_OFFSET     0x20
#define NDS_HDR_ARM9_ENTRY          0x24
#define NDS_HDR_ARM9_RAM_ADDRESS    0x28
#define NDS_HDR_ARM9_SIZE           0x2C
#define NDS_HDR_ARM7_ROM_OFFSET     0x30
#define NDS_HDR_ARM7_ENTRY          0x34
#define NDS_HDR_ARM7_RAM_ADDRESS    0x38
#define NDS_HDR_ARM7_SIZE           0x3C
#define NDS_HDR_BANNER_OFFSET       0x68
#define NDS_HDR_USED_ROM_SIZE       0x80

// Size in bytes of the NDS ROM header sent as the first block of a transfer.
#define NDS_HDR_SIZE                0x160

// Size in bytes of the signature that follows the used part of a signed ROM.
#define NDS_RSA_SIZE                0x88

// Offsets inside the banner of a NDS ROM. The banner holds the same titles the
// console menu shows, one per language, each up to 128 characters of UTF-16 and
// split into lines by newlines.
#define NDS_BANNER_ICON_BITMAP      0x20
#define NDS_BANNER_ICON_PALETTE     0x220
#define NDS_BANNER_TITLE_ENGLISH    0x340
#define NDS_BANNER_TITLE_CHARS      128

// Number of players the game supports, counting the host. Only one client is
// served at a time, so that is the host and one other console.
//
// This is the total including the host, not the number of clients, so a host
// that announces 1 is telling clients that the game is full before anyone has
// joined.
#define DLPLAY_MAX_PLAYERS          2

// Size in bytes of the part of the program sent in each block. It is the size
// announced for the frames sent to the client, less the six bytes of the command
// that carries it.
#define DLPLAY_BLOCK_OVERHEAD       6
#define DLPLAY_BLOCK_SIZE           (DLPLAY_CMD_DATA_SIZE - DLPLAY_BLOCK_OVERHEAD)

// The size a retail DS Download Station announces, used instead of the one above
// when the room is big enough that air time runs out. See
// Wifi_DlPlayPickFrameSize().
#define DLPLAY_CMD_DATA_SIZE_SMALL  0x0100

// The most clients that can be served with the larger frame before Nintendo's
// air time budget is exceeded.
#define DLPLAY_LARGE_ROOM_CLIENTS   11

// Game ID (GGID) and fixed identifier announced by DS Download Station hosts.
//
//
// The value varies between hosts: a retail game hosting Download Play sends ver
// 1 and platform 0 (0x00010001), and GBATEK documents 0x00400001. A client only
// checks the magic number (MBi_CheckMBParent), so none of them is "the" right
// answer -- matching the host with the same game ID is.
#define DLPLAY_GAME_ID              0x00400120
#define DLPLAY_FIXED_ID             0x08000001

// Sizes announced in beacon frames for the frames of the protocol. Note that
// these are the sizes of the payload, not of the whole multiplayer frames.
//
// The size sent to the client is what decides how long a transfer takes, and
// clients size their buffers from what the beacon announces rather than from any
// fixed value: the retail DS Download Station announces 0x100, a retail game
// hosting Download Play announces 0x17C, and GBATEK calls 0x1FE the default.
// 0x1FE is also MB_COMM_P_SENDLEN_MAX, the largest Nintendo's own library
// allows, and it leaves 504 bytes for the program in each block against the 250
// that 0x100 leaves.
//
// There is airtime for it. Nintendo's budget for one multiplayer cycle
// (mb_wm_base.c) is 330 + 4*(sendSize+38) + children*(112 + 4*(recvSize+32))
// microseconds against a limit of 5600; one client at this size costs 2794.
#define DLPLAY_CMD_DATA_SIZE        0x01FE
#define DLPLAY_REPLY_DATA_SIZE      0x0008

// Size in bytes of the payload of the frames sent to the client. It is the
// announced size plus the two bytes of the header of the first entry and the
// two bytes of the empty entry that follows it.
#define DLPLAY_CMD_FRAME_SIZE       (DLPLAY_CMD_DATA_SIZE + 4)

// Values of the "beacon type" field of the Nintendo vendor information element,
// which is a set of flags rather than a type:
//
//     1  accepting entries
//     2  this host is sending a program
//     4  key sharing
//     8  continuous send
//
//
// A retail game hosting Download Play was measured sending 3 instead, without
// the continuous-send flag. That is a different kind of host, and imitating the
// station whose game ID this one announces is what matters.
//
// The entry flag stays set while a client is being served rather than dropping
// to 2
#define DLPLAY_BEACON_TYPE_SENDING  0x0B
#define DLPLAY_BEACON_TYPE_IDLE     0x0B

// Size in bytes of each of the blocks that the game information record is split
// in, and the number of them.
#define DLPLAY_FRAGMENT_SIZE        0x70
#define DLPLAY_FRAGMENT_COUNT       10

// Size in bytes of the SSID announced by DS Download Play hosts.
#define DLPLAY_SSID_SIZE            32

// Size in bytes of the game information record announced in beacon frames.
//
// This is the size of Nintendo's MBGameInfoFixed structure: a 544 byte icon, a
// 22 byte record for the host, the maximum number of players and a byte of
// padding, then 96 bytes of title and 192 bytes of description.
#define DLPLAY_GAME_INFO_SIZE       0x358

// Offsets of the fields of the game information record.
#define DLPLAY_INFO_ICON_PALETTE    0x000
#define DLPLAY_INFO_HOST_PLAYER     0x220
#define DLPLAY_INFO_ICON_BITMAP     0x020
#define DLPLAY_INFO_HOST_NAME_LEN   0x221
#define DLPLAY_INFO_HOST_NAME       0x222
#define DLPLAY_INFO_MAX_PLAYERS     0x236
#define DLPLAY_INFO_TITLE           0x238
#define DLPLAY_INFO_DESCRIPTION     0x298

// Offsets of the fields of the header of each fragment, from the start of the
// fragment. The checksum only covers the bytes from DLPLAY_FRAG_INDEX2 to the
// end of the fragment, which is why the fields before it can be modified while
// the beacon is being transmitted without recalculating it.
#define DLPLAY_FRAG_GGID            0x00
#define DLPLAY_FRAG_SEQUENCE_END    0x04
#define DLPLAY_FRAG_CONNECTED       0x06
#define DLPLAY_FRAG_INDEX           0x07
#define DLPLAY_FRAG_CHECKSUM        0x08
#define DLPLAY_FRAG_INDEX2          0x0A
#define DLPLAY_FRAG_TOTAL           0x0B
#define DLPLAY_FRAG_PAYLOAD_LEN     0x0C
#define DLPLAY_FRAG_PAYLOAD         0x0E

// The last fragment doesn't carry a piece of the record. It reports who is in
// the game instead, so the fields after the checksum have different meanings.
#define DLPLAY_FRAG_PLAYER_COUNT    0x0A
#define DLPLAY_FRAG_PLAYER_FLAGS    0x0C
#define DLPLAY_FRAG_PLAYER_CHANGED  0x0E

// Bit of DLPLAY_FRAG_PLAYER_FLAGS that stands for the host itself. Clients look
// at this to know who is already in the game, so leaving it clear makes the host
// look like it isn't there.
#define DLPLAY_PLAYER_FLAG_HOST     (1 << 0)

// Number of payload bytes carried by each fragment.
#define DLPLAY_FRAG_PAYLOAD_MAX     0x62

// Second byte of the frame header: the number of the WM port the payload
// belongs to, plus bit 4 when the list of clients follows it.

#define DLPLAY_PORT_BOOT            1
#define DLPLAY_PORT_STATION_NAME    13
#define DLPLAY_PORT_STATION_MENU    14
#define DLPLAY_PORT_STATION_FILE    15

// The Download Station content protocol, which a client speaks once the program
// this host sent it is running.
//
//
// A packet on one of these ports carries a halfword the multiboot port doesn't:
// the index of the chunk the client should expect next, sitting between the
// payload and the list of clients. The size byte doesn't count it.
// Every chunk is preceded by its own index, and both are part of the payload.
#define DLPLAY_STATION_INDEX_SIZE   2

// The length of the content, announced on port 13 before it is sent.
#define DLPLAY_STATION_LEN_SIZE     4

// Index that says there is nothing more to send, in place of a chunk.
#define DLPLAY_STATION_INDEX_END    0xFFFF

// A request on port 13: eight bytes naming what the client wants, then the
// halfword below.
//
// The names belong to the content, not here. The menu a station program displays
// is a list of entries and each one names the file to fetch: the menu blob of
// the Wii sender ends with "file.nds", while the retail kiosk's menu names
// "rom0000d" instead, and the client asks for whichever the user picked. The
// host only has to hand back what it is asked for, which is why the application
// answers the lookup rather than this file holding a list of names.
#define DLPLAY_STATION_NAME_SIZE    8
#define DLPLAY_STATION_REQUEST_SIZE (DLPLAY_STATION_NAME_SIZE + 2)

// The stream the client wants the answer on, and the port that means.
//
// The two requests a retail kiosk answers carry 1 and 2, and it serves them on
// ports 14 and 15 respectively, echoing the number in the length message. So
// this is the client choosing where its content arrives, and the host has no
// business deciding that from the name.
#define DLPLAY_STATION_STREAM_MIN   1
#define DLPLAY_STATION_STREAM_MAX   2
#define DLPLAY_STATION_STREAM_PORT(id) (DLPLAY_PORT_STATION_NAME + (id))

// Frames between answering a request and starting to send.
//
#define DLPLAY_STATION_SEND_DELAY   2

// How many chunks to queue per update.
//
#define DLPLAY_STATION_PER_UPDATE   1

#define DLPLAY_FLAGS_HAS_FOOTER     (1 << 4)
#define DLPLAY_FLAGS_PORT(p)        ((p) | DLPLAY_FLAGS_HAS_FOOTER)

#define DLPLAY_FLAGS_NORMAL         DLPLAY_FLAGS_PORT(DLPLAY_PORT_BOOT)
#define DLPLAY_FLAGS_FOOTERLESS     DLPLAY_PORT_BOOT

// Bytes of content carried by each frame, by the port it goes out on. Port 14
// takes 30 and port 15 takes 126, each preceded by the number of the chunk,
// which is what makes the payloads 32 and 128 bytes on the air.
//
// Nothing in the request implies these; they are what the kiosk uses. They are
// keyed by port rather than by what is being sent, because the port comes from
// the client and the host doesn't know whether it is sending a menu or a
// program.
#define DLPLAY_STATION_CHUNK_PORT14 0x1E
#define DLPLAY_STATION_CHUNK_PORT15 0x7E

// A console already running the Download Station client reports its name as ten
// spaces, which is how a host tells it apart from a console that has just been
// sent a program and wants another one.
#define DLPLAY_STATION_NAME_CHAR    ' '

// Size in bytes of the command block of each message, counted from the command
// byte and not counting the footer.
//
// The messages that carry no arguments are still six bytes: the command and
// five unused bytes. The boot information is the command, the 0xE4 byte block
// that describes where to load the program, and five unused bytes.
#define DLPLAY_CMD_SIZE_SHORT       6
#define DLPLAY_CMD_SIZE_BOOT_INFO   0xEA

// Block types of the frames sent to the client. These are , which the protocol shares with the library that runs on
// a real console, and the names say what each one means there. The numbers were
// already right; the names they used to have came from the Wii program this was
// derived from and described a protocol that doesn't exist.
#define DLPLAY_CMD_OP_DUMMY         0x00
#define DLPLAY_CMD_OP_SENDSTART     0x01
#define DLPLAY_CMD_OP_KICKREQ       0x02
#define DLPLAY_CMD_OP_DL_FILEINFO   0x03
#define DLPLAY_CMD_OP_DATA          0x04
#define DLPLAY_CMD_OP_BOOTREQ       0x05
#define DLPLAY_CMD_OP_MEMBER_FULL   0x06

// Block types of the frames received from the client.
//
// A client sends DUMMY whenever it has nothing else to say, so it is the normal
// state of the exchange rather than a fault. The eight zero bytes that every run
// so far has captured are one of these.
#define DLPLAY_REPLY_OP_DUMMY            0
#define DLPLAY_REPLY_OP_FILEREQ          7
#define DLPLAY_REPLY_OP_ACCEPT_FILEINFO  8
#define DLPLAY_REPLY_OP_CONTINUE         9
#define DLPLAY_REPLY_OP_STOPREQ          10
#define DLPLAY_REPLY_OP_BOOTREQ_ACCEPTED 11

// Messages between this host and a program it has already sent, which are not
// part of the Download Play protocol at all.
//
// A console running a program this host delivered has no reason to speak the
// transfer protocol again, and a host has no way of knowing one has come back
// except by what it says. Nintendo's own hosts tell a station client apart the
// same way -- by the content of a reply rather than by anything in the frame.
//
// The value is outside the range the protocol uses, so a client sending it can
// never be mistaken for one at some step of a transfer, and a console that
// really is mid-transfer can never be mistaken for a returning one.
#define DLPLAY_CMD_OP_APP                0x40
#define DLPLAY_REPLY_OP_APP              0x40

// The request a client sends to ask for a program, Nintendo's MBCommRequestData
// (mb.h). It doesn't fit in the eight bytes of a reply frame, so it arrives in
// five pieces of six bytes, each one preceded by the block type and its own
// index. The first piece a client sends is number 1, and they cycle 1, 2, 3, 4,
// 0, so they can turn up in any order.
#define DLPLAY_REQ_SIZE             29
#define DLPLAY_REQ_PIECE_SIZE       6
#define DLPLAY_REQ_PIECE_COUNT      5
#define DLPLAY_REQ_BUFFER_SIZE      (DLPLAY_REQ_PIECE_SIZE * DLPLAY_REQ_PIECE_COUNT)
#define DLPLAY_REQ_ALL_PIECES       ((1 << DLPLAY_REQ_PIECE_COUNT) - 1)

// Offsets of the fields of the request. The favourite colour is in the low four
// bits of its byte and the player number in the high four.
#define DLPLAY_REQ_OFS_GAME_ID      0
#define DLPLAY_REQ_OFS_COLOR        4
#define DLPLAY_REQ_OFS_NAME_LEN     5
#define DLPLAY_REQ_OFS_NAME         6
#define DLPLAY_REQ_OFS_VERSION      26
#define DLPLAY_REQ_OFS_FILE_ID      28

// How far ahead of the block the client has asked for the host is allowed to
// get before it goes back and sends that one again. This is Nintendo's
// MB_SEND_THRESHOLD (mb_parent.c), and it is the whole retransmission scheme.
//
// It has to cover everything in flight. Nintendo sends one block per multiplayer
// cycle and uses 2; this host queues DLPLAY_BLOCKS_PER_UPDATE at a time, and a
// window smaller than that would order a block resent before the client had a
// chance to answer for it.
#define DLPLAY_BLOCKS_PER_UPDATE    3
#define DLPLAY_SEND_THRESHOLD       (DLPLAY_BLOCKS_PER_UPDATE + 2)

// Parts of the ROM that are sent to the client, in the order they are sent. The
// client works out where to load each one from the boot information, so only
// their contents travel.
#define DLPLAY_SEGMENT_HEADER       0
#define DLPLAY_SEGMENT_ARM9         1
#define DLPLAY_SEGMENT_ARM7         2
#define DLPLAY_SEGMENT_COUNT        3

// Description of the program to be sent, filled by Wifi_DlPlayRomParse().
typedef struct {
    const u8 *rom;
    size_t rom_size;

    // Each part of the program starts on a block of its own, so the last block
    // of one is short rather than being shared with the next.
    struct {
        const u8 *data;
        u32 size;
        int first_block;
        int blocks;
    } segment[DLPLAY_SEGMENT_COUNT];

    // Total number of blocks, counting every part.
    int total_blocks;

    // Signature of the ROM, or all zeros if the ROM isn't signed.
    const u8 *rsa;
} Wifi_DlPlayRom;

// Reads the header of a NDS ROM and calculates how it has to be split in
// blocks. It returns 0 on success, or a negative value if the ROM can't be sent
// with DS Download Play.
int Wifi_DlPlayRomParse(Wifi_DlPlayRom *out, const void *rom, size_t rom_size);

// Returns a pointer to the bytes of the program that belong to the provided
// block, which goes from 0 to "total_blocks" - 1, and writes how many of them
// there are to "size". The last block of each part of the program is shorter
// than the rest.
// Sets how many bytes of the program each frame carries. Call before parsing a
// ROM: it decides how many blocks it is cut into.
void Wifi_DlPlayRomSetBlockSize(size_t size);

const u8 *Wifi_DlPlayRomGetBlock(const Wifi_DlPlayRom *rom, int block, size_t *size);

// Builds the game information record announced in beacon frames, splits it in
// fragments and hands them over to the ARM7. It returns 0 on success.
int Wifi_DlPlayBeaconSetInfo(const Wifi_DlPlayRom *rom, const Wifi_DlPlayInfo *info);

// Returns the first fragment of the game information record, which is the one
// stored in the beacon frame when it is created.
const void *Wifi_DlPlayBeaconGetFirstFragment(void);

// Announces who is in the game. "clients" is a mask of the association IDs of
// the clients that are connected, with bit 0 standing for the host.
//
// A client looks for itself in this list before it carries on, so it has to be
// updated as clients come and go.
void Wifi_DlPlayBeaconSetPlayers(u16 clients);

#endif // DSWIFI_ARM9_NTR_DLPLAY_H__
