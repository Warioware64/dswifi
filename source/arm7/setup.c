// SPDX-License-Identifier: MIT
//
// Copyright (C) 2025-2026 Antonio Niño Díaz

#include <time.h>

#include "arm7/debug.h"
#include "arm7/ipc.h"
#include "arm7/setup.h"
#include "arm7/ntr/setup.h"
#include "arm7/ntr/update.h"
#include "arm7/twl/setup.h"
#include "arm7/twl/update.h"
#include "common/random.h"

// The library's ARM7 work runs in one thread of its own (upstream's 54d8e9f),
// made at the first Wifi_Init() and kept: each init then deinit is one turn of
// its loop. A thread made per init would, on a quick deinit and init again,
// start on the stack the last one may still be running on.
//
// The FIFO's handlers only ask for an init or a deinit and wake it: they run in
// libnds's FIFO thread, whose stack holds none of the work.
//
// In DSi mode the card is polled from this thread, never from a timer or from
// its interrupt (a busy program had one of those always due, and nothing else
// ran): Wifi_Poll() wakes it as often as the program's loop runs, and the
// update reads and sends a few packets a time, each with interrupts off.

static int8_t wifi_thread_priority = 10;
static void *wifi_thread_stack = NULL;
static size_t wifi_thread_stack_size = 4 * 1024;
static bool wifi_thread_made = false;

static void *volatile wifi_init_request = NULL;
static volatile bool wifi_deinit_request = false;

void Wifi_SetThreadConfig(int8_t priority, void *stack, size_t stack_size)
{
    wifi_thread_priority = priority;
    wifi_thread_stack = stack;
    if (stack_size != 0)
        wifi_thread_stack_size = stack_size;
}

void Wifi_Request_Deinit(void)
{
    wifi_deinit_request = true;
}

void Wifi_Poll(void)
{
    volatile Wifi_MainStruct *data = WifiData;
    if (data != NULL && (data->reqFlags & WFLAG_REQ_DSI_MODE))
        cothread_send_signal(DSWIFI_ARM7_SIGNAL);
}

// Requires the data returned by the ARM9 WiFi init call.
//
// The data returned by the ARM9 Wifi_Init() function must be passed to the ARM7
// and then given to this function.
//
// This function also enables power to the WiFi system, which will shorten
// battery life.
u8 wifi_init_stage7 = WIFI_INIT_STAGE_NONE;

int Wifi_GetInitStage7(void)
{
    return wifi_init_stage7;
}

// One init: false if it may not go on.
static bool Wifi_InitHere(void *wifidata)
{
    WifiData = (Wifi_MainStruct *)wifidata;

    // Before anything else: were the two halves built from the same library?
    //
    // Everything from here on reads a structure both CPUs have to agree on, and
    // a core built against a different version reads the wrong fields without
    // ever failing to link. Stopping here turns that into a number the ARM9 can
    // report, which is the difference between a message and an afternoon.
    if (WifiData->abi != WIFI_SHARED_ABI)
    {
        WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_ABI_MISMATCH);
        return false;
    }

    // Record how far we get: if any of the calls below doesn't return (they run
    // with interrupts off), this byte is the only way to find out where it
    // stopped. See WIFI_INIT_STAGE_* in wifi_shared.h.
    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_ENTERED);

    Wifi_RandomAddEntropy(time(NULL));

    int oldIME = enterCriticalSection();
    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
        Wifi_TWL_Init();
    else
        Wifi_NTR_Init();
    leaveCriticalSection(oldIME);

    WLOG_PUTS("W: ARM7 ready\n");
    WLOG_FLUSH();

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_READY);
    WifiData->flags7 |= WFLAG_ARM7_ACTIVE;
    return true;
}

static int Wifi_Update_Thread(void *arg)
{
    (void)arg;

    while (1)
    {
        // Until the ARM9 asks for an init.
        void *wifidata;
        while ((wifidata = wifi_init_request) == NULL)
            cothread_yield_irq_or_signal(IRQ_VBLANK, DSWIFI_ARM7_SIGNAL);
        wifi_init_request = NULL;
        wifi_deinit_request = false;

        if (!Wifi_InitHere(wifidata))
            continue;

        while (!wifi_deinit_request)
        {
            if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
            {
                // (Its own critical sections, one a packet.)
                Wifi_TWL_Update();
            }
            else
            {
                int oldIME = enterCriticalSection();
                Wifi_NTR_Update();
                leaveCriticalSection(oldIME);
            }

            // DS mode: each VBlank and each ARM9 sync, as from the interrupts
            // before. DSi mode: those and each Wifi_Poll().
            cothread_yield_irq_or_signal(IRQ_VBLANK, DSWIFI_ARM7_SIGNAL);
        }

        int oldIME = enterCriticalSection();
        Wifi_Deinit();
        leaveCriticalSection(oldIME);
    }

    return 0;
}

void Wifi_Init(void *wifidata)
{
    WLOG_PUTS("W: ARM7 Init\n");
    WLOG_FLUSH();

    wifi_init_request = wifidata;

    if (!wifi_thread_made)
    {
        cothread_t thread_id;
        if (wifi_thread_stack != NULL)
            thread_id = cothread_create_priority_manual(Wifi_Update_Thread, NULL,
                                                        wifi_thread_stack, wifi_thread_stack_size,
                                                        wifi_thread_priority, COTHREAD_DETACHED);
        else
            thread_id = cothread_create_priority(Wifi_Update_Thread, NULL, wifi_thread_stack_size,
                                                 wifi_thread_priority, COTHREAD_DETACHED);
        if (thread_id == -1)
            libndsCrash("DSWiFi thread creation failed");
        wifi_thread_made = true;
    }

    cothread_send_signal(DSWIFI_ARM7_SIGNAL);
}

// This function cuts power to the WiFi system. After this WiFi will be unusable
// until Wifi_Init() is called again.
void Wifi_Deinit(void)
{
    WLOG_PUTS("W: Stopping WiFi\n");
    WLOG_FLUSH();

    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
        Wifi_TWL_Deinit();
    else
        Wifi_NTR_Deinit();

    WLOG_PUTS("W: WiFi stopped\n");
    WLOG_FLUSH();

    // Tell the ARM9 that the ARM7 is now idle
    WifiData->flags7 &= ~WFLAG_ARM7_ACTIVE;
    WifiData = NULL;
}

void Wifi_Start(void)
{
    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
        Wifi_TWL_Start();
    else
        Wifi_NTR_Start();
}

void Wifi_Stop(void)
{
    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
        Wifi_TWL_Stop();
    else
        Wifi_NTR_Stop();
}

void Wifi_SetupFilterMode(Wifi_FilterMode mode)
{
    if (WifiData->reqFlags & WFLAG_REQ_DSI_MODE)
        Wifi_TWL_SetupFilterMode(mode);
    else
        Wifi_NTR_SetupFilterMode(mode);
}
