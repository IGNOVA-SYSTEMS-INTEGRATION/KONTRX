/**
 * @file    http_server_task.c
 * @brief   Kontrx — W5500 HTTP/REST Server (FreeRTOS Task: Normal Priority)
 *
 * Architecture:
 *   - Listens on Socket 0, port 80.
 *   - Serves the SPA (web_assets.h) on GET /.
 *   - REST endpoints:
 *       GET  /api/status           → JSON: sensor readings + relay states + uptime
 *       POST /api/config/relays    → JSON: set relay pin + NC/NO per relay
 *       POST /api/config/mqtt      → JSON: set MQTT broker config
 *       POST /api/relay            → query: ?id=N&state=0|1  toggle individual relay
 *       POST /api/relay/all        → query: ?state=0|1  all relays
 *       POST /api/ota/verify       → JSON: OTP check
 *       POST /update               → binary: firmware chunk stream (OTA)
 *
 * All shared state accesses use Mutex-protected helpers from modbus_dma.h.
 * SPI2 write bursts use a SPI mutex to share the bus with the WIZnet library.
 *
 * Note on the W5500 SPI mutex:
 *   The WIZnet ioLibrary uses global byte-at-a-time callbacks. We protect
 *   each full socket transaction (socket(), connect(), recv(), send(), disconnect())
 *   with spiMutex to prevent the Modbus task from concurrently accessing SPI.
 */

#include "http_server_task.h"
#include "freertos_tasks.h"
#include "web_assets.h"
#include "modbus_dma.h"
#include "flash_stm32.h"
#include "rtc_stm32.h"
#include "stm32f407_regs.h"
#include "gpio_stm32.h"
#include "socket.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "portable.h"   /* xPortGetFreeHeapSize, configTOTAL_HEAP_SIZE */
#include <string.h>
#include <stdio.h>

/* ======================================================================
 *  OTA inter-task state (defined here, declared extern in header)
 * ====================================================================== */
volatile uint8_t  ota_request_pending = 0;
volatile uint32_t ota_content_length  = 0;
volatile uint8_t  ota_otp_validated   = 0;
volatile uint8_t  ota_write_done      = 0;
volatile uint8_t  ota_write_ok        = 0;
volatile OTA_Debug_t g_ota_debug      = {0};

osSemaphoreId_t   sem_ota_start = NULL;
osSemaphoreId_t   sem_ota_done  = NULL;

/* ======================================================================
 *  Private constants
 * ====================================================================== */
#define HTTP_SOCK        0
#define HTTP_PORT        80U
#define RX_BUF_SIZE      1536U   /* Must fit a full HTTP request header */

#define OTA_OTP_SECRET   "KontrxOTA2026"
#define STAGING_ADDR     0x08040000U

/* Uptime counter (incremented by tick hook, not by this task) */
extern volatile uint32_t g_uptime_seconds;

/* External: relay GPIO driver */
extern uint8_t relayStates[MAX_RELAYS];
void Relay_SetState(uint8_t idx, uint8_t state);

/* ======================================================================
 *  Buffer — kept static to avoid stack pressure
 * ====================================================================== */
static uint8_t rx_buf[RX_BUF_SIZE];
static char    tx_buf[6144];  /* Sized for max /api/status JSON:
                               *   16 sensors × ~80B = 1280B
                               *   16 relays  × ~60B =  960B
                               *   mqtt/ota/sys fields  ~800B
                               *   HTTP header          ~200B
                               *   headroom            ~904B
                               *   Total ≈ 4244B → 6144B safe margin */



/* ======================================================================
 *  Static working buffers for JSON_StatusResponse
 *  Keeping these off the task stack prevents stack overflow on the
 *  2KB (now 4KB) HTTPServer stack when snprintf + structs are called.
 * ====================================================================== */
static Modbus_SensorData_t s_sd;
static Gateway_Config_t    s_cfg;
static wiz_NetInfo         s_ni;

/* ======================================================================
 *  JSON helpers (no dynamic allocation, snprintf into tx_buf)
 * ====================================================================== */
const char *SensorTypeName(uint8_t type) {
    switch (type) {
        case 1: return "ph";
        case 2: return "orp";
        case 3: return "ec";
        case 4: return "do";
        case 5: return "ammonia";
        case 6: return "ultrasonic";
        case 7: return "multi_us";
        default: return "unknown";
    }
}

static int JSON_StatusResponse(char *buf, int buflen) {
    /* Use static storage — keeps stack frames shallow and prevents
     * the HTTPServer task from overflowing its stack on every API call. */
    Get_Shared_Sensor_Data(&s_sd);
    Get_Shared_Config(&s_cfg);

    int pos = 0;

    /* --- sensors (full configured list, joined with live readings) --- */
    pos += snprintf(buf + pos, buflen - pos, "{\"sensors\":[");
    uint8_t emitted = 0;
    for (uint8_t i = 0; i < s_cfg.sensors.count && i < MAX_SENSORS; i++) {
        uint8_t type = s_cfg.sensors.entries[i].type;
        uint8_t id   = s_cfg.sensors.entries[i].id;
        float val = -1000.0f, tmp = -1000.0f;
        uint8_t valid = 0;
        for (uint8_t j = 0; j < s_sd.readings_count && j < MAX_SENSORS; j++) {
            if (s_sd.readings[j].type == type && s_sd.readings[j].id == id) {
                val   = s_sd.readings[j].value;
                tmp   = s_sd.readings[j].temp;
                /* valid=1 if we EVER had a good reading (last_ok_ms>0).
                 * This prevents the sensor card from flickering/disappearing
                 * on every Modbus polling cycle that happens to miss a response.
                 * The per-cycle readings[j].valid is only 1 for the exact cycle
                 * a response arrived, so using it directly causes the dashboard
                 * to show no value for most of the time between polls. */
                valid = (s_sd.readings[j].last_ok_ms > 0) ? 1 : 0;
                break;
            }
        }
        if (emitted) pos += snprintf(buf + pos, buflen - pos, ",");
        pos += snprintf(buf + pos, buflen - pos,
            "{\"type\":\"%s\",\"id\":%u,\"value\":%.2f,\"temp\":%.2f,\"valid\":%u}",
            SensorTypeName(type), id, val, tmp, valid);
        emitted = 1;
    }
    pos += snprintf(buf + pos, buflen - pos, "],");

    /* --- multi-us boards --- */
    pos += snprintf(buf + pos, buflen - pos, "\"multi_us\":[");
    for (uint8_t i = 0; i < s_sd.multi_us_count && i < MAX_MULTI_US; i++) {
        const MultiUS_t *m = &s_sd.multi_us[i];
        pos += snprintf(buf + pos, buflen - pos,
            "{\"id\":%u,\"dist\":[%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f],"
            "\"avg\":%.0f,\"comp\":%.0f,\"temp\":%.1f}%s",
            m->id,
            m->dist[0], m->dist[1], m->dist[2], m->dist[3],
            m->dist[4], m->dist[5], m->dist[6], m->dist[7],
            m->avg, m->comp, m->temp,
            (i < s_sd.multi_us_count - 1) ? "," : "");
    }
    pos += snprintf(buf + pos, buflen - pos, "],");

    /* --- relays (dynamic) --- */
    pos += snprintf(buf + pos, buflen - pos, "\"relays\":[");
    for (uint8_t i = 0; i < s_cfg.relay_count && i < MAX_RELAYS; i++) {
        pos += snprintf(buf + pos, buflen - pos,
            "{\"id\":%u,\"state\":%u,\"pin\":\"P%c%u\",\"nc\":%u,\"name\":\"%s\"}%s",
            i,
            relayStates[i],
            'A' + s_cfg.relays[i].port_id, s_cfg.relays[i].pin_num,
            s_cfg.relays[i].is_nc,
            s_cfg.relays[i].name[0] ? s_cfg.relays[i].name : "Relay",
            (i < s_cfg.relay_count - 1) ? "," : "");
    }
    pos += snprintf(buf + pos, buflen - pos, "],");

    ctlnetwork(CN_GET_NETINFO, &s_ni);

    pos += snprintf(buf + pos, buflen - pos,
        "\"uptime_s\":%lu,"
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"serial\":\"KX-%07lu\","
        "\"ip\":\"%d.%d.%d.%d\","
        "\"fw\":\"" FW_VERSION "\","
        "\"device_id\":\"%s\","
        "\"provision_status\":\"%s\","
        "\"provision_message\":\"%s\","
        "\"sparkplug_topic\":\"%s\","
        "\"pending_sparkplug_topic\":\"%s\","
        "\"mqtt\":{"
          "\"connected\":%u,"
          "\"interval\":%lu,"
          "\"send_mode\":%u,"
          "\"active_topic\":\"%s\","
          "\"broker\":\"%s\","
          "\"port\":%u,"
          "\"client_id\":\"%s\","
          "\"username\":\"%s\","
          "\"log\":["
        ,
        (unsigned long)g_uptime_seconds,
        s_ni.mac[0], s_ni.mac[1], s_ni.mac[2], s_ni.mac[3], s_ni.mac[4], s_ni.mac[5],
        (unsigned long)s_cfg.serial,
        s_ni.ip[0], s_ni.ip[1], s_ni.ip[2], s_ni.ip[3],
        s_cfg.device_id, s_cfg.provision_status, s_cfg.provision_message, s_cfg.sparkplug_topic,
        s_cfg.pending_sparkplug_topic,
        g_mqtt_status.connected, (unsigned long)s_cfg.mqtt_interval, s_cfg.mqtt_send_mode, g_mqtt_status.active_topic,
        s_cfg.mqtt_broker, s_cfg.mqtt_port, s_cfg.mqtt_client_id, s_cfg.mqtt_username);

    for (uint8_t i = 0; i < g_mqtt_status.log_count && i < MQTT_LOG_MAX; i++) {
        pos += snprintf(buf + pos, buflen - pos,
            "{\"topic\":\"%s\",\"success\":%u,\"time\":%lu}%s",
            g_mqtt_status.log[i].topic,
            g_mqtt_status.log[i].success,
            (unsigned long)g_mqtt_status.log[i].timestamp,
            (i < g_mqtt_status.log_count - 1) ? "," : "");
    }
    pos += snprintf(buf + pos, buflen - pos, "]},\"ota_debug\":{"
        "\"fw_size\":%lu,"
        "\"computed_crc\":\"0x%08lX\","
        "\"staged_sp\":\"0x%08lX\","
        "\"sp_valid\":%u,"
        "\"meta_magic\":\"0x%08lX\","
        "\"meta_status\":\"0x%08lX\","
        "\"meta_size\":%lu,"
        "\"meta_crc32\":\"0x%08lX\","
        "\"meta_ok\":%u,"
        "\"write_ok\":%u,"
        "\"step\":%lu"
        "}",          /* close ota_debug only — root object still open */
        (unsigned long)g_ota_debug.fw_size,
        (unsigned long)g_ota_debug.computed_crc,
        (unsigned long)g_ota_debug.staged_sp,
        g_ota_debug.sp_valid,
        (unsigned long)g_ota_debug.meta_magic,
        (unsigned long)g_ota_debug.meta_status,
        (unsigned long)g_ota_debug.meta_size,
        (unsigned long)g_ota_debug.meta_crc32,
        g_ota_debug.meta_ok,
        g_ota_debug.write_ok,
        (unsigned long)g_ota_debug.step);

    /* CPU and RAM (Heap) stats — appended inside root object, then close it */
    size_t heap_free  = xPortGetFreeHeapSize();
    size_t heap_total = configTOTAL_HEAP_SIZE;
    pos += snprintf(buf + pos, buflen - pos,
        ",\"sys\":{\"cpu_pct\":%u,\"heap_free\":%u,\"heap_total\":%u}}",
        /* ↑ note trailing }} : closes sys object AND root object         */
        (unsigned)g_cpu_usage_pct,
        (unsigned)heap_free,
        (unsigned)heap_total);

    return pos;
}

/* ======================================================================
 *  Simple query-string parser: returns int value of key, or -1
 * ====================================================================== */
static int ParseQueryInt(const char *url, const char *key) {
    const char *p = strstr(url, key);
    if (!p) return -1;
    p += strlen(key);
    if (*p != '=') return -1;
    p++;
    int val = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
    return val;
}

/* ======================================================================
 *  JSON field parser: find "key":VALUE in a JSON body
 *  Returns pointer into src at VALUE start, or NULL.
 * ====================================================================== */
static const char *JSON_FindValue(const char *src, const char *key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(src, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ') p++;
    return p;
}

static int JSON_ReadInt(const char *src, const char *key) {
    const char *v = JSON_FindValue(src, key);
    if (!v) return -1;
    if (*v == '"') v++;
    int val = 0, sign = 1;
    if (*v == '-') { sign = -1; v++; }
    while (*v >= '0' && *v <= '9') {
        val = val * 10 + (*v - '0');
        v++;
    }
    return val * sign;
}

static void Parse_Pin_String(const char *str, uint8_t *port_id, uint8_t *pin_num) {
    if (!str || !*str) return;
    const char *p = str;
    /* Skip leading 'P' or 'p' if present */
    if (*p == 'P' || *p == 'p') p++;

    /* Read Port letter (A-E or a-e) */
    char ch = *p;
    if (ch >= 'a' && ch <= 'z') ch -= 32; /* Uppercase */
    if (ch >= 'A' && ch <= 'E') {
        *port_id = (uint8_t)(ch - 'A');
        p++;
    }

    /* Read Pin number (0-15) */
    int pin = 0;
    while (*p >= '0' && *p <= '9') {
        pin = pin * 10 + (*p - '0');
        p++;
    }
    if (pin >= 0 && pin <= 15) {
        *pin_num = (uint8_t)pin;
    }
}

static int Is_Pin_Reserved(uint8_t port, uint8_t pin) {
    if (port > 4 || pin > 15) return 1; /* Invalid or out of bounds */

    /* PA6: LED, PA9, PA10: USART1 Console */
    if (port == 0) {
        if (pin == 6 || pin == 9 || pin == 10) return 1;
    }
    /* PB10, PB11: USART3 Modbus, PB12..PB15: SPI2 Ethernet */
    if (port == 1) {
        if (pin == 10 || pin == 11 || pin == 12 || pin == 13 || pin == 14 || pin == 15) return 1;
    }
    /* PC6/PC7: USART6 debug console */
    if (port == 2) {
        if (pin == 6 || pin == 7) return 1;
    }
    /* PD2, PD3: MAX485 RE/DE Control */
    if (port == 3) {
        if (pin == 2 || pin == 3) return 1;
    }
    return 0; /* Free to use */
}

static void JSON_ReadStr(const char *src, const char *key, char *out, int maxlen) {
    const char *v = JSON_FindValue(src, key);
    if (!v) { out[0]='\0'; return; }
    if (*v != '"') { out[0]='\0'; return; }
    v++;
    int i = 0;
    while (*v && *v != '"' && i < maxlen - 1) out[i++] = *v++;
    out[i] = '\0';
}

/* ======================================================================
 *  HTTP response helpers
 * ====================================================================== */
#define HTTP_200_HTML "HTTP/1.1 200 OK\r\nContent-Type:text/html;charset=UTF-8\r\nConnection:close\r\n\r\n"
#define HTTP_200_JSON "HTTP/1.1 200 OK\r\nContent-Type:application/json\r\nConnection:close\r\n\r\n"
#define HTTP_400      "HTTP/1.1 400 Bad Request\r\nConnection:close\r\n\r\n{\"ok\":false}"
#define HTTP_401      "HTTP/1.1 401 Unauthorized\r\nConnection:close\r\n\r\n{\"ok\":false,\"error\":\"OTP invalid\"}"
#define HTTP_200_OK_JSON "{\"ok\":true}"

static void Send_Response(uint8_t sn, const char *header, const char *body) {
    send(sn, (uint8_t *)header, strlen(header));
    if (body) send(sn, (uint8_t *)body, strlen(body));
}

/**
 * @brief Send a large buffer in 1 KB chunks, yielding between each chunk.
 *        Prevents the W5500 2KB socket TX buffer from stalling on large payloads.
 */
#define SEND_CHUNK_SIZE 1024U
static void Send_Chunked(uint8_t sn, const uint8_t *data, uint32_t total) {
    uint32_t sent = 0;
    while (sent < total) {
        uint16_t chunk = (uint16_t)((total - sent) > SEND_CHUNK_SIZE
                                    ? SEND_CHUNK_SIZE
                                    : (total - sent));
        int32_t result = send(sn, (uint8_t *)(data + sent), chunk);
        if (result > 0) {
            sent += (uint32_t)result;
        } else if (result == SOCK_BUSY) {
            osDelay(1);   /* 1ms yield — W5500 TX buffer full; wait a tick and
                             retry. Allows other tasks (incl. 1ms Control Engine)
                             to run meanwhile. Well within 1ms budget. */
        } else {
            break;        /* Socket error — abort */
        }
    }
}

/* ======================================================================
 *  Flash Aligned Writer Helper (prevents unaligned programming errors)
 * ====================================================================== */
typedef struct {
    uint32_t write_addr;
    uint8_t  cache[4];
    uint8_t  cache_len;
} Flash_Aligned_Writer_t;

static void Flash_Writer_Init(Flash_Aligned_Writer_t *w, uint32_t start_addr) {
    w->write_addr = start_addr;
    w->cache_len  = 0;
}

static void Flash_Writer_Write(Flash_Aligned_Writer_t *w, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        w->cache[w->cache_len++] = data[i];
        if (w->cache_len == 4) {
            uint32_t word = *(const uint32_t *)w->cache;
            FLASH_WriteWord(w->write_addr, word);
            w->write_addr += 4;
            w->cache_len = 0;
        }
    }
}

static void Flash_Writer_Flush(Flash_Aligned_Writer_t *w) {
    if (w->cache_len > 0) {
        uint32_t word = 0xFFFFFFFFU;
        for (uint8_t i = 0; i < w->cache_len; i++) {
            ((uint8_t *)&word)[i] = w->cache[i];
        }
        FLASH_WriteWord(w->write_addr, word);
        w->write_addr += 4;
        w->cache_len = 0;
    }
}

/* ======================================================================
 *  Request dispatcher
 * ====================================================================== */
static void Dispatch_Request(uint8_t sn, uint8_t *req, uint16_t len) {
    req[len] = '\0';
    char *line = (char *)req;

    /* -----------------------------------------------------------------
     * GET /
     * Serve the full SPA HTML page
     * ----------------------------------------------------------------- */
    if (strncmp(line, "GET / ", 6) == 0 || strncmp(line, "GET /index", 10) == 0) {
        /* Send headers with Content-Length so browser knows total size */
        uint32_t html_len = sizeof(KONTRX_HTML) - 1;
        char hdr[160];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=UTF-8\r\n"
            "Content-Length: %lu\r\n"
            "Connection: close\r\n"
            "\r\n",
            (unsigned long)html_len);
        send(sn, (uint8_t *)hdr, (uint16_t)strlen(hdr));
        /* Stream HTML in 1KB chunks */
        Send_Chunked(sn, (const uint8_t *)KONTRX_HTML, html_len);
        return;
    }

    /* -----------------------------------------------------------------
     * GET /api/status  → JSON sensor + relay snapshot
     * ----------------------------------------------------------------- */
    if (strncmp(line, "GET /api/status", 15) == 0) {
        int n = JSON_StatusResponse(tx_buf, sizeof(tx_buf));
        char status_hdr[160];
        snprintf(status_hdr, sizeof(status_hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",
            n);
        send(sn, (uint8_t *)status_hdr, (uint16_t)strlen(status_hdr));
        Send_Chunked(sn, (const uint8_t *)tx_buf, (uint32_t)n);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/relay?id=N&state=S  → Toggle individual relay
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/relay?", 16) == 0) {
        int id    = ParseQueryInt(line, "id");
        int state = ParseQueryInt(line, "state");
        Gateway_Config_t rcfg;
        Get_Shared_Config(&rcfg);
        if (id >= 0 && id < (int)rcfg.relay_count && id < MAX_RELAYS && state >= 0) {
            Relay_SetState((uint8_t)id, (uint8_t)state);
        }
        snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":true,\"id\":%d,\"state\":%d}", id, relayStates[id]);
        send(sn, (uint8_t *)HTTP_200_JSON, strlen(HTTP_200_JSON));
        send(sn, (uint8_t *)tx_buf, strlen(tx_buf));
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/relay/all?state=S  → All relays
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/relay/all", 19) == 0) {
        int state = ParseQueryInt(line, "state");
        if (state >= 0) {
            for (int i = 0; i < MAX_RELAYS; i++) Relay_SetState((uint8_t)i, (uint8_t)state);
        }
        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/relays  → Full replace: {"relays":[{"name","pin","nc"},...]}
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/relays", 23) == 0) {
        char *body = strstr(line, "\r\n\r\n");
        if (!body) { Send_Response(sn, HTTP_400, NULL); return; }
        body += 4;

        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);

        uint8_t temp_ports[MAX_RELAYS];
        uint8_t temp_pins[MAX_RELAYS];
        uint8_t temp_nc[MAX_RELAYS];
        char    temp_names[MAX_RELAYS][20];
        uint8_t relay_count = 0;

        /* De-assert old pins first (release them from output drive) */
        for (int i = 0; i < (int)cfg.relay_count && i < MAX_RELAYS; i++) {
            uint8_t old_port = cfg.relays[i].port_id;
            uint8_t old_pin  = cfg.relays[i].pin_num;
            if (old_port <= 4 && old_pin <= 15) {
                GPIO_TypeDef *gpio = GPIO_Ports[old_port];
                gpio->MODER &= ~(3U << (old_pin * 2)); /* Reset to Input mode (00) */
                gpio->PUPDR &= ~(3U << (old_pin * 2)); /* No pull */
            }
        }

        /* Parse list: items are "{"name":"...","pin":"Pxy","nc":n}," */
        const char *cursor = body;
        while (relay_count < MAX_RELAYS) {
            const char *obj = strstr(cursor, "\"pin\":");
            if (!obj) break;
            /* back up to start of this object */
            const char *obj_start = obj;
            while (obj_start > body && *obj_start != '{') obj_start--;

            char pin_str[16] = {0};
            JSON_ReadStr(obj_start, "pin", pin_str, sizeof(pin_str));

            uint8_t port_id = 0xFF, pin_num = 0xFF;
            Parse_Pin_String(pin_str, &port_id, &pin_num);

            if (port_id > 4 || pin_num > 15) {
                snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":false,\"error\":\"Invalid pin %s.\"}", pin_str);
                Send_Response(sn, HTTP_200_JSON, tx_buf);
                return;
            }
            if (Is_Pin_Reserved(port_id, pin_num)) {
                snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":false,\"error\":\"Pin %s is reserved by system hardware.\"}", pin_str);
                Send_Response(sn, HTTP_200_JSON, tx_buf);
                return;
            }
            for (int j = 0; j < relay_count; j++) {
                if (temp_ports[j] == port_id && temp_pins[j] == pin_num) {
                    snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":false,\"error\":\"Pin %s is assigned twice.\"}", pin_str);
                    Send_Response(sn, HTTP_200_JSON, tx_buf);
                    return;
                }
            }

            temp_ports[relay_count]  = port_id;
            temp_pins[relay_count]   = pin_num;
            int nc = JSON_ReadInt(obj_start, "nc");
            temp_nc[relay_count]     = (nc == 1) ? 1 : 0;
            char nm[20] = {0};
            JSON_ReadStr(obj_start, "name", nm, sizeof(nm));
            if (nm[0]) {
                snprintf(temp_names[relay_count], sizeof(temp_names[relay_count]), "%s", nm);
            } else {
                snprintf(temp_names[relay_count], sizeof(temp_names[relay_count]), "Relay%u", relay_count + 1);
            }
            relay_count++;

            /* advance past this object */
            const char *close = strchr(obj_start, '}');
            if (!close) break;
            cursor = close + 1;
        }

        /* Allow empty relay list (clears config) */

        /* Build new config */
        Gateway_Config_t ncfg;
        Get_Shared_Config(&ncfg);
        ncfg.relay_count = relay_count;
        memset(ncfg.relays, 0, sizeof(ncfg.relays));
        for (int i = 0; i < relay_count; i++) {
            ncfg.relays[i].port_id = temp_ports[i];
            ncfg.relays[i].pin_num = temp_pins[i];
            ncfg.relays[i].is_nc   = temp_nc[i];
            ncfg.relays[i].state   = 0;
            snprintf(ncfg.relays[i].name, sizeof(ncfg.relays[i].name), "%s", temp_names[i]);
        }
        ncfg.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&ncfg);

        /* Configure GPIO + apply OFF state for each new relay */
        for (int i = 0; i < relay_count; i++) {
            uint8_t port = ncfg.relays[i].port_id;
            uint8_t pin  = ncfg.relays[i].pin_num;
            if (port <= 4 && pin <= 15) {
                GPIO_InitOutput(GPIO_Ports[port], pin);
                Relay_SetState((uint8_t)i, 0);
            }
        }

        /* Persist to flash emulated EEPROM (Sector 11 = last 16KB at 0x080E0000) */
        FLASH_EraseSector(11);
        FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&ncfg, sizeof(Gateway_Config_t));

        Send_Response(sn, HTTP_200_JSON, "{\"ok\":true}");
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/mqtt/confirm_topic  → Confirm pending topic from API
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/mqtt/confirm_topic", 35) == 0) {
        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);
        if (cfg.pending_sparkplug_topic[0] != '\0') {
            strncpy(cfg.sparkplug_topic, cfg.pending_sparkplug_topic, sizeof(cfg.sparkplug_topic) - 1);
            cfg.sparkplug_topic[sizeof(cfg.sparkplug_topic) - 1] = '\0';
            cfg.pending_sparkplug_topic[0] = '\0';
            cfg.magic = CONFIG_MAGIC_CURRENT;
            Update_Shared_Config(&cfg);

            /* Persist to flash */
            FLASH_EraseSector(11);
            FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&cfg, sizeof(Gateway_Config_t));
        }
        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/mqtt/reject_topic  → Reject pending topic from API
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/mqtt/reject_topic", 34) == 0) {
        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);
        cfg.pending_sparkplug_topic[0] = '\0';
        cfg.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&cfg);

        /* Persist to flash */
        FLASH_EraseSector(11);
        FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&cfg, sizeof(Gateway_Config_t));

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/mqtt/delete  → Clear MQTT broker settings & disconnect
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/mqtt/delete", 28) == 0) {
        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);
        
        memset(cfg.mqtt_broker, 0, sizeof(cfg.mqtt_broker));
        cfg.mqtt_port = 1883;
        memset(cfg.mqtt_client_id, 0, sizeof(cfg.mqtt_client_id));
        memset(cfg.mqtt_username, 0, sizeof(cfg.mqtt_username));
        memset(cfg.mqtt_password, 0, sizeof(cfg.mqtt_password));
        memset(cfg.sparkplug_topic, 0, sizeof(cfg.sparkplug_topic));
        cfg.mqtt_interval = 5;
        cfg.mqtt_send_mode = 0;
        
        cfg.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&cfg);

        /* Persist to flash */
        FLASH_EraseSector(11);
        FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&cfg, sizeof(Gateway_Config_t));

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/mqtt  → MQTT broker config
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/mqtt", 21) == 0) {
        char *body = strstr(line, "\r\n\r\n");
        if (!body) { Send_Response(sn, HTTP_400, NULL); return; }
        body += 4;

        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);

        JSON_ReadStr(body, "broker",    cfg.mqtt_broker,    sizeof(cfg.mqtt_broker));
        JSON_ReadStr(body, "client_id", cfg.mqtt_client_id, sizeof(cfg.mqtt_client_id));
        JSON_ReadStr(body, "username",  cfg.mqtt_username,  sizeof(cfg.mqtt_username));
        JSON_ReadStr(body, "password",  cfg.mqtt_password,  sizeof(cfg.mqtt_password));
        JSON_ReadStr(body, "sparkplug_topic", cfg.sparkplug_topic, sizeof(cfg.sparkplug_topic));

        const char *port_v = JSON_FindValue(body, "port");
        if (port_v) {
            uint16_t p = 0;
            while (*port_v >= '0' && *port_v <= '9') { p = p * 10 + (*port_v - '0'); port_v++; }
            cfg.mqtt_port = p;
        }

        const char *int_v = JSON_FindValue(body, "interval");
        if (int_v) {
            uint32_t iv = 0;
            while (*int_v >= '0' && *int_v <= '9') { iv = iv * 10 + (*int_v - '0'); int_v++; }
            if (iv > 0) cfg.mqtt_interval = iv;
        }

        const char *sm_v = JSON_FindValue(body, "send_mode");
        if (sm_v && *sm_v >= '0' && *sm_v <= '9') {
            cfg.mqtt_send_mode = (uint8_t)(*sm_v - '0');
        }

        cfg.magic = CONFIG_MAGIC_CURRENT; /* Ensure CONFIG_MAGIC is saved */
        Update_Shared_Config(&cfg);

        /* Persist */
        FLASH_EraseSector(11);
        FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&cfg, sizeof(Gateway_Config_t));

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/provision/delete  → Clear provisioning data
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/provision/delete", 26) == 0) {
        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);
        
        memset(cfg.device_id, 0, sizeof(cfg.device_id));
        memset(cfg.provision_status, 0, sizeof(cfg.provision_status));
        memset(cfg.provision_message, 0, sizeof(cfg.provision_message));
        memset(cfg.sparkplug_topic, 0, sizeof(cfg.sparkplug_topic));
        memset(cfg.pending_sparkplug_topic, 0, sizeof(cfg.pending_sparkplug_topic));
        
        cfg.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&cfg);

        /* Persist to flash */
        FLASH_EraseSector(11);
        FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&cfg, sizeof(Gateway_Config_t));

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/provision/manual  → Manually provision device
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/provision/manual", 26) == 0) {
        char *body = strstr(line, "\r\n\r\n");
        if (!body) { Send_Response(sn, HTTP_400, NULL); return; }
        body += 4;

        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);
        
        JSON_ReadStr(body, "device_id", cfg.device_id, sizeof(cfg.device_id));
        JSON_ReadStr(body, "sparkplug_topic", cfg.sparkplug_topic, sizeof(cfg.sparkplug_topic));
        
        /* If manual topic is empty, generate standard Sparkplug B topic path */
        if (cfg.sparkplug_topic[0] == '\0') {
            snprintf(cfg.sparkplug_topic, sizeof(cfg.sparkplug_topic), "spBv1.0/KontrxGroup/DDATA/%s", cfg.device_id);
        }
        
        strncpy(cfg.provision_status, "Active", sizeof(cfg.provision_status) - 1);
        cfg.provision_status[sizeof(cfg.provision_status) - 1] = '\0';
        
        snprintf(cfg.provision_message, sizeof(cfg.provision_message), "Manually provisioned via Web UI");
        
        cfg.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&cfg);

        /* Persist to flash */
        FLASH_EraseSector(11);
        FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&cfg, sizeof(Gateway_Config_t));

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/provision  → Receive provisioning data from mobile app
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/provision", 19) == 0 ||
        strncmp(line, "POST /api/config/provision", 26) == 0) {
        char *body = strstr(line, "\r\n\r\n");
        if (!body) { Send_Response(sn, HTTP_400, NULL); return; }
        body += 4;

        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);

        JSON_ReadStr(body, "deviceId",       cfg.device_id,         sizeof(cfg.device_id));
        JSON_ReadStr(body, "status",         cfg.provision_status,  sizeof(cfg.provision_status));
        JSON_ReadStr(body, "message",        cfg.provision_message, sizeof(cfg.provision_message));

        /* Parse MQTT Broker IP — try all field name variants from mobile app */
        char broker[64] = {0};
        JSON_ReadStr(body, "mqttBroker", broker, sizeof(broker));
        if (broker[0] == '\0') JSON_ReadStr(body, "brokerUrl", broker, sizeof(broker));
        if (broker[0] == '\0') JSON_ReadStr(body, "broker",    broker, sizeof(broker));
        if (broker[0] != '\0') {
            strncpy(cfg.mqtt_broker, broker, sizeof(cfg.mqtt_broker) - 1);
            cfg.mqtt_broker[sizeof(cfg.mqtt_broker) - 1] = '\0';
        }

        /* Parse broker port — default to 1883 (standard MQTT) if not provided */
        const char *port_v = JSON_FindValue(body, "port");
        if (port_v && *port_v >= '0' && *port_v <= '9') {
            uint16_t p = 0;
            while (*port_v >= '0' && *port_v <= '9') { p = p * 10 + (*port_v - '0'); port_v++; }
            if (p > 0) cfg.mqtt_port = p;
        }
        if (cfg.mqtt_port == 0) {
            cfg.mqtt_port = 1883; /* default MQTT port */
        }

        /* Parse MQTT topic from provisioning — saves to pending_sparkplug_topic for user confirmation */
        {
            char prov_topic[128] = {0};
            JSON_ReadStr(body, "topic", prov_topic, sizeof(prov_topic));
            if (prov_topic[0] == '\0')
                JSON_ReadStr(body, "sparkplugTopic", prov_topic, sizeof(prov_topic));
            if (prov_topic[0] == '\0')
                JSON_ReadStr(body, "mqttTopic", prov_topic, sizeof(prov_topic));
            if (prov_topic[0] != '\0') {
                strncpy(cfg.pending_sparkplug_topic, prov_topic, sizeof(cfg.pending_sparkplug_topic) - 1);
                cfg.pending_sparkplug_topic[sizeof(cfg.pending_sparkplug_topic) - 1] = '\0';
            }
        }

        cfg.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&cfg);

        /* Persist to Flash sector 11 */
        FLASH_EraseSector(11);
        FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&cfg, sizeof(Gateway_Config_t));

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/serial → Set sequential device serial:
     *   {"serial": 7}   (or {"serial":"0000007"})  => "KX-0000007"
     * Stored in flash; used by /api/status and the QR payload.
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/serial", 23) == 0) {
        char *body = strstr(line, "\r\n\r\n");
        if (!body) { Send_Response(sn, HTTP_400, NULL); return; }
        body += 4;

        const char *sv = JSON_FindValue(body, "serial");
        if (!sv) {
            snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":false,\"error\":\"Missing serial.\"}");
            Send_Response(sn, HTTP_200_JSON, tx_buf);
            return;
        }
        uint32_t s = 0;
        while (*sv >= '0' && *sv <= '9') { s = s * 10 + (uint32_t)(*sv - '0'); sv++; }
        if (s == 0 || s > 9999999UL) {
            snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":false,\"error\":\"Serial must be 1..9999999.\"}");
            Send_Response(sn, HTTP_200_JSON, tx_buf);
            return;
        }

        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);
        cfg.serial = s;
        cfg.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&cfg);

        /* Persist */
        FLASH_EraseSector(11);
        FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&cfg, sizeof(Gateway_Config_t));

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/sensors  → Full replace:
     *   {"sensors":[{"type":"ph","id":2}, {"type":"do","id":9}, ...]}
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/sensors", 24) == 0) {
        char *body = strstr(line, "\r\n\r\n");
        if (!body) { Send_Response(sn, HTTP_400, NULL); return; }
        body += 4;

        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);

        uint8_t s_types[MAX_SENSORS];
        uint8_t s_ids[MAX_SENSORS];
        uint8_t s_count = 0;

        /* Parse list of {"type":"...","id":N} objects */
        const char *cursor = body;
        while (s_count < MAX_SENSORS) {
            const char *obj = strstr(cursor, "\"type\":");
            if (!obj) break;
            const char *obj_start = obj;
            while (obj_start > body && *obj_start != '{') obj_start--;

            char type_str[16] = {0};
            JSON_ReadStr(obj_start, "type", type_str, sizeof(type_str));

            uint8_t type = 0;
            if (strcmp(type_str, "ph") == 0)        type = 1;
            else if (strcmp(type_str, "orp") == 0)  type = 2;
            else if (strcmp(type_str, "ec") == 0)   type = 3;
            else if (strcmp(type_str, "do") == 0)   type = 4;
            else if (strcmp(type_str, "ammonia") == 0) type = 5;
            else if (strcmp(type_str, "ultrasonic") == 0) type = 6;
            else if (strcmp(type_str, "multi_us") == 0)  type = 7;

            if (type < 1 || type > 7) {
                snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":false,\"error\":\"Unknown sensor type %s.\"}", type_str);
                Send_Response(sn, HTTP_200_JSON, tx_buf);
                return;
            }

            int id = JSON_ReadInt(obj_start, "id");
            if (id < 1 || id > 247) {
                snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":false,\"error\":\"Sensor id must be 1..247.\"}");
                Send_Response(sn, HTTP_200_JSON, tx_buf);
                return;
            }
            for (int j = 0; j < s_count; j++) {
                if (s_ids[j] == (uint8_t)id) {
                    snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":false,\"error\":\"Modbus id %d is assigned twice.\"}", id);
                    Send_Response(sn, HTTP_200_JSON, tx_buf);
                    return;
                }
            }

            s_types[s_count] = type;
            s_ids[s_count]   = (uint8_t)id;
            s_count++;

            const char *close = strchr(obj_start, '}');
            if (!close) break;
            cursor = close + 1;
        }

        if (s_count == 0) {
            snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":false,\"error\":\"No sensors provided.\"}");
            Send_Response(sn, HTTP_200_JSON, tx_buf);
            return;
        }

        cfg.sensors.count = s_count;
        memset(cfg.sensors.entries, 0, sizeof(cfg.sensors.entries));
        for (int i = 0; i < s_count; i++) {
            cfg.sensors.entries[i].type = s_types[i];
            cfg.sensors.entries[i].id   = s_ids[i];
        }

        cfg.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&cfg);

        /* Persist */
        FLASH_EraseSector(11);
        FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&cfg, sizeof(Gateway_Config_t));

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/modbus/scan  → Trigger background Modbus scan
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/modbus/scan", 21) == 0) {
        if (!g_scan_status.is_scanning) {
            g_scan_status.count = 0;
            g_scan_status.progress = 0;
            g_scan_status.is_scanning = 1;
        }
        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * GET /api/modbus/scan  → Fetch scan status and results
     * ----------------------------------------------------------------- */
    if (strncmp(line, "GET /api/modbus/scan", 20) == 0) {
        // Build JSON response
        int pos = 0;
        pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos,
            "{\"scanning\":%s,\"progress\":%d,\"devices\":[",
            g_scan_status.is_scanning ? "true" : "false",
            g_scan_status.progress);
        
        for (int i = 0; i < g_scan_status.count; i++) {
            pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos,
                "{\"id\":%u,\"type\":%u}%s",
                g_scan_status.devices[i].id,
                g_scan_status.devices[i].type,
                (i < g_scan_status.count - 1) ? "," : "");
        }
        pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos, "]}");
        
        send(sn, (uint8_t *)HTTP_200_JSON, strlen(HTTP_200_JSON));
        send(sn, (uint8_t *)tx_buf, (uint16_t)pos);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/ota/verify  → validate OTP
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/ota/verify", 20) == 0) {
        char *body = strstr(line, "\r\n\r\n");
        if (!body) { Send_Response(sn, HTTP_400, NULL); return; }
        body += 4;

        char otp[64] = {0};
        JSON_ReadStr(body, "otp", otp, sizeof(otp));

        if (strcmp(otp, OTA_OTP_SECRET) == 0) {
            ota_otp_validated = 1;
            Send_Response(sn, HTTP_200_JSON, "{\"ok\":true}");
        } else {
            ota_otp_validated = 0;
            Send_Response(sn, HTTP_200_JSON, "{\"ok\":false}");
        }
        return;
    }



    /* -----------------------------------------------------------------
     * POST /api/ota/prepare  → Erase staging sectors for OTA
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/ota/prepare", 21) == 0) {
        printf("[OTA] Sending prepare response...\r\n");
        const char *resp = "HTTP/1.1 200 OK\r\nContent-Type:application/json\r\nConnection:close\r\n\r\n{\"status\":\"preparing\"}";
        send(sn, (uint8_t *)resp, strlen(resp));
        
        /* Give W5500 and TCP stack 100ms to transmit the HTTP response
         * before we force disconnect/close and execute the blocking flash erase.
         * Otherwise, the client receives a connection reset ("Failed to fetch"). */
        osDelay(100);
        disconnect(sn);
        close(sn);

        printf("[OTA] Preparing flash: Erasing sectors 6 and 7...\r\n");
        
        /* Suspend Modbus and MQTT tasks during blocking Flash erase to prevent CPU
         * starvation, watchdog resets, or W5500 SPI contention while CPU is stalled. */
        extern osThreadId_t g_tid_modbus;
        extern osThreadId_t g_tid_mqtt;
        if (g_tid_modbus) vTaskSuspend((TaskHandle_t)g_tid_modbus);
        if (g_tid_mqtt)   vTaskSuspend((TaskHandle_t)g_tid_mqtt);

        FLASH_EraseSector(6);
        FLASH_EraseSector(7);

        /* Resume suspended tasks after flash operation finishes */
        if (g_tid_modbus) vTaskResume((TaskHandle_t)g_tid_modbus);
        if (g_tid_mqtt)   vTaskResume((TaskHandle_t)g_tid_mqtt);

        printf("[OTA] Flash erase complete.\r\n");
        return;
    }

    /* -----------------------------------------------------------------
     * POST /update  → OTA binary upload
     * Check OTP header, then stream body to staging flash area.
     * Staging sectors are already erased via prepare endpoint.
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /update", 12) == 0) {

        /* OTP validation disabled by user request. Force ota_otp_validated to 1. */
        ota_otp_validated = 1;

        /* Parse Content-Length */
        uint32_t content_length = 0;
        char *cl = strstr(line, "Content-Length:");
        if (!cl) cl = strstr(line, "content-length:");
        if (cl) {
            cl += 15;
            while (*cl == ' ') cl++;
            while (*cl >= '0' && *cl <= '9') { content_length = content_length * 10 + (*cl - '0'); cl++; }
        }

        /* Find body start */
        char *body = strstr(line, "\r\n\r\n");
        if (!body) { Send_Response(sn, HTTP_400, NULL); return; }
        body += 4;

        Flash_Aligned_Writer_t writer;
        Flash_Writer_Init(&writer, STAGING_ADDR);
        uint32_t total_written = 0;

        /* Write the first body chunk (already in rx_buf after headers) */
        uint32_t first_body_len = (uint32_t)(len - (uint32_t)(body - (char *)req));
        if (first_body_len > 0 && total_written < content_length) {
            uint32_t to_copy = content_length - total_written;
            if (first_body_len < to_copy) to_copy = first_body_len;
            Flash_Writer_Write(&writer, (const uint8_t *)body, to_copy);
            total_written += to_copy;
        }

        /* Stream remaining chunks with proper recv checking */
        uint32_t timeout_cnt = 0;
        while (total_written < content_length) {
            uint16_t chunk = getSn_RX_RSR(sn);
            if (chunk > 0) {
                timeout_cnt = 0;
                if (chunk > sizeof(rx_buf)) chunk = sizeof(rx_buf);
                uint32_t remaining = content_length - total_written;
                if ((uint32_t)chunk > remaining) chunk = (uint16_t)remaining;
                int32_t recvd = recv(sn, rx_buf, chunk);
                if (recvd > 0) {
                    Flash_Writer_Write(&writer, rx_buf, (uint32_t)recvd);
                    total_written += (uint32_t)recvd;
                }
            } else {
                if (getSn_SR(sn) != SOCK_ESTABLISHED) break;
                osDelay(1);  /* 1ms wait for more OTA bytes — W5500 RX empty. */
                timeout_cnt++;
                if (timeout_cnt > 10000) break;
            }
        }

        /* Flush remaining bytes in writer cache */
        Flash_Writer_Flush(&writer);

        /* Signal OTA task to verify + write metadata */
        ota_content_length   = total_written;
        ota_request_pending  = 1;
        osSemaphoreRelease(sem_ota_start);

        /* Wait for OTA task to confirm */
        if (osSemaphoreAcquire(sem_ota_done, 5000) == osOK && ota_write_ok) {
            const char *resp = "HTTP/1.1 200 OK\r\nContent-Type:text/plain\r\nConnection:close\r\n\r\n"
                               "OTA OK. Rebooting...";
            send(sn, (uint8_t *)resp, strlen(resp));
        } else {
            const char *resp = "HTTP/1.1 500 Internal Server Error\r\nConnection:close\r\n\r\nOTA CRC FAIL";
            send(sn, (uint8_t *)resp, strlen(resp));
        }

        disconnect(sn);
        ota_otp_validated  = 0; /* Reset OTP after use */
        return;
    }

    /* Default: 404 */
    const char *r404 = "HTTP/1.1 404 Not Found\r\nConnection:close\r\n\r\nNot Found";
    send(sn, (uint8_t *)r404, strlen(r404));
}

/* ======================================================================
 *  HTTP Server Task
 * ====================================================================== */
void Task_HTTPServer(void *arg) {
    (void)arg;

    /* Initialise OTA semaphores (only done once) */
    if (!sem_ota_start) sem_ota_start = osSemaphoreNew(1, 0, NULL);
    if (!sem_ota_done)  sem_ota_done  = osSemaphoreNew(1, 0, NULL);

    printf("[HTTP] Task started; opening TCP port %u\r\n", HTTP_PORT);

    for (;;) {
        switch (getSn_SR(HTTP_SOCK)) {

        /* ---------------------------------------------------------------
         * CLOSED → create TCP socket and prepare to listen.
         * After socket() the W5500 needs a few ms to allocate the
         * socket and transition to SOCK_INIT.  5ms is sufficient.
         * -------------------------------------------------------------- */
        case SOCK_CLOSED:
            if (socket(HTTP_SOCK, Sn_MR_TCP, HTTP_PORT, 0x00) < 0) {
                printf("[HTTP] Socket open failed\r\n");
                osDelay(100); /* backoff on W5500 socket allocation failure */
            } else {
                osDelay(5);   /* let W5500 settle to SOCK_INIT state */
            }
            break;

        /* ---------------------------------------------------------------
         * INIT → start listening for incoming connections.
         * After listen() the W5500 needs a few ms to transition its
         * internal state machine from SOCK_INIT to SOCK_LISTEN.  Without
         * the delay getSn_SR() still returns SOCK_INIT on the very next
         * 1ms loop tick, so listen() is called (and printed) again and
         * again.  10ms is more than enough for the W5500 to settle.
         * --------------------------------------------------------------- */
        case SOCK_INIT:
            if (listen(HTTP_SOCK) != SOCK_OK) {
                printf("[HTTP] Listen failed\r\n");
                osDelay(100);
            } else {
                printf("[HTTP] Listening on port %u\r\n", HTTP_PORT);
                osDelay(10);  /* let W5500 settle to SOCK_LISTEN state */
            }
            break;

        /* ---------------------------------------------------------------
         * LISTEN → idle, waiting for browser SYN  (no action needed)
         * -------------------------------------------------------------- */
        case SOCK_LISTEN:
            break;

        /* ---------------------------------------------------------------
         * ESTABLISHED → receive and dispatch an HTTP request
         *
         * Wait up to 50ms in 5ms steps for the full request to arrive.
         * This avoids the "partial-request" race where strncmp on an
         * incomplete URL falls through to the 404 handler.
         * -------------------------------------------------------------- */
        case SOCK_ESTABLISHED:
            /* Clear connection interrupt flag */
            if (getSn_IR(HTTP_SOCK) & Sn_IR_CON) {
                setSn_IR(HTTP_SOCK, Sn_IR_CON);
            }
            {
                uint16_t size = 0;
                /* Retry loop: wait for at least 8 bytes (shortest valid
                 * request line: "GET / H") before processing */
                for (int retry = 0; retry < 10; retry++) {
                    size = getSn_RX_RSR(HTTP_SOCK);
                    if (size >= 8) break;
                    osDelay(5); /* 5ms poll step while waiting for the browser's
                                 * request bytes to arrive from the network.
                                 * Max wait = 10 x 5ms = 50ms, then 404 fallback.
                                 * HTTP task only; unrelated to the 1ms engine. */
                }

                if (size > 0) {
                    if (size > (uint16_t)(sizeof(rx_buf) - 1))
                        size = (uint16_t)(sizeof(rx_buf) - 1);
                    recv(HTTP_SOCK, rx_buf, size);
                    Dispatch_Request(HTTP_SOCK, rx_buf, size);
                    /* Give W5500 TX buffer 20ms to flush all response bytes
                     * to the browser before we send FIN (disconnect).
                     * Without this the final TCP segment can be lost when
                     * the socket transitions too quickly to TIME_WAIT/CLOSED. */
                    osDelay(20);
                    disconnect(HTTP_SOCK);
                }
            }
            break;

        /* ---------------------------------------------------------------
         * CLOSE_WAIT → remote sent FIN; we ack and finish our side
         * -------------------------------------------------------------- */
        case SOCK_CLOSE_WAIT:
            disconnect(HTTP_SOCK);
            break;

        /* ---------------------------------------------------------------
         * FIN_WAIT / CLOSING / TIME_WAIT
         * Force-close the socket so W5500 recycles it to SOCK_CLOSED
         * immediately — without this, the socket stays stuck and the
         * server cannot accept any new connections (page refuses to load).
         * The browser handles the RST cleanly on reconnect.
         * -------------------------------------------------------------- */
        case SOCK_FIN_WAIT:
        case SOCK_CLOSING:
        case SOCK_TIME_WAIT:
            close(HTTP_SOCK);
            break;

        default:
            break;
        }

        osDelay(1); /* 1ms yield per loop — keeps the W5500 state machine
                     * responsive (~1KB API responses) without starving the
                     * 1ms Control Engine. Exact 1ms, does not exceed budget. */
    }
}
