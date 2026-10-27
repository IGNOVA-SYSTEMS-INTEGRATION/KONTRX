/**
 * @file    timekeeper.h
 * @brief   Wall-clock time for logs and telemetry.
 *
 * The clock is kept in software: epoch = base + uptime. It is set by SNTP
 * (local server first, then internet) or, until SNTP succeeds, by the
 * browser at login. Before any source has set it, Time_Now() returns 0.
 */
#ifndef TIMEKEEPER_H
#define TIMEKEEPER_H

#include <stdint.h>

typedef enum {
    TIME_SRC_NONE    = 0,
    TIME_SRC_BROWSER = 1,
    TIME_SRC_SNTP    = 2
} TimeSource_t;

/* Log timestamps: an epoch value, or this flag | uptime seconds when the
 * clock was not set yet. */
#define TIME_UPTIME_FLAG 0x80000000UL

uint32_t     Time_Now(void);           /* Unix epoch seconds, 0 if unknown */
uint32_t     Time_LogStamp(void);
TimeSource_t Time_Source(void);
uint32_t     Time_LastSyncUptime(void);

/* Accepts a new time when its source is at least as good as the current
 * one, or the current one is older than an hour. Returns 1 if applied. */
int  Time_Set(uint32_t epoch, TimeSource_t src);

/* Non-blocking SNTP client; call often (IOWorker). Uses W5500 socket 7. */
void Time_Service(void);

#endif /* TIMEKEEPER_H */
