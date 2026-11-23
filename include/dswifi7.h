// SPDX-License-Identifier: MIT
//
// Copyright (C) 2005-2006 Stephen Stair - sgstair@akkit.org - http://www.akkit.org
// Copyright (C) 2025 Antonio Niño Díaz

/// @file dswifi7.h
///
/// @brief ARM7 header of DSWifi.

#ifndef DSWIFI7_H
#define DSWIFI7_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include <nds/ndstypes.h>

#include <dswifi_version.h>

/// Deprecated function with no effect.
__attribute__((deprecated))
static inline void Wifi_Update(void)
{
}

/// Sets up the library's ARM7 thread, which does all its work: the init, the
/// updates and the deinit the ARM9 asks for. Call before installWifiFIFO().
///
/// By default the thread has a 4 KB stack from the heap and priority 10, above
/// main()'s (0). A program whose own loop must not wait for the wireless (its
/// sound, say) gives it a lower priority than that loop's; the loop must then
/// yield (cothread_yield_irq(), not swiIntrWait()) for the thread to run.
///
/// @param priority
///     The thread's priority (-128 to 127, higher first).
/// @param stack
///     A stack of the program's own, or NULL for one from the heap.
/// @param stack_size
///     Its size in bytes (0: the default).
void Wifi_SetThreadConfig(int8_t priority, void *stack, size_t stack_size);

/// Wakes the library's thread for an update of the DSi's wireless.
///
/// In DSi mode the card is polled from the thread, never from a timer or its
/// interrupt, a few packets each update. The thread updates on each VBlank and
/// each ARM9 request, but traffic needs it more often: call this from the
/// program's loop as often as it runs (every couple of milliseconds). In DS
/// mode it does nothing (that radio's interrupt does its work).
void Wifi_Poll(void);

/// Setup system FIFO handler for WiFi library messages.
void installWifiFIFO(void);

/// How far the ARM7 got through its half of the initialization.
///
/// The same number the ARM9 reads with Wifi_GetInitFailStage(), but taken from
/// this CPU's own memory instead of the struct the two CPUs share. It is only
/// useful while working out why initialization failed: if the ARM9 reads zero
/// and this doesn't, the two CPUs disagree about where that struct is, which is
/// a different problem from the ARM7 never having started.
///
/// @return
///     The last step this CPU reached, or zero if it reached none.
int Wifi_GetInitStage7(void);

#ifdef __cplusplus
}
#endif

#endif // DSWIFI7_H
