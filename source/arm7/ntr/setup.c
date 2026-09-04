// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2025 Antonio Niño Díaz

#include "arm7/debug.h"
#include "arm7/ipc.h"
#include "arm7/setup.h"
#include "arm7/wfc.h"
#include "arm7/ntr/baseband.h"
#include "arm7/ntr/flash.h"
#include "arm7/ntr/interrupts.h"
#include "arm7/ntr/mac.h"
#include "arm7/ntr/registers.h"
#include "arm7/ntr/rf.h"
#include "arm7/ntr/setup.h"
#include "common/common_ntr_defs.h"
#include "common/random.h"

void Wifi_NTR_SetWepKey(void *wepkey, int wepmode)
{
    if (wepmode == WEPMODE_NONE)
        return;

    int len = Wifi_WepKeySizeFromMode(wepmode);

#if DSWIFI_LOGS
    WLOG_PUTS("W: WEP: ");
    char *c = wepkey;
    for (int i = 0; i < len; i++)
        WLOG_PRINTF("%x", c[i]);
    WLOG_PUTS("\n");
    WLOG_FLUSH();
#endif

    for (size_t i = 0; i < (WEP_KEY_MAX_SIZE / sizeof(u16)); i++)
    {
        W_WEPKEY_0[i] = 0;
        W_WEPKEY_1[i] = 0;
        W_WEPKEY_2[i] = 0;
        W_WEPKEY_3[i] = 0;
    }

    // Copy the WEP key carefully. The source array may not be aligned to 16 bit
    int src = 0;
    int dest = 0;
    while (src < len)
    {
        u16 value = ((u8 *)wepkey)[src++];
        if (src < len)
            value |= ((u8 *)wepkey)[src++] << 8;

        W_WEPKEY_0[dest] = value;
        W_WEPKEY_1[dest] = value;
        W_WEPKEY_2[dest] = value;
        W_WEPKEY_3[dest] = value;
        dest++;
    }
}

void Wifi_NTR_SetWepMode(int wepmode)
{
    if (wepmode < 0 || wepmode > 7)
        return;

    if (wepmode == WEPMODE_NONE)
        W_WEP_CNT = WEP_CNT_DISABLE;
    else
        W_WEP_CNT = WEP_CNT_ENABLE;

    if (wepmode == WEPMODE_NONE)
        wepmode = WEPMODE_64BIT;
    else if (wepmode == WEPMODE_64BIT_ASCII)
        wepmode = WEPMODE_64BIT;
    else if (wepmode == WEPMODE_128BIT_ASCII)
        wepmode = WEPMODE_128BIT;
    else if (wepmode == WEPMODE_152BIT_ASCII)
        wepmode = WEPMODE_152BIT;

    W_MODE_WEP = (W_MODE_WEP & ~MODE_WEP_KEYLEN_MASK)
               | (wepmode << MODE_WEP_KEYLEN_SHIFT);
}

void Wifi_NTR_SetSleepMode(int mode)
{
    if (mode > 3 || mode < 0)
        return;

    W_MODE_WEP = (W_MODE_WEP & ~MODE_WEP_SLEEP_MASK) | mode;
}

void Wifi_NTR_SetAssociationID(u16 aid)
{
    W_AID_FULL = aid;
    W_AID_LOW = aid & 0xF;
    WifiData->clients.curClientAID = aid & 0xF;
}

void Wifi_NTR_DisableTempPowerSave(void)
{
    W_POWER_TX &= ~2;
    W_POWER_048 = 0;
}

void Wifi_NTR_SetupTransferOptions(int rate, bool short_preamble)
{
    if (short_preamble)
        W_PREAMBLE |= 6;
    else
        W_PREAMBLE &= ~6;

    WifiData->maxrate7 = rate;

    u16 value = Wifi_FlashReadHWord(F_WIFI_CFG_058) + 0x202;

    if (rate == WIFI_TRANSFER_RATE_2MBPS)
    {
        value -= 0x6161;
        if (short_preamble)
            value -= 0x6060;
    }

    W_CONFIG_140 = value;
}

void Wifi_NTR_SetupFilterMode(Wifi_FilterMode mode)
{
    switch (mode)
    {
        case WIFI_FILTERMODE_IDLE:
            // Ignore all frames
            W_RXFILTER  = 0;
            W_RXFILTER2 = RXFILTER2_IGNORE_DS_DS | RXFILTER2_IGNORE_STA_STA
                        | RXFILTER2_IGNORE_DS_STA | RXFILTER2_IGNORE_DS_DS;
            break;

        case WIFI_FILTERMODE_SCAN:
            W_RXFILTER  =
                // Receive beacon frames and DS to STA frames
                RXFILTER_MGMT_BEACON_OTHER_BSSID |
                // Receive probe requests
                RXFILTER_MGMT_NONBEACON_OTHER_BSSID |
                RXFILTER_MGMT_NONBEACON_OTHER_BSSID_EX;
            W_RXFILTER2 = RXFILTER2_IGNORE_DS_DS | RXFILTER2_IGNORE_STA_STA
                        | RXFILTER2_IGNORE_STA_DS;
            break;

        case WIFI_FILTERMODE_INTERNET:
            // Receive retransmit frames, and DS to STA frames
            W_RXFILTER  = RXFILTER_MGMT_BEACON_OTHER_BSSID;
            W_RXFILTER2 = RXFILTER2_IGNORE_DS_DS | RXFILTER2_IGNORE_STA_STA
                        | RXFILTER2_IGNORE_STA_DS;
            break;

        case WIFI_FILTERMODE_MULTIPLAYER_HOST:
            // Receive retransmit and multiplayer frames, and STA to DS frames.
            //
            // Empty replies and MP ACKs have to be accepted. A client always
            // transmits something in its reply slot, but the reply is empty
            // until its software has queued data to send, so a client that
            // hasn't accepted what the host sent answers every frame with an
            // empty reply. Filtering those out makes such a client look exactly
            // like one that isn't answering at all, which is not a distinction
            // the host can afford to lose. Nintendo's firmware accepts both.
            //
            // Probe requests are sent to the broadcast BSSID, so the two
            // "non-beacon management frames from other BSSIDs" bits are needed
            // to receive them. Clients looking for a host may send them.
            W_RXFILTER  = RXFILTER_MGMT_BEACON_OTHER_BSSID
                        | RXFILTER_MGMT_NONBEACON_OTHER_BSSID
                        | RXFILTER_MGMT_NONBEACON_OTHER_BSSID_EX
                        | RXFILTER_MP_EMPTY_REPLY
                        | RXFILTER_MP_ACK;
            W_RXFILTER2 = RXFILTER2_IGNORE_DS_DS | RXFILTER2_IGNORE_STA_STA
                        | RXFILTER2_IGNORE_DS_STA;
            break;

        case WIFI_FILTERMODE_MULTIPLAYER_CLIENT:
            // Receive retransmit frames and DS to STA frames.
            //
            // The two multiplayer bits let a client see the answers the other
            // clients give and the acknowledgement the host sends afterwards.
            // Nothing in the library acts on either, but they are the only way
            // to watch what a console replies to a host: the hardware appears
            // to process multiplayer frames only while it is itself taking part
            // in the exchange, so a console that is merely listening never sees
            // them however its filter is set.
            W_RXFILTER  = RXFILTER_MGMT_BEACON_OTHER_BSSID
                        | RXFILTER_MP_ACK
                        | RXFILTER_MP_EMPTY_REPLY;
            // A reply travels from a station to the host, so ignoring that
            // direction would throw away the very frames the two bits above
            // were set for. The library drops them itself, they are here to be
            // watched.
            W_RXFILTER2 = RXFILTER2_IGNORE_DS_DS | RXFILTER2_IGNORE_STA_STA;
            break;

        case WIFI_FILTERMODE_PROMISCUOUS:
            // For watching an exchange between two other consoles.
            //
            // This is the multiplayer host filter, which is known to receive
            // both the frames a host polls with and the answers a client sends,
            // widened in the two ways that matter: bit 0 accepts broadcasts
            // whose BSSID isn't ours, which is every frame of somebody else's
            // session, and W_RXFILTER2 only ignores DS to DS, so both directions
            // of the exchange get through.
            //
            // Setting every bit doesn't work. GBATEK marks bits 1 to 6 as
            // "unknown, usually zero" and no firmware sets them, and clearing
            // W_RXFILTER2 entirely is a combination no firmware writes either;
            // doing both delivered nothing but beacons. The values here stay
            // within what real firmware uses (it writes 08h, 0Bh or 0Dh).
            //
            // Note that this still cannot see a frame addressed to another
            // console: the hardware filters on the destination address before
            // any of these bits apply, so the authentication and association
            // exchange between two other consoles is out of reach.
            W_RXFILTER  = RXFILTER_MGMT_BEACON_OTHER_BSSID
                        | RXFILTER_MP_ACK
                        | RXFILTER_MP_EMPTY_REPLY
                        | RXFILTER_MGMT_NONBEACON_OTHER_BSSID
                        | RXFILTER_MGMT_NONBEACON_OTHER_BSSID_EX;
            W_RXFILTER2 = RXFILTER2_IGNORE_DS_DS;
            break;
    }
}

void Wifi_NTR_WakeUp(void)
{
    W_POWER_US = 0;

    swiDelay(67109); // 8ms delay

    Wifi_BBPowerOn();

    // Unset and set bit 7 of register 1 to reset the baseband
    u32 i = Wifi_BBRead(1);
    Wifi_BBWrite(1, i & 0x7f);
    Wifi_BBWrite(1, i);

    swiDelay(335544); // 40ms delay

    Wifi_RFInit();
}

void Wifi_NTR_Shutdown(void)
{
    if (Wifi_FlashReadByte(F_RF_CHIP_TYPE) == 2)
        Wifi_RFWrite(0xC008);

    int a = Wifi_BBRead(REG_MM3218_EXT_GAIN);
    Wifi_BBWrite(REG_MM3218_EXT_GAIN, a | 0x3F);

    Wifi_BBPowerOff();

    W_POWER_US = 1;
}

static void Wifi_NTR_TxSetup(void)
{
    W_TXREQ_SET = TXBIT_LOC3 | TXBIT_LOC2 | TXBIT_LOC1;
}

static void Wifi_NTR_RxSetup(void)
{
    W_RXCNT = RXCNT_ENABLE_RX;

    W_RXBUF_BEGIN   = MAC_RXBUF_START_ADDRESS;
    W_RXBUF_WR_ADDR = MAC_RXBUF_START_OFFSET >> 1;

    W_RXBUF_END     = MAC_RXBUF_END_ADDRESS;
    W_RXBUF_READCSR = (W_RXBUF_BEGIN & 0x3FFF) >> 1;

    // The RX GAP is unreliable, disable it:
    //
    // "On the DS-Lite, after adding it to W_RXBUF_RD_ADDR, the W_RXBUF_GAPDISP
    // setting is destroyed (reset to 0000h) by hardware. The original DS leaves
    // W_RXBUF_GAPDISP intact."
    W_RXBUF_GAP     = 0;
    W_RXBUF_GAPDISP = 0;

    // Enable reception of packages and clear RX buffer (copy W_RXBUF_WR_ADDR to
    // W_RXBUF_WRCSR).
    W_RXCNT = RXCNT_ENABLE_RX | RXCNT_EMPTY_RXBUF;
}

void Wifi_NTR_Start(void)
{
    int oldIME = enterCriticalSection();

    Wifi_Stop();

    // Wifi_WakeUp();

    W_WEP_CNT     = WEP_CNT_ENABLE;
    W_POST_BEACON = 0xFFFF;

    Wifi_NTR_SetAssociationID(0);

    W_US_COUNTCNT = 1;
    W_POWER_TX    = 0x0000;
    W_BSSID[0]    = 0x0000;
    W_BSSID[1]    = 0x0000;
    W_BSSID[2]    = 0x0000;

    Wifi_NTR_TxSetup();
    Wifi_NTR_RxSetup();

    W_RXCNT = RXCNT_ENABLE_RX;

#if 0
    switch (W_MODE_WEP & 7)
    {
        case 0: // infrastructure mode?
            W_IF = IRQ_ALL_BITS;
            W_IE = 0x003F;

            W_RXSTAT_OVF_IE  = 0x1FFF;
            // W_RXSTAT_INC_IE = 0x0400;
            W_TXSTATCNT      = 0;
            W_X_00A          = 0;
            W_US_COUNTCNT    = 0;
            W_MODE_RST       = 1;
            // SetStaState(0x40);
            break;

        case 1: // ad-hoc mode? -- beacons are required to be created!
            W_IF = 0xFFF; // TODO: Is this a bug?
            W_IE = 0x703F;

            W_RXSTAT_OVF_IE  = 0x1FFF;
            W_RXSTAT_INC_IE  = 0; // 0x400
            W_TXSTATCNT      = TXSTATCNT_IRQ_MP_ACK
                             | TXSTATCNT_IRQ_MP_CMD
                             | TXSTATCNT_IRQ_BEACON;
            W_X_00A          = 0;
            W_MODE_RST       = 1;
            // ??
            W_US_COMPARECNT  = 1;
            W_TXREQ_SET      = TXBIT_CMD;
            break;

        case 2: // DS comms mode?
#endif
    W_IF = IRQ_ALL_BITS;
    // W_IE = 0xE03F;
    W_IE = 0x40B3;

    W_RXSTAT_OVF_IE = 0x1FFF;
    W_RXSTAT_INC_IE = 0; // 0x68

    Wifi_SetupFilterMode(WIFI_FILTERMODE_IDLE);

    W_TXSTATCNT      = 0;
    W_X_00A          = 0;
    W_MODE_RST       = 1;

    // Arm the TSF compare if the firmware-inherited value is garbage.
    //
    // The beacon timeslot (TBTT) is generated from the US counter vs the
    // W_US_COMPARE registers, and this code has always trusted whatever
    // value the firmware left there. On DS-phat/lite that is reliably the
    // all-ones-except-low-10-bits pattern and TBTT works. A DSi booting a
    // flashcart in DS-compat mode, however, sometimes leaves junk here
    // (e.g. 0x0000031f9275d000 observed on real hardware) — TBTT then
    // never fires and host mode silently broadcasts no beacons at all:
    // Wifi_MultiplayerHostMode() reports success, but no other console can
    // ever see the room (intermittent across boots).
    //
    // Only rewrite a value that is neither zero nor already armed:
    // rewriting an armed value is pointless, and at least one emulator
    // (melonDS) reports the registers as zero while generating TBTT fine —
    // and stops delivering beacons if they are overwritten.
    {
        u16 c0 = W_US_COMPARE0;
        u16 c1 = W_US_COMPARE1;
        u16 c2 = W_US_COMPARE2;
        u16 c3 = W_US_COMPARE3;

        bool armed = (c0 == 0xFC00) && (c1 == 0xFFFF) &&
                     (c2 == 0xFFFF) && (c3 == 0xFFFF);
        bool zero  = (c0 | c1 | c2 | c3) == 0;

        if (!armed && !zero)
        {
            WLOG_PRINTF("W: arming TSF compare (was %x:%x:%x:%x)\n",
                        c3, c2, c1, c0);

            W_US_COUNT0 = 0;
            W_US_COUNT1 = 0;
            W_US_COUNT2 = 0;
            W_US_COUNT3 = 0;

            W_US_COMPARE0 = 0xFC00;
            W_US_COMPARE1 = 0xFFFF;
            W_US_COMPARE2 = 0xFFFF;
            W_US_COMPARE3 = 0xFFFF;
        }
    }

    W_US_COUNTCNT    = 1;
    W_US_COMPARECNT  = 1;
    // SetStaState(0x20);
#if 0
            break;

        case 3:
        case 4:
            break;
    }
#endif
    W_POWER_048 = 0;
    Wifi_NTR_DisableTempPowerSave();
    // W_TXREQ_SET = TXBIT_CMD;
    W_POWERSTATE |= 2;
    W_TXREQ_RESET = TXBIT_ALL;

    int i = 0xFA0;
    while (i != 0 && !(W_RF_PINS & 0x80))
        i--;

    WifiData->flags7 |= WFLAG_ARM7_RUNNING;

    leaveCriticalSection(oldIME);
}

void Wifi_NTR_Stop(void)
{
    int oldIME = enterCriticalSection();

    WifiData->flags7 &= ~WFLAG_ARM7_RUNNING;

    W_IE            = 0;
    W_MODE_RST      = 0;
    W_US_COMPARECNT = 0;
    W_US_COUNTCNT   = 0;
    W_TXSTATCNT     = 0;
    W_X_00A         = 0;
    W_TXBUF_BEACON  = TXBUF_BEACON_DISABLE;
    W_TXREQ_RESET   = TXBIT_ALL;
    W_TXBUF_RESET   = TXBIT_ALL;

    // Wifi_Shutdown();

    leaveCriticalSection(oldIME);
}

void Wifi_NTR_Init(void)
{
    WLOG_PUTS("W: Init (DS mode)\n");

    // Initialize NTR WiFi on DSi.
    if (isDSiMode())
    {
        gpioSetWifiMode(GPIO_WIFI_MODE_NTR);
        if (REG_GPIO_WIFI)
            swiDelay(5 * 134056); // 5 milliseconds
    }

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_NTR_ENTERED);

    // Cut power before enabling it, so the controller starts from a known state
    // rather than from whatever the last program left behind.
    //
    // This matters for a program that arrived over DS Download Play: it starts
    // with the wireless hardware still powered and in the middle of a multiplayer
    // session, and initialising on top of that leaves the baseband and the radio
    // mid-transaction. Their busy flags are cleared by the hardware alone, so the
    // console used to stop in one of those waits and never reach the point where
    // the library could say anything.
    powerOff(POWER_WIFI);
    swiDelay(5 * 134056); // 5 milliseconds

    powerOn(POWER_WIFI); // Enable power for the WiFi controller
    REG_WIFIWAITCNT =
        WIFI_RAM_N_10_CYCLES | WIFI_RAM_S_6_CYCLES | WIFI_IO_N_6_CYCLES | WIFI_IO_S_4_CYCLES;

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_POWERED);

    Wifi_FlashInitData();

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_FLASH_READ);

    // reset/shutdown wifi:
    W_MODE_RST = 0xFFFF;
    Wifi_NTR_Stop();
    Wifi_NTR_Shutdown(); // power off wifi

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_SHUTDOWN);

    WifiData->curChannel     = 1;
    WifiData->reqChannel     = 1;
    WifiData->curMode        = WIFIMODE_DISABLED;
    WifiData->maxrate7       = WIFI_TRANSFER_RATE_1MBPS;

    for (int i = 0; i < W_MACMEM_SIZE; i += 2)
        W_MACMEM(i) = 0;

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_MACMEM_CLEARED);

    // Load WFC data from flash
    Wifi_NTR_GetWfcSettings();
    if (isDSiMode())
    {
        // If this is a DSi, try to load the additional AP settings. However,
        // ignore all the APs that use WPA, only load open and WPE APs.
        Wifi_TWL_GetWfcSettings(false);
    }

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_WFC_READ);

    WLOG_PRINTF("W: %d valid AP found\n", WifiData->wfc_number_of_configs);
    WLOG_FLUSH();

    for (int i = 0; i < 3; i++)
        WifiData->MacAddr[i] = Wifi_FlashReadHWord(F_MAC_ADDRESS + i * 2);

    WLOG_PRINTF("W: MAC: %x:%x:%x:%x:%x:%x\n",
        WifiData->MacAddr[0] & 0xFF, (WifiData->MacAddr[0] >> 8) & 0xFF,
        WifiData->MacAddr[1] & 0xFF, (WifiData->MacAddr[1] >> 8) & 0xFF,
        WifiData->MacAddr[2] & 0xFF, (WifiData->MacAddr[2] >> 8) & 0xFF);

    W_IE = 0;
    Wifi_NTR_WakeUp();

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_WOKE_UP);

    Wifi_MacInit();
    Wifi_BBInit();

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_MAC_BB_INIT);

    // Set Default Settings
    W_MACADDR[0] = WifiData->MacAddr[0];
    W_MACADDR[1] = WifiData->MacAddr[1];
    W_MACADDR[2] = WifiData->MacAddr[2];

    W_TX_RETRYLIMIT = 7;
    Wifi_NTR_SetSleepMode(MODE_WEP_SLEEP_OFF);
    Wifi_NTR_SetWepMode(WEPMODE_NONE);

    Wifi_SetChannel(1);

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_CHANNEL_SET);

    Wifi_BBWrite(REG_MM3218_CCA, 0x00);
    Wifi_BBWrite(REG_MM3218_ENERGY_DETECTION_THRESHOLD, 0x1F);

    Wifi_RandomAddEntropy(W_RANDOM);

    // Setup WiFi interrupt after we have setup everything else
    irqSet(IRQ_WIFI, Wifi_Interrupt);
    irqEnable(IRQ_WIFI);

    WIFI_SET_INIT_STAGE(WIFI_INIT_STAGE_NTR_DONE);
}

void Wifi_NTR_Deinit(void)
{
    irqDisable(IRQ_WIFI);
    irqSet(IRQ_WIFI, NULL);

    Wifi_NTR_Stop();
    Wifi_NTR_Shutdown();

    powerOff(POWER_WIFI);
}
