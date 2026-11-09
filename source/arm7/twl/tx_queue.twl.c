// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025 Antonio Niño Díaz

#include "arm7/debug.h"
#include "arm7/ipc.h"
#include "arm7/twl/card.h"

void Wifi_TWL_TxArm9QueueFlush(void)
{
    const u8 *txbufData = (const u8 *)WifiData->txbufData;

    int oldIME = enterCriticalSection();

    u32 read_idx = WifiData->txbufRead;

    assert((read_idx & 3) == 0);

    // A few a call: this runs every millisecond (the update timer) with
    // interrupts off, and a whole queue of a busy game's packets took longer
    // than that -- the timer was due again before it returned, and nothing
    // else on the ARM7 ever ran. The rest go next time.
    for (int sent = 0; sent < 3; sent++)
    {
        // Read packet size
        size_t size = read_u32(txbufData + read_idx);
        if (size == 0)
        {
            // No more packets to process
            break;
        }
        else if (size == WIFI_SIZE_WRAP) // Mark to reset pointer
        {
            read_idx = 0;
            size = read_u32(txbufData + read_idx);
            if (size == 0)
                break;
        }
        read_idx += sizeof(uint32_t);

        // Read packet data
        data_send_pkt_idk(txbufData + read_idx, size);
        read_idx += round_up_32(size);

        assert(read_idx <= (WIFI_TXBUFFER_SIZE - sizeof(u32)));

        WifiData->txbufRead = read_idx;

        WifiData->stats[WSTAT_TXPACKETS]++;
        WifiData->stats[WSTAT_TXBYTES] += size;
        WifiData->stats[WSTAT_TXDATABYTES] += size;
    }

    leaveCriticalSection(oldIME);
}
