// SPDX-License-Identifier: CC0-1.0
//
// SPDX-FileContributor: Antonio Niño Díaz, 2025

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <nds.h>

#include "log.h"

// Text waiting to be written, as a ring so that the interrupt handler that
// drains the ARM7 log can keep appending while a flush is in progress. It has to
// be big enough to hold everything that piles up between two flushes.
// Everything logged while a client is being served has to fit in here, because
// writing to the card upsets the multiplayer frame timing and so only happens
// once the host is idle again. A transfer that overflowed this used to lose the
// end of itself silently, which is exactly the part worth reading.
#define LOG_BUFFER_SIZE     (64 * 1024)

// How much is copied out of the ring at a time. Small enough that holding off
// interrupts to copy it doesn't matter.
#define LOG_CHUNK_SIZE      512

// Candidate paths, in order. The demo can be started with either FAT or NitroFS
// as the default drive, so the drive is named explicitly first.
static const char *log_paths[] = { "fat:/dlplay_log.txt", "/dlplay_log.txt" };

static FILE *log_file = NULL;
static const char *log_path = NULL;

static char log_buffer[LOG_BUFFER_SIZE];
static size_t log_head = 0;
static size_t log_tail = 0;
static size_t log_used = 0;
static unsigned int log_dropped = 0;

// Appends bytes to the ring. Interrupts are held off because LogPutChar() is
// called from the handler that drains the ARM7 log, so it can land in the middle
// of a LogPrintf() from the main loop.
static void LogAppend(const char *text, size_t len)
{
    if (log_file == NULL)
        return;

    int oldIME = enterCriticalSection();

    size_t space = LOG_BUFFER_SIZE - log_used;

    if (len > space)
    {
        log_dropped += len - space;
        len = space;
    }

    for (size_t i = 0; i < len; i++)
    {
        log_buffer[log_head] = text[i];
        log_head = (log_head + 1) % LOG_BUFFER_SIZE;
    }

    log_used += len;

    leaveCriticalSection(oldIME);
}

bool LogInit(void)
{
    if (log_file != NULL)
        return true;

    for (size_t i = 0; i < sizeof(log_paths) / sizeof(log_paths[0]); i++)
    {
        log_file = fopen(log_paths[i], "w");
        if (log_file != NULL)
        {
            log_path = log_paths[i];
            return true;
        }
    }

    return false;
}

void LogPrintf(const char *fmt, ...)
{
    if (log_file == NULL)
        return;

    char line[256];

    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    if (len <= 0)
        return;

    // vsnprintf() reports what it would have written, which can be more than it
    // actually did.
    if (len > (int)sizeof(line) - 1)
        len = sizeof(line) - 1;

    LogAppend(line, len);
}

void LogPutChar(char c)
{
    LogAppend(&c, 1);
}

void LogFlush(void)
{
    if ((log_file == NULL) || (log_used == 0))
        return;

    while (1)
    {
        char chunk[LOG_CHUNK_SIZE];

        // Take a bounded piece out of the ring with interrupts off, then do the
        // slow write with them back on.
        int oldIME = enterCriticalSection();

        size_t len = log_used;
        if (len > sizeof(chunk))
            len = sizeof(chunk);

        for (size_t i = 0; i < len; i++)
        {
            chunk[i] = log_buffer[log_tail];
            log_tail = (log_tail + 1) % LOG_BUFFER_SIZE;
        }

        log_used -= len;

        leaveCriticalSection(oldIME);

        if (len == 0)
            break;

        fwrite(chunk, 1, len, log_file);
    }

    fflush(log_file);
}

void LogClose(void)
{
    if (log_file == NULL)
        return;

    LogFlush();

    fclose(log_file);
    log_file = NULL;
}

const char *LogPath(void)
{
    return log_path;
}

unsigned int LogDropped(void)
{
    return log_dropped;
}
