/**
 * @file    timekeeper.c
 * @brief   Software wall clock set by SNTP or the browser (see timekeeper.h).
 */
#include "timekeeper.h"
#include "freertos_tasks.h"
#include "modbus_dma.h"
#include "socket.h"
#include "wizchip_conf.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

#define NTP_SOCKET          7U          /* free W5500 socket (2 KB buffers) */
#define NTP_LOCAL_PORT      50123U
#define NTP_SERVER_PORT     123U
#define NTP_UNIX_OFFSET     2208988800UL /* 1900-01-01 to 1970-01-01 */
#define NTP_REPLY_TIMEOUT   1500U       /* ms per server */
#define NTP_RESYNC_S        3600U       /* re-sync hourly once set */
#define NTP_RETRY_S         30U         /* after every server failed */
#define TIME_MIN_VALID      1700000000UL /* anything earlier is not a real clock */
#define TIME_STALE_S        3600U

/* Fallback internet server when no local one answers: time.cloudflare.com
 * (anycast address). The broker host and the gateway are tried first. */
static const uint8_t s_internet_ntp[4] = {162, 159, 200, 1};

static volatile uint32_t s_epoch_at_boot;   /* epoch when uptime was 0 */
static volatile uint8_t  s_source = TIME_SRC_NONE;
static volatile uint32_t s_last_sync_uptime;

uint32_t Time_Now(void) {
    if (s_source == TIME_SRC_NONE) return 0;
    return s_epoch_at_boot + g_uptime_seconds;
}

uint32_t Time_LogStamp(void) {
    uint32_t now = Time_Now();
    return now ? now : (TIME_UPTIME_FLAG | (g_uptime_seconds & ~TIME_UPTIME_FLAG));
}

TimeSource_t Time_Source(void) { return (TimeSource_t)s_source; }
uint32_t Time_LastSyncUptime(void) { return s_last_sync_uptime; }

int Time_Set(uint32_t epoch, TimeSource_t src) {
    if (epoch < TIME_MIN_VALID || src == TIME_SRC_NONE) return 0;
    uint8_t stale = (g_uptime_seconds - s_last_sync_uptime) > TIME_STALE_S;
    if ((uint8_t)src < s_source && !stale) return 0;

    uint8_t first = (s_source == TIME_SRC_NONE);
    s_epoch_at_boot = epoch - g_uptime_seconds;
    s_source = (uint8_t)src;
    s_last_sync_uptime = g_uptime_seconds;
    if (first) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Clock set from %s", (src == TIME_SRC_SNTP) ? "SNTP" : "browser");
        Log_Event("SYS", msg);
    }
    return 1;
}

/* ---------------------------------------------------------------------- */

typedef enum { NTP_IDLE = 0, NTP_WAIT_REPLY } NtpState_t;

static NtpState_t s_state = NTP_IDLE;
static uint8_t    s_server_idx;
static uint32_t   s_sent_tick;
static uint32_t   s_next_try_uptime;

static int Parse_IPv4(const char *str, uint8_t *ip) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(str, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return 0;
    if (a > 255U || b > 255U || c > 255U || d > 255U) return 0;
    ip[0] = (uint8_t)a; ip[1] = (uint8_t)b; ip[2] = (uint8_t)c; ip[3] = (uint8_t)d;
    return 1;
}

/* Server list: MQTT broker host (local), gateway (local), internet. */
static int Server_At(uint8_t idx, uint8_t *ip) {
    switch (idx) {
        case 0: {
            char broker[64];
            memcpy(broker, (const void *)sharedConfig.mqtt_broker, sizeof(broker));
            broker[sizeof(broker) - 1U] = '\0';
            return Parse_IPv4(broker, ip);
        }
        case 1: {
            wiz_NetInfo ni;
            wizchip_getnetinfo(&ni);
            memcpy(ip, ni.gw, 4);
            return (ip[0] | ip[1] | ip[2] | ip[3]) != 0U;
        }
        case 2:
            memcpy(ip, s_internet_ntp, 4);
            return 1;
        default:
            return 0;
    }
}

static int Send_Request(uint8_t idx) {
    uint8_t ip[4];
    if (!Server_At(idx, ip)) return 0;
    if (socket(NTP_SOCKET, Sn_MR_UDP, NTP_LOCAL_PORT, 0) != NTP_SOCKET) return 0;

    uint8_t req[48];
    memset(req, 0, sizeof(req));
    req[0] = 0x1B;   /* LI 0, version 3, mode 3 (client) */
    if (sendto(NTP_SOCKET, req, sizeof(req), ip, NTP_SERVER_PORT) != (int32_t)sizeof(req)) {
        close(NTP_SOCKET);
        return 0;
    }
    s_sent_tick = osKernelGetTickCount();
    return 1;
}

/* Returns 1 with *epoch set when a valid server reply is in the socket. */
static int Read_Reply(uint32_t *epoch) {
    if (getSn_RX_RSR(NTP_SOCKET) < 48U + 8U) return 0;   /* 8 = W5500 UDP header */
    uint8_t buf[48];
    uint8_t from[4];
    uint16_t port;
    int32_t n = recvfrom(NTP_SOCKET, buf, sizeof(buf), from, &port);
    if (n < 48) return 0;

    uint8_t mode = buf[0] & 0x07U;
    uint8_t stratum = buf[1];
    uint32_t secs = ((uint32_t)buf[40] << 24) | ((uint32_t)buf[41] << 16) |
                    ((uint32_t)buf[42] << 8)  |  (uint32_t)buf[43];
    if (mode != 4U || stratum == 0U || stratum > 15U || secs <= NTP_UNIX_OFFSET) return 0;
    *epoch = secs - NTP_UNIX_OFFSET;
    return 1;
}

void Time_Service(void) {
    switch (s_state) {
        case NTP_IDLE: {
            uint8_t synced = (s_source == TIME_SRC_SNTP);
            if (g_uptime_seconds < s_next_try_uptime) return;
            if (synced && (g_uptime_seconds - s_last_sync_uptime) < NTP_RESYNC_S) return;
            while (s_server_idx < 3U && !Send_Request(s_server_idx)) s_server_idx++;
            if (s_server_idx >= 3U) {
                s_server_idx = 0;
                s_next_try_uptime = g_uptime_seconds + NTP_RETRY_S;
                return;
            }
            s_state = NTP_WAIT_REPLY;
            break;
        }
        case NTP_WAIT_REPLY: {
            uint32_t epoch = 0;
            if (Read_Reply(&epoch)) {
                close(NTP_SOCKET);
                Time_Set(epoch, TIME_SRC_SNTP);
                s_server_idx = 0;
                s_state = NTP_IDLE;
            } else if ((osKernelGetTickCount() - s_sent_tick) >= NTP_REPLY_TIMEOUT) {
                close(NTP_SOCKET);
                s_server_idx++;              /* next server on the next call */
                s_state = NTP_IDLE;
            }
            break;
        }
    }
}
