// SPDX-License-Identifier: CC0-1.0
//
// SPDX-FileContributor: Antonio Niño Díaz, 2025

// ARM7 core of the Download Play host demo.
//
// This is the default ARM7 core of BlocksDS without the parts that this demo
// doesn't need. It is built here instead of using one of the prebuilt cores
// that come with BlocksDS so that it uses the same copy of DSWiFi as the ARM9.

#include <nds.h>

#include <dswifi7.h>

volatile bool exit_loop = false;

void power_button_callback(void)
{
    exit_loop = true;
}

void vblank_handler(void)
{
    inputGetAndSend();
    Wifi_Update();
}



// Boot progress, reported over IPCSYNC rather than the FIFO.
//
// The FIFO is the thing under suspicion, and the ARM7 console needs the FIFO to
// work, so a console that stops early has no way to say so. IPCSYNC is four
// bits in each direction, always readable by the other CPU, and libnds only
// uses it when a program exits. The ARM9 reads it back with IPC_GetSync().
//
// IPC_SendSync() also raises an interrupt in the other CPU, which is not wanted
// here, so write the register directly and leave the other bits alone.
#define ARM7_MARK_MAIN          1  // main() entered
#define ARM7_MARK_SETTINGS      2  // readUserSettings() and touchInit() done
#define ARM7_MARK_IRQ           3  // irqInit() done
#define ARM7_MARK_FIFO          4  // fifoInit() done
#define ARM7_MARK_HANDLERS      5  // FIFO handlers installed
#define ARM7_MARK_LOOP          6  // main loop reached
#define ARM7_MARK_ALIVE         14 // Alternated every frame with the probe
                                   // result below, so that the ARM9 sees both
                                   // that the ARM7 is running and what it got

static inline void Arm7Mark(unsigned int code)
{
    REG_IPC_SYNC = (REG_IPC_SYNC & 0xF0FF) | ((code & 0x0F) << 8);
}

// A message the ARM9 sends on a channel nothing else uses, to find out whether
// ARM9-to-ARM7 delivery works at all.
//
// Everything measured so far says the words leave the ARM9 and the hardware
// FIFO empties, so the ARM7 reads them, and yet no handler ever runs. This
// separates "no message of any kind is delivered" from "delivery works and
// something about the DSWiFi channel in particular doesn't", which are very
// different problems. Both message kinds are tried because it is an address
// message that DSWiFi and the ARM7 console both depend on.
#define PROBE_CHANNEL       FIFO_USER_01

#define PROBE_GOT_VALUE32   1
#define PROBE_GOT_ADDRESS   2

static volatile unsigned int probe_got = 0;

static void ProbeValue32Handler(u32 value, void *userdata)
{
    (void)value;
    (void)userdata;

    probe_got |= PROBE_GOT_VALUE32;
}

static void ProbeAddressHandler(void *address, void *userdata)
{
    (void)address;
    (void)userdata;

    probe_got |= PROBE_GOT_ADDRESS;
}

// IPCFIFOCNT as the previous program left it, and how many words were already
// waiting in the receive FIFO before libnds looked at it.
static u16 inherited_fifo_cr = 0;
static int inherited_words = 0;

// Empty the IPC receive FIFO before libnds arms it, and count what was there.
//
// The count is the measurement this build exists for. libnds's
// fifoProcessRxBuffer() reads the word at the head of its receive queue as a
// message header; a word that claims to be a data message of a length that
// never arrives makes it "try later" without advancing the head, and every
// message behind it then waits for ever. The hardware FIFO still drains, so the
// other CPU sees a perfectly healthy register, and the ARM7 keeps running --
// which is the state this console is in.
//
// Whether such a word can exist depends on which CPU reaches its fifoInit()
// first. The ARM9 clears this FIFO in its own, from initSystem(), so if the
// ARM9 gets there first there is nothing stale to find and a count of zero says
// the wedge came from somewhere else entirely.
//
// The FIFO has to be enabled to read it: while it is disabled a read returns
// the oldest word without removing it, which would spin here for ever. Bit 14
// is the error flag, write-one-to-clear, and libnds only ever writes zero to
// it, so acknowledge whatever the previous program left set.
static void DrainStaleFifo(void)
{
    inherited_fifo_cr = REG_IPC_FIFO_CR;

    REG_IPC_FIFO_CR = IPC_FIFO_ENABLE | IPC_FIFO_ERROR | IPC_FIFO_SEND_CLEAR;

    // The hardware FIFO holds 16 words. The limit is only there so that a
    // controller that never reports itself empty can't hang the console.
    while ((inherited_words < 32) && !(REG_IPC_FIFO_CR & IPC_FIFO_RECV_EMPTY))
    {
        (void)REG_IPC_FIFO_RX;
        inherited_words++;
    }

    REG_IPC_FIFO_CR = IPC_FIFO_ENABLE | IPC_FIFO_ERROR;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    // Before anything else, and in particular before libnds touches the FIFO.
    DrainStaleFifo();
    Arm7Mark(ARM7_MARK_MAIN);

    // Initialize sound hardware
    enableSound();

    // Read user information from the firmware (name, birthday, etc). DSWiFi
    // uses the name of the console to announce the host.
    readUserSettings();

    // Stop LED blinking
    ledBlink(LED_ALWAYS_ON);

    touchInit();

    Arm7Mark(ARM7_MARK_SETTINGS);

    irqInit();

    Arm7Mark(ARM7_MARK_IRQ);

    fifoInit();

    Arm7Mark(ARM7_MARK_FIFO);

    installSoundFIFO();
    installSystemFIFO(); // Sleep mode, storage, firmware...
    installWifiFIFO();
    if (isDSiMode())
        installCameraFIFO();

    fifoSetValue32Handler(PROBE_CHANNEL, ProbeValue32Handler, 0);
    fifoSetAddressHandler(PROBE_CHANNEL, ProbeAddressHandler, 0);

    Arm7Mark(ARM7_MARK_HANDLERS);

    // This sets a callback that is called when the power button in a DSi
    // console is pressed. It has no effect in a DS.
    setPowerButtonCB(power_button_callback);

    // Read current date from the RTC and setup an interrupt to update the time
    // regularly.
    initClockIRQTimer(LIBNDS_DEFAULT_TIMER_RTC);

    // Now that the FIFO is setup we can start sending input data to the ARM9.
    irqSet(IRQ_VBLANK, vblank_handler);
    irqEnable(IRQ_VBLANK);

    Arm7Mark(ARM7_MARK_LOOP);

    bool reported = false;
    bool alive = false;

    while (!exit_loop)
    {
        // Alternate the probe result with ARM7_MARK_ALIVE. Two different
        // readings mean the ARM7 is running and the other one is the result;
        // two equal readings mean it stopped at that mark.
        alive = !alive;

        // Report how far DSWiFi got on this CPU, taken from this CPU's own
        // memory. If the ARM9 reads zero from the struct they share while this
        // says otherwise, they are not looking at the same memory.
        unsigned int stage7 = Wifi_GetInitStage7();
        if (stage7 > 12)
            stage7 = 12;

        Arm7Mark(alive ? ARM7_MARK_ALIVE : (1 + stage7));

        if (!reported && consoleIsSetup())
        {
            consolePrintf("W: ARM7 CONSOLE ALIVE\n");
            consolePrintf("W: IPCFIFOCNT was %x\n", inherited_fifo_cr);
            consolePrintf("W: stale words %d\n", inherited_words);
            reported = true;
        }

        const uint16_t key_mask = KEY_SELECT | KEY_START | KEY_L | KEY_R;
        uint16_t keys_pressed = ~REG_KEYINPUT;

        if ((keys_pressed & key_mask) == key_mask)
            exit_loop = true;

        swiWaitForVBlank();
    }

    return 0;
}
