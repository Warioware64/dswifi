// SPDX-License-Identifier: CC0-1.0
//
// SPDX-FileContributor: Antonio Niño Díaz, 2025

// Writes a copy of everything this demo reports to a file on the card, so that a
// run against a real console can be looked at afterwards instead of squinting at
// the screen while it happens.
//
// Text is collected in RAM and only written out at safe moments. Writing to the
// card blocks for milliseconds at a time, which is long enough to disturb the
// multiplayer frame timing that the protocol depends on, so nothing is written
// while a client is being served.

#ifndef DLPLAY_LOG_H__
#define DLPLAY_LOG_H__

#include <stdbool.h>

// Opens the log file. Returns true if it was opened, false if the card can't be
// written to, in which case every other function here does nothing.
bool LogInit(void);

// Appends a formatted line. Call this from the main loop, never from a callback.
void LogPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Appends one character. This is safe to call from an interrupt handler, and is
// what the hook that copies the ARM7 log uses.
void LogPutChar(char c);

// Writes whatever has been collected to the card. Call this from the main loop
// at a moment when a pause won't break anything.
void LogFlush(void);

// Writes out the last of the text and closes the file.
void LogClose(void);

// Returns the path of the log file, or NULL if there isn't one.
const char *LogPath(void);

// Number of characters dropped because the buffer filled up between flushes.
unsigned int LogDropped(void);

#endif // DLPLAY_LOG_H__
