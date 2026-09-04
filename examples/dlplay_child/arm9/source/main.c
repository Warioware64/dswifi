// SPDX-License-Identifier: CC0-1.0

// This example is meant to be sent to a console by examples/download_play_host,
// not started from a card. Once it is running it connects back to the host that
// sent it and the two exchange messages.
//
// A program delivered this way could not use wifi at all until recently: the
// library asked libnds for an uncached view of the memory it shares with the
// ARM7, and on a console started by Download Play the answer could be an address
// that isn't memory, so initialization never finished. That is fixed, and this
// example is what proves it.

#include <stdio.h>
#include <string.h>

#include <nds.h>
#include <dswifi9.h>
#include <dswifi_dlplay.h>

// What the loader leaves behind for a program it has just started.
//
// GBATEK documents this as the entry point information of DS Download Play, and
// says outright that "multiplayer games can use that info for communicating with
// each other after the upload".
#define MB_BOOT_INDICATOR   (*(vu16 *)0x027FFC40)
#define MB_BOOT_FROM_DLPLAY 2
#define MB_HOST_BSSID       ((vu16 *)0x027FFC46)
#define MB_HOST_GAME_ID     (*(vu32 *)0x027FFC4E)
#define MB_HOST_CHANNEL     (*(vu16 *)0x027FFC78)

// The 32 bytes the host chose to send with the program, left by the loader where
// Nintendo's MB_GetMultiBootDownloadParameter() reads them.
#define MB_USER_PARAM       ((const char *)0x027FFBE0)

static PrintConsole topScreen;

// What the host last said, copied out of the packet handler.
//
// The handler runs in an interrupt and must not call anything that takes a lock,
// so it does nothing but copy: printing happens in the main loop.
static volatile bool host_said_something = false;
static char host_message[DSWIFI_DLPLAY_APP_MAX_SIZE + 1];

static void FromHostPacketHandler(Wifi_MPPacketType type, int base, int len)
{
    if (type != WIFI_MPTYPE_CMD)
        return;

    char buffer[DSWIFI_DLPLAY_APP_MAX_SIZE];

    // Most of what the host sends is the transfer protocol talking to consoles
    // that are still being sent a program. This says whether the frame was meant
    // for the application at all.
    int size = Wifi_DlPlayAppReadMessage(base, len, buffer, sizeof(buffer));

    if (size <= 0)
        return;

    memcpy(host_message, buffer, size);
    host_message[size] = '\0';

    host_said_something = true;
}

// Fills in the host this program was sent by, from what the loader left behind.
//
// Returns false when the program wasn't started by Download Play, which is what
// happens if somebody runs it from a card. There is nothing to connect back to
// then.
static bool FindHost(Wifi_AccessPoint *ap)
{
    if (MB_BOOT_INDICATOR != MB_BOOT_FROM_DLPLAY)
        return false;

    memset(ap, 0, sizeof(*ap));

    for (int i = 0; i < 3; i++)
    {
        u16 half = MB_HOST_BSSID[i];

        ap->bssid[i * 2 + 0] = half & 0xFF;
        ap->bssid[i * 2 + 1] = (half >> 8) & 0xFF;
    }

    ap->channel = MB_HOST_CHANNEL;

    return true;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    defaultExceptionHandler();

    videoSetMode(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    consoleInit(&topScreen, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleSelect(&topScreen);

    printf("DSWiFi Download Play child\n");
    printf("==========================\n\n");

    Wifi_AccessPoint host;

    if (!FindHost(&host))
    {
        printf("Not started by Download Play.\n\n");
        printf("Send this program with the\n");
        printf("download_play_host example\n");
        printf("rather than running it from\n");
        printf("a card.\n");
        goto wait_exit;
    }

    printf("Sent by %02x:%02x:%02x:%02x:%02x:%02x\n",
           host.bssid[0], host.bssid[1], host.bssid[2],
           host.bssid[3], host.bssid[4], host.bssid[5]);
    printf("on channel %d, game %08X\n\n",
           host.channel, (unsigned int)MB_HOST_GAME_ID);

    // Anything the host chose to send along with the program.
    if (MB_USER_PARAM[0] != '\0')
        printf("Host said: %.31s\n\n", MB_USER_PARAM);

    printf("Initializing WiFi...\n");

    if (!Wifi_InitDefault(INIT_ONLY | WIFI_LOCAL_ONLY))
    {
        printf("Can't initialize WiFi! (stage %d)\n", Wifi_GetInitFailStage());
        goto wait_exit;
    }

    printf("Connecting back...\n");

    Wifi_MultiplayerFromHostSetPacketHandler(FromHostPacketHandler);

    if (Wifi_MultiplayerClientMode(DSWIFI_DLPLAY_REPLY_MAX_SIZE + 1) != 0)
    {
        printf("Can't switch to client mode.\n");
        goto wait_exit;
    }

    while (!Wifi_LibraryModeReady())
        swiWaitForVBlank();

    // The host is still where it was a moment ago, so there is nothing to scan
    // for: the address and channel above are enough to associate.
    Wifi_ConnectOpenAP(&host);

    int last_status = -1;
    unsigned int ticks = 0;

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();

        if (keysHeld() & KEY_START)
            break;

        int status = Wifi_AssocStatus();

        if (status != last_status)
        {
            last_status = status;

            consoleSetCursor(NULL, 0, 10);
            printf("Link: %-24s\n", ASSOCSTATUS_STRINGS[status]);
        }

        if (status != ASSOCSTATUS_ASSOCIATED)
            continue;

        // Say something roughly twice a second. A client answers in its own slot
        // of every frame the host sends, so there is no need to do it faster.
        if ((ticks % 30) == 0)
        {
            char message[DSWIFI_DLPLAY_REPLY_MAX_SIZE];

            snprintf(message, sizeof(message), "hi %03u", (ticks / 30) & 0x3FF);
            Wifi_DlPlayAppReply(message, strlen(message));
        }

        ticks++;

        if (host_said_something)
        {
            host_said_something = false;

            consoleSetCursor(NULL, 0, 12);
            printf("Host: %-24s\n", host_message);
        }
    }

    Wifi_DisableWifi();

wait_exit:
    printf("\nPress START to exit.\n");

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();

        if (keysHeld() & KEY_START)
            break;
    }

    return 0;
}
