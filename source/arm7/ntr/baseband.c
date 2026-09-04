// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2025 Antonio Niño Díaz

#include "arm7/ntr/flash.h"
#include "arm7/ntr/registers.h"

// Number of times to look at a busy flag before giving up on it.
//
// Nothing clears these but the hardware, so a wait without a limit is a hang.
// That is not hypothetical: a program started by DS Download Play inherits a
// wireless controller that is still in the middle of a multiplayer session, and
// a baseband left mid-transaction never reports itself idle. Wifi_BBWrite() has
// always had a limit; the two below did not, and the console stopped there
// before the library could report anything.
#define WIFI_BUSY_RETRIES   0x2710

int Wifi_BBRead(int addr)
{
    int i = WIFI_BUSY_RETRIES;
    while (W_BB_BUSY & 1)
    {
        if (!i--)
            return -1;
    }

    W_BB_CNT = addr | 0x6000;

    i = WIFI_BUSY_RETRIES;
    while (W_BB_BUSY & 1)
    {
        if (!i--)
            return -1;
    }

    return W_BB_READ;
}

int Wifi_BBWrite(int addr, int value)
{
    int i = WIFI_BUSY_RETRIES;
    while (W_BB_BUSY & 1)
    {
        if (!i--)
            return -1;
    }

    W_BB_WRITE = value;
    W_BB_CNT   = addr | 0x5000;

    i = WIFI_BUSY_RETRIES;
    while (W_BB_BUSY & 1)
    {
        if (!i--)
            return -1;
    }
    return 0;
}

void Wifi_BBInit(void)
{
    W_BB_MODE = 0x0100;

    for (int i = 0; i < 0x69; i++)
        Wifi_BBWrite(i, Wifi_FlashReadByte(F_BB_CFG + i));
}
