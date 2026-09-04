// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#ifndef DSWIFI_ARM9_NTR_MULTIPLAYER_H__
#define DSWIFI_ARM9_NTR_MULTIPLAYER_H__

#include <dswifi_common.h>

#include "common/common_ntr_defs.h"
#include "common/ieee_defs.h"

// Sent by the host with CMD packets

typedef struct {
    Wifi_TxHeader tx;
    IEEE_DataFrameHeader ieee;
    u16 client_time;
    u16 client_bits;
    u8 body[0];
} TxMultiplayerHostIeeeDataFrame;

typedef struct {
    IEEE_DataFrameHeader ieee;
    u16 client_time;
    u16 client_bits;
    u8 body[0];
} MultiplayerHostIeeeDataFrame;

// Sent by the host with direct data packets

typedef struct {
    Wifi_TxHeader tx;
    IEEE_DataFrameHeader ieee;
    u8 body[0];
} TxIeeeDataFrame;

// Sent by clients with REPLY and direct data packets

typedef struct {
    Wifi_TxHeader tx;
    IEEE_DataFrameHeader ieee;
    u8 client_aid;
    u8 client_pad;
    u8 body[0];
} TxMultiplayerClientIeeeDataFrame;

static_assert(sizeof(TxMultiplayerClientIeeeDataFrame) == 38);

// Frames received from a client. The two bytes between the IEEE header and the
// body are an association ID and a pad byte in frames sent by DSWifi, but a size
// and a flags byte in frames sent by Nintendo's software. Only their combined
// length matters here, because the body starts after them either way, and the
// sender is identified by its MAC address rather than by anything in here.
typedef struct {
    IEEE_DataFrameHeader ieee;
    u8 client_aid;
    u8 client_pad;
    u8 body[0];
} MultiplayerClientIeeeDataFrame;

// The body offset is the whole reply protocol: a console running Nintendo's
// software puts the size of its reply in the first of those two bytes and its
// flags in the second, and the message the host has to act on starts right
// after them. It used to be a single field followed by implicit padding, which
// happened to add up to the same 26 bytes but pinned nothing.
static_assert(sizeof(MultiplayerClientIeeeDataFrame) == 26);

bool Wifi_MultiplayerClientGetMacFromAID(int aid, void *dest_macaddr);
int Wifi_MultiplayerClientGetAIDFromMac(const void *macaddr);
bool Wifi_MultiplayerClientMatchesMacAndAID(int aid, const void *macaddr);

// Handlers that need to be called from the loop that processes packets.
// Internally they check if there is a user handler or not. If there is a
// handler, it will send the packets to that handler.
void Wifi_MultiplayerHandlePacketFromClient(const u8 *packet, size_t size);
void Wifi_MultiplayerHandlePacketFromHost(const u8 *packet, size_t size);

#endif // DSWIFI_ARM9_NTR_MULTIPLAYER_H__
