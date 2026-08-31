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
#include "cJSON.h"
#include "web_assets.h"
#include "modbus_dma.h"
#include "w25q16.h"
#include "flash_partition.h"
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

extern volatile uint32_t g_uptime_seconds;
extern uint8_t g_log_ring[LOG_BUFFER_SIZE];
extern volatile uint32_t g_log_head;
extern volatile uint32_t g_log_tail;
extern volatile uint32_t g_total_logs_written;
extern volatile uint32_t g_sys_log_count;
extern volatile uint8_t  g_sys_log_full;

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
static Gateway_Config_t s_http_cfg_temp;  /* Shared config scratch for all HTTP handlers */

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

static void Send_JSON_Logs(uint8_t sn) {
    // Send HTTP Header first
    const char *hdr = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "{\"logs\":[";
    send(sn, (uint8_t *)hdr, strlen(hdr));

    taskENTER_CRITICAL();
    uint32_t count = g_sys_log_count;
    uint32_t temp_tail = g_log_tail;
    taskEXIT_CRITICAL();

    uint32_t processed = 0;
    uint32_t printed = 0;
    char line_buf[128];
    uint32_t safety_counter = 0;

    while (processed < count && safety_counter++ < 1000) {
        taskENTER_CRITICAL();
        // Check if we need to wrap at the end of the buffer
        if (temp_tail + sizeof(LogHeader_t) > LOG_BUFFER_SIZE) {
            temp_tail = 0;
            taskEXIT_CRITICAL();
            continue;
        }

        LogHeader_t *l_hdr = (LogHeader_t *)&g_log_ring[temp_tail];
        if (l_hdr->category_id == 0xFF) {
            temp_tail = 0;
            taskEXIT_CRITICAL();
            continue;
        }

        uint32_t s = l_hdr->timestamp;
        uint8_t cat_id = l_hdr->category_id;
        uint8_t msg_len = l_hdr->msg_len;

        // Copy message string safely while in critical section
        char msg_temp[64];
        uint32_t copy_len = msg_len;
        if (copy_len >= sizeof(msg_temp)) copy_len = sizeof(msg_temp) - 1;
        memcpy(msg_temp, &g_log_ring[temp_tail + sizeof(LogHeader_t)], copy_len);
        msg_temp[copy_len] = '\0';

        // Advance tail for the next loop
        temp_tail += sizeof(LogHeader_t) + msg_len;
        taskEXIT_CRITICAL();

        uint32_t hrs = s / 3600;
        uint32_t mins = (s % 3600) / 60;
        uint32_t secs = s % 60;

        const char *cat_str = "SYS";
        if (cat_id == 1) cat_str = "MQTT";
        else if (cat_id == 2) cat_str = "MODBUS";
        else if (cat_id == 3) cat_str = "RELAY";
        else if (cat_id == 4) cat_str = "OTA";

        int n = snprintf(line_buf, sizeof(line_buf),
                         "%s{\"time\":\"%02lu:%02lu:%02lu\",\"cat\":\"%s\",\"msg\":\"%s\"}",
                         (printed > 0) ? "," : "",
                         (unsigned long)hrs, (unsigned long)mins, (unsigned long)secs,
                         cat_str, msg_temp);

        send(sn, (uint8_t *)line_buf, n);
        printed++;
        processed++;
        
        // 1ms yield to prevent task starvation of the 1ms control loop during long transmissions
        osDelay(1);
    }

    const char *footer = "]}";
    send(sn, (uint8_t *)footer, strlen(footer));
}

static int JSON_HardwareResponse(char *buf, int buflen) {
    Get_Shared_Config(&s_cfg);
    int pos = 0;
    pos += snprintf(buf + pos, buflen - pos, "{\"sensors\":[");
    for (uint8_t i = 0; i < s_cfg.sensors.count && i < MAX_SENSORS; i++) {
        uint8_t type = s_cfg.sensors.entries[i].type;
        uint8_t id   = s_cfg.sensors.entries[i].id;
        pos += snprintf(buf + pos, buflen - pos,
            "{\"id\":%u,\"type\":%u,\"type_name\":\"%s\"}%s",
            id, type, SensorTypeName(type),
            (i < s_cfg.sensors.count - 1) ? "," : "");
    }
    pos += snprintf(buf + pos, buflen - pos, "],\"relays\":[");
    for (uint8_t i = 0; i < s_cfg.actuator_count && i < MAX_RELAYS; i++) {
        char pin_val[32] = {0};
        if (s_cfg.actuators[i].type == ACTUATOR_TYPE_LOCAL_GPIO) {
            snprintf(pin_val, sizeof(pin_val), "%s%u", s_cfg.actuators[i].port_or_ip, s_cfg.actuators[i].pin_or_slave);
        } else {
            snprintf(pin_val, sizeof(pin_val), "%s", s_cfg.actuators[i].port_or_ip);
        }
        const char *act_type_name = "unknown";
        if (s_cfg.actuators[i].type == ACTUATOR_TYPE_LOCAL_GPIO) act_type_name = "GPIO";
        else if (s_cfg.actuators[i].type == ACTUATOR_TYPE_MODBUS_TCP) act_type_name = "Modbus TCP";
        else if (s_cfg.actuators[i].type == ACTUATOR_TYPE_OPC_UA_CLIENT) act_type_name = "OPC UA";

        pos += snprintf(buf + pos, buflen - pos,
            "{\"id\":%u,\"name\":\"%s\",\"type\":%u,\"type_name\":\"%s\",\"state\":%u,\"pin\":\"%s\",\"nc\":%u}%s",
            i,
            s_cfg.actuators[i].name[0] ? s_cfg.actuators[i].name : "Actuator",
            s_cfg.actuators[i].type,
            act_type_name,
            relayStates[i],
            pin_val,
            s_cfg.actuators[i].is_active_low,
            (i < s_cfg.actuator_count - 1) ? "," : "");
    }
    pos += snprintf(buf + pos, buflen - pos, "]}");
    return pos;
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
    /* --- actuators (dynamic) --- */
    pos += snprintf(buf + pos, buflen - pos, "\"relays\":[");
    for (uint8_t i = 0; i < s_cfg.actuator_count && i < MAX_RELAYS; i++) {
        char pin_val[32] = {0};
        if (s_cfg.actuators[i].type == ACTUATOR_TYPE_LOCAL_GPIO) {
            snprintf(pin_val, sizeof(pin_val), "%s%u", s_cfg.actuators[i].port_or_ip, s_cfg.actuators[i].pin_or_slave);
        } else {
            snprintf(pin_val, sizeof(pin_val), "%s", s_cfg.actuators[i].port_or_ip);
        }
        
        pos += snprintf(buf + pos, buflen - pos,
            "{\"id\":%u,\"state\":%u,\"pin\":\"%s\",\"nc\":%u,\"name\":\"%s\","
            "\"type\":%u,\"port\":%u,\"slave_id\":%u,\"reg_addr\":%u,\"opc_node_id\":\"%s\"}%s",
            i,
            relayStates[i],
            pin_val,
            s_cfg.actuators[i].is_active_low,
            s_cfg.actuators[i].name[0] ? s_cfg.actuators[i].name : "Actuator",
            s_cfg.actuators[i].type,
            s_cfg.actuators[i].port,
            s_cfg.actuators[i].pin_or_slave,
            s_cfg.actuators[i].reg_addr,
            s_cfg.actuators[i].opc_node_id,
            (i < s_cfg.actuator_count - 1) ? "," : "");
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
    uint8_t has_pending = 0;
    char pending_ver[36] = {0};
    if (rulesMutex && osMutexAcquire(rulesMutex, 0) == osOK) {
        has_pending = hasPendingRules;
        strncpy(pending_ver, pendingRules.version_id, sizeof(pending_ver) - 1);
        osMutexRelease(rulesMutex);
    }
    pos += snprintf(buf + pos, buflen - pos,
        ",\"sys\":{\"cpu_pct\":%u,\"heap_free\":%u,\"heap_total\":%u,\"log_full\":%u,\"has_pending\":%u,\"pending_version\":\"%s\"}}",
        /* ↑ note trailing }} : closes sys object AND root object         */
        (unsigned)g_cpu_usage_pct,
        (unsigned)heap_free,
        (unsigned)heap_total,
        (unsigned)g_sys_log_full,
        (unsigned)has_pending,
        pending_ver);

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
            osDelay(1);   /* 1ms yield between successful chunk writes to prevent SPI lock starvation */
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
 *  Safe Config Flash Writer
 *  Suspends background tasks to prevent SPI conflicts, MCU freezes, or
 *  race conditions while erasing and writing Sector 7 emulated EEPROM.
 * ====================================================================== */
static void Safe_Write_Config_To_Flash(const Gateway_Config_t *cfg_in, const char *action_desc) {
    Partition_SaveConfig(cfg_in);

    char log_msg[64];
    snprintf(log_msg, sizeof(log_msg), "Saved %s configuration to external flash.", action_desc);
    Log_Event("SYS", log_msg);
}

static void Stream_Web_Asset(uint8_t sn) {
    uint32_t html_len = strlen(KONTRX_HTML);
    printf("[HTTP] Streaming web asset: %lu bytes\r\n", (unsigned long)html_len);
    char hdr[200];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: %lu\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Connection: close\r\n"
        "\r\n",
        (unsigned long)html_len);
    send(sn, (uint8_t *)hdr, (uint16_t)strlen(hdr));

    // Stream from W25Q16 in 512-byte chunks with send() retry
    uint8_t chunk_buf[512];
    uint32_t addr = PARTITION_WEB_ADDR;
    uint32_t remaining = html_len;
    uint32_t last_progress_print = 0;
    while (remaining > 0) {
        uint32_t read_len = (remaining > 512) ? 512 : remaining;
        W25Q_Read(addr, chunk_buf, read_len);
        int32_t result = send(sn, chunk_buf, (uint16_t)read_len);
        if (result > 0) {
            addr += read_len;
            remaining -= read_len;
            if (html_len - remaining - last_progress_print >= 10240 || remaining == 0) {
                printf("[HTTP] Streaming: %lu/%lu bytes sent\r\n", 
                       (unsigned long)(html_len - remaining), (unsigned long)html_len);
                last_progress_print = html_len - remaining;
            }
        } else if (result == SOCK_BUSY) {
            osDelay(1);  // TX buffer full, retry next tick
        } else {
            printf("[HTTP] Streaming socket error, aborting stream! (result=%ld)\r\n", (long)result);
            break;  // Socket error, abort
        }
        osDelay(1); // Yield to other tasks
    }
}

/* ======================================================================
 *  Request dispatcher
 * ====================================================================== */
static void Dispatch_Request(uint8_t sn, uint8_t *req, uint16_t len) {
    req[len] = '\0';
    char *line = (char *)req;
    
    char first_line[64] = {0};
    char *newline = strchr(line, '\r');
    if (!newline) newline = strchr(line, '\n');
    if (newline) {
        int first_len = newline - line;
        if (first_len > 63) first_len = 63;
        strncpy(first_line, line, first_len);
    } else {
        strncpy(first_line, line, sizeof(first_line) - 1);
    }
    printf("[HTTP] Dispatch_Request: %s\r\n", first_line);

    /* OPTIONS check for CORS pre-flight requests */
    if (strncmp(line, "OPTIONS ", 8) == 0) {
        char cors_hdr[] =
            "HTTP/1.1 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n";
        send(sn, (uint8_t *)cors_hdr, (uint16_t)strlen(cors_hdr));
        return;
    }

    /* -----------------------------------------------------------------
     * GET /
     * Serve the full SPA HTML page from W25Q16 with Cache-Control headers
     * ----------------------------------------------------------------- */
    if (strncmp(line, "GET / ", 6) == 0 || strncmp(line, "GET /index", 10) == 0) {
        Stream_Web_Asset(sn);
        return;
    }

    /* -----------------------------------------------------------------
     * GET /api/hardware  → JSON list of available peripherals (CORS)
     * ----------------------------------------------------------------- */
    if (strncmp(line, "GET /api/hardware", 17) == 0) {
        int n = JSON_HardwareResponse(tx_buf, sizeof(tx_buf));
        char hw_hdr[240];
        snprintf(hw_hdr, sizeof(hw_hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n",
            n);
        send(sn, (uint8_t *)hw_hdr, (uint16_t)strlen(hw_hdr));
        Send_Chunked(sn, (const uint8_t *)tx_buf, (uint32_t)n);
        return;
    }

    /* -----------------------------------------------------------------
     * GET /api/rules/history  → List historical rule versions (CORS)
     * ----------------------------------------------------------------- */
    if (strncmp(line, "GET /api/rules/history", 22) == 0) {
        RuleConfig_t temp_rules;
        
        int pos = 0;
        pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos, "[");
        uint8_t added = 0;
        char prim_version[36] = {0};

        // Read Primary
        W25Q_Read(RULES_PRIMARY_ADDR, (uint8_t *)&temp_rules, sizeof(RuleConfig_t));
        uint32_t prim_crc = Compute_CRC32((const uint8_t *)&temp_rules, offsetof(RuleConfig_t, checksum));
        if (temp_rules.magic == RULES_MAGIC_CURRENT && temp_rules.checksum == prim_crc) {
            pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos,
                "{\"version_id\":\"%s\",\"timestamp\":\"%s\",\"rules_count\":%u}",
                temp_rules.version_id, temp_rules.timestamp, (unsigned)temp_rules.rule_count);
            strncpy(prim_version, temp_rules.version_id, sizeof(prim_version) - 1);
            added = 1;
        }

        // Read Backup
        W25Q_Read(RULES_BACKUP_ADDR, (uint8_t *)&temp_rules, sizeof(RuleConfig_t));
        uint32_t back_crc = Compute_CRC32((const uint8_t *)&temp_rules, offsetof(RuleConfig_t, checksum));
        if (temp_rules.magic == RULES_MAGIC_CURRENT && temp_rules.checksum == back_crc) {
            if (!added || strcmp(prim_version, temp_rules.version_id) != 0) {
                pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos,
                    "%s{\"version_id\":\"%s\",\"timestamp\":\"%s\",\"rules_count\":%u}",
                    added ? "," : "",
                    temp_rules.version_id, temp_rules.timestamp, (unsigned)temp_rules.rule_count);
            }
        }
        pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos, "]");

        char hist_hdr[320];
        snprintf(hist_hdr, sizeof(hist_hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n",
            pos);
        send(sn, (uint8_t *)hist_hdr, (uint16_t)strlen(hist_hdr));
        Send_Chunked(sn, (const uint8_t *)tx_buf, (uint32_t)pos);
        return;
    }

    /* -----------------------------------------------------------------
     * GET /api/rules  → Get currently active rules from RAM (CORS)
     * ----------------------------------------------------------------- */
    if (strncmp(line, "GET /api/rules ", 14) == 0 || strncmp(line, "GET /api/rules?", 15) == 0) {
        RuleConfig_t current_rules;
        memset(&current_rules, 0, sizeof(RuleConfig_t));
        if (osMutexAcquire(rulesMutex, osWaitForever) == osOK) {
            current_rules = activeRules;
            osMutexRelease(rulesMutex);
        }

        int pos = 0;
        pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos,
            "{\"version_id\":\"%s\",\"timestamp\":\"%s\",\"rules_valid\":%u,\"bypass_validation\":%u,\"rules\":[",
            current_rules.version_id, current_rules.timestamp, current_rules.rules_valid, current_rules.bypass_validation);
        
        for (uint32_t i = 0; i < current_rules.rule_count; i++) {
            pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos,
                "%s{\"rule_id\":\"%s\",\"input_id\":\"%s\",\"operator\":\"%s\",\"threshold\":%.2f,\"output_id\":\"%s\",\"action\":\"%s\",\"active\":%u}",
                (i > 0) ? "," : "",
                current_rules.rules[i].rule_id,
                current_rules.rules[i].input_id,
                current_rules.rules[i].operator,
                current_rules.rules[i].threshold,
                current_rules.rules[i].output_id,
                current_rules.rules[i].action,
                current_rules.rules[i].active);
        }
        pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos, "]}");

        char rules_hdr[320];
        snprintf(rules_hdr, sizeof(rules_hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n",
            pos);
        send(sn, (uint8_t *)rules_hdr, (uint16_t)strlen(rules_hdr));
        Send_Chunked(sn, (const uint8_t *)tx_buf, (uint32_t)pos);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/rules/toggle  → Toggle individual rule active state (CORS)
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/rules/toggle", 22) == 0) {
        char rule_id[32] = {0};
        int active_val = -1;
        
        char *id_ptr = strstr(line, "id=");
        if (id_ptr) {
            id_ptr += 3;
            char *amp = strchr(id_ptr, '&');
            char *space = strchr(id_ptr, ' ');
            char *end = amp ? amp : (space ? space : id_ptr + strlen(id_ptr));
            int len = end - id_ptr;
            if (len > 0 && len < 32) {
                memcpy(rule_id, id_ptr, len);
                rule_id[len] = '\0';
            }
        }
        
        char *act_ptr = strstr(line, "active=");
        if (act_ptr) {
            act_ptr += 7;
            active_val = *act_ptr - '0';
        }
        
        if (rule_id[0] == '\0' || active_val < 0 || active_val > 1) {
            Send_Response(sn, HTTP_200_JSON, "{\"ok\":false,\"error\":\"Invalid rule_id or active value\"}");
            return;
        }
        
        uint8_t found = 0;
        if (osMutexAcquire(rulesMutex, osWaitForever) == osOK) {
            for (uint32_t i = 0; i < activeRules.rule_count; i++) {
                if (strcmp(activeRules.rules[i].rule_id, rule_id) == 0) {
                    activeRules.rules[i].active = (uint8_t)active_val;
                    found = 1;
                    break;
                }
            }
            if (found) {
                Partition_SaveRules(&activeRules);
            }
            osMutexRelease(rulesMutex);
        }
        
        if (found) {
            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "Rule %s active status toggled to %d.", rule_id, active_val);
            Log_Event("SYS", log_msg);
            Send_Response(sn, HTTP_200_JSON, "{\"ok\":true}");
        } else {
            Send_Response(sn, HTTP_200_JSON, "{\"ok\":false,\"error\":\"Rule not found\"}");
        }
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/rules/bypass  → Toggle sensor validation bypass (CORS)
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/rules/bypass", 22) == 0) {
        int enable_val = -1;
        char *enable_ptr = strstr(line, "enable=");
        if (enable_ptr) {
            enable_ptr += 7;
            enable_val = *enable_ptr - '0';
        }
        
        if (enable_val < 0 || enable_val > 1) {
            Send_Response(sn, HTTP_200_JSON, "{\"ok\":false,\"error\":\"Invalid enable value\"}");
            return;
        }
        
        if (osMutexAcquire(rulesMutex, osWaitForever) == osOK) {
            activeRules.bypass_validation = (uint8_t)enable_val;
            Partition_SaveRules(&activeRules);
            osMutexRelease(rulesMutex);
        }
        
        char log_msg[64];
        snprintf(log_msg, sizeof(log_msg), "Rule validation bypass set to %d.", enable_val);
        Log_Event("SYS", log_msg);
        Send_Response(sn, HTTP_200_JSON, "{\"ok\":true}");
        return;
    }

    /* -----------------------------------------------------------------
     * GET /api/rules/pending  → Get pending rules if any (CORS)
     * ----------------------------------------------------------------- */
    if (strncmp(line, "GET /api/rules/pending", 22) == 0) {
        RuleConfig_t pending_snap;
        uint8_t has_pending = 0;
        
        if (osMutexAcquire(rulesMutex, osWaitForever) == osOK) {
            pending_snap = pendingRules;
            has_pending = hasPendingRules;
            osMutexRelease(rulesMutex);
        }

        int pos = 0;
        if (has_pending) {
            pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos,
                "{\"has_pending\":true,\"version_id\":\"%s\",\"timestamp\":\"%s\",\"bypass_validation\":%u,\"rules\":[",
                pending_snap.version_id, pending_snap.timestamp, pending_snap.bypass_validation);
            
            for (uint32_t i = 0; i < pending_snap.rule_count; i++) {
                pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos,
                    "%s{\"rule_id\":\"%s\",\"input_id\":\"%s\",\"operator\":\"%s\",\"threshold\":%.2f,\"output_id\":\"%s\",\"action\":\"%s\",\"active\":%u}",
                    (i > 0) ? "," : "",
                    pending_snap.rules[i].rule_id,
                    pending_snap.rules[i].input_id,
                    pending_snap.rules[i].operator,
                    pending_snap.rules[i].threshold,
                    pending_snap.rules[i].output_id,
                    pending_snap.rules[i].action,
                    pending_snap.rules[i].active);
            }
            pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos, "]}");
        } else {
            pos += snprintf(tx_buf + pos, sizeof(tx_buf) - pos, "{\"has_pending\":false}");
        }

        char rules_hdr[320];
        snprintf(rules_hdr, sizeof(rules_hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n",
            pos);
        send(sn, (uint8_t *)rules_hdr, (uint16_t)strlen(rules_hdr));
        Send_Chunked(sn, (const uint8_t *)tx_buf, (uint32_t)pos);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/rules/accept  → Accept and activate pending rules (CORS)
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/rules/accept", 22) == 0) {
        RuleConfig_t newRules;
        uint8_t activated = 0;

        if (osMutexAcquire(rulesMutex, osWaitForever) == osOK) {
            if (hasPendingRules) {
                newRules = pendingRules;
                hasPendingRules = 0;
                activated = 1;
            }
            osMutexRelease(rulesMutex);
        }

        if (activated) {
            // Backup current rules
            printf("[HTTP] POST /api/rules/accept: Backing up current stable rules...\r\n");
            Partition_BackupCurrentRules();

            // Save new accepted rules to primary flash partition
            printf("[HTTP] POST /api/rules/accept: Saving accepted rules to primary flash partition...\r\n");
            Partition_SaveRules(&newRules);

            // Copy to activeRules
            if (osMutexAcquire(rulesMutex, osWaitForever) == osOK) {
                activeRules = newRules;
                
                // Set up the watchdog timer (5 seconds stability test)
                printf("[HTTP] POST /api/rules/accept: Active rules updated in RAM. Activating watchdog...\r\n");
                rulesTestTicks = osKernelGetTickCount() + pdMS_TO_TICKS(5000);
                rulesTesting = 1;
                
                osMutexRelease(rulesMutex);
            }

            char accept_log[128];
            snprintf(accept_log, sizeof(accept_log), "Pending rules version %s ACCEPTED and applied.", newRules.version_id);
            Log_Event("SYS", accept_log);

            Send_Response(sn, HTTP_200_JSON, "{\"ok\":true}");
        } else {
            Send_Response(sn, HTTP_200_JSON, "{\"ok\":false,\"error\":\"No pending rules to accept\"}");
        }
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/rules/reject  → Reject and clear pending rules (CORS)
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/rules/reject", 22) == 0) {
        char rejected_version[36] = {0};
        uint8_t cleared = 0;

        if (osMutexAcquire(rulesMutex, osWaitForever) == osOK) {
            if (hasPendingRules) {
                strncpy(rejected_version, pendingRules.version_id, sizeof(rejected_version) - 1);
                memset(&pendingRules, 0, sizeof(RuleConfig_t));
                hasPendingRules = 0;
                cleared = 1;
            }
            osMutexRelease(rulesMutex);
        }

        if (cleared) {
            char reject_log[128];
            snprintf(reject_log, sizeof(reject_log), "Pending rules version %s REJECTED.", rejected_version);
            Log_Event("SYS", reject_log);
            Send_Response(sn, HTTP_200_JSON, "{\"ok\":true}");
        } else {
            Send_Response(sn, HTTP_200_JSON, "{\"ok\":false,\"error\":\"No pending rules to reject\"}");
        }
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/rules/clear  → Delete all rules (CORS)
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/rules/clear", 21) == 0) {
        if (osMutexAcquire(rulesMutex, osWaitForever) == osOK) {
            memset(&activeRules, 0, sizeof(RuleConfig_t));
            activeRules.magic = RULES_MAGIC_CURRENT;
            strcpy(activeRules.version_id, "default");
            strcpy(activeRules.timestamp, "2026-08-30T00:00:00.000Z");
            activeRules.rule_count = 0;
            activeRules.rules_valid = 1;
            Partition_SaveRules(&activeRules);
            
            memset(&pendingRules, 0, sizeof(RuleConfig_t));
            hasPendingRules = 0;
            
            osMutexRelease(rulesMutex);
        }
        Log_Event("SYS", "All active and pending controller rules cleared.");
        Send_Response(sn, HTTP_200_JSON, "{\"ok\":true}");
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/rules  → Receive and save rules + version_id & timestamp (CORS)
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/rules", 15) == 0 || strncmp(line, "POST /api/rules/update", 22) == 0) {
        char *body = strstr(line, "\r\n\r\n");
        if (!body) {
            Send_Response(sn, HTTP_400, "{\"status\":\"error\",\"error\":\"No body\"}");
            return;
        }
        body += 4;

        cJSON *root = cJSON_Parse(body);
        if (!root) {
            Send_Response(sn, HTTP_200_JSON, "{\"status\":\"error\",\"error\":\"Invalid JSON syntax\"}");
            return;
        }

        cJSON *version_item = cJSON_GetObjectItemCaseSensitive(root, "version_id");
        cJSON *timestamp_item = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
        cJSON *rules_arr = cJSON_GetObjectItemCaseSensitive(root, "rules");

        if (!version_item || !cJSON_IsString(version_item) ||
            !timestamp_item || !cJSON_IsString(timestamp_item) ||
            !rules_arr || !cJSON_IsArray(rules_arr)) {
            cJSON_Delete(root);
            Send_Response(sn, HTTP_200_JSON, "{\"status\":\"error\",\"error\":\"Missing version_id, timestamp, or rules array\"}");
            return;
        }

        int rules_count = cJSON_GetArraySize(rules_arr);
        printf("[HTTP] POST /api/rules: Parsing %d rules, version '%s'\r\n", rules_count, version_item->valuestring);
        if (rules_count > MAX_RULES) {
            cJSON_Delete(root);
            Send_Response(sn, HTTP_200_JSON, "{\"status\":\"error\",\"error\":\"Rule count exceeds MAX_RULES\"}");
            return;
        }

        // Allocate temporary structure on the stack
        RuleConfig_t tempRules;
        memset(&tempRules, 0, sizeof(tempRules));
        tempRules.magic = RULES_MAGIC_CURRENT;
        strncpy(tempRules.version_id, version_item->valuestring, sizeof(tempRules.version_id) - 1);
        strncpy(tempRules.timestamp, timestamp_item->valuestring, sizeof(tempRules.timestamp) - 1);
        tempRules.rule_count = 0;
        tempRules.rules_valid = 0; // Starts unvalidated

        for (int i = 0; i < rules_count; i++) {
            cJSON *rule_obj = cJSON_GetArrayItem(rules_arr, i);
            if (!cJSON_IsObject(rule_obj)) {
                cJSON_Delete(root);
                Send_Response(sn, HTTP_200_JSON, "{\"status\":\"error\",\"error\":\"Rule item must be an object\"}");
                return;
            }

            cJSON *r_id = cJSON_GetObjectItemCaseSensitive(rule_obj, "rule_id");
            cJSON *in_id = cJSON_GetObjectItemCaseSensitive(rule_obj, "input_id");
            cJSON *op = cJSON_GetObjectItemCaseSensitive(rule_obj, "operator");
            cJSON *thresh = cJSON_GetObjectItemCaseSensitive(rule_obj, "threshold");
            cJSON *out_id = cJSON_GetObjectItemCaseSensitive(rule_obj, "output_id");
            cJSON *act = cJSON_GetObjectItemCaseSensitive(rule_obj, "action");

            if (!r_id || !cJSON_IsString(r_id) ||
                !in_id || (!cJSON_IsString(in_id) && !cJSON_IsNumber(in_id)) ||
                !op || !cJSON_IsString(op) ||
                !thresh || !cJSON_IsNumber(thresh) ||
                !out_id || (!cJSON_IsString(out_id) && !cJSON_IsNumber(out_id)) ||
                !act || !cJSON_IsString(act)) {
                cJSON_Delete(root);
                Send_Response(sn, HTTP_200_JSON, "{\"status\":\"error\",\"error\":\"Rule field missing or invalid type\"}");
                return;
            }

            strncpy(tempRules.rules[i].rule_id, r_id->valuestring, sizeof(tempRules.rules[i].rule_id) - 1);
            if (cJSON_IsString(in_id)) {
                strncpy(tempRules.rules[i].input_id, in_id->valuestring, sizeof(tempRules.rules[i].input_id) - 1);
            } else {
                snprintf(tempRules.rules[i].input_id, sizeof(tempRules.rules[i].input_id), "%d", in_id->valueint);
            }
            strncpy(tempRules.rules[i].operator, op->valuestring, sizeof(tempRules.rules[i].operator) - 1);
            tempRules.rules[i].threshold = (float)thresh->valuedouble;
            if (cJSON_IsString(out_id)) {
                strncpy(tempRules.rules[i].output_id, out_id->valuestring, sizeof(tempRules.rules[i].output_id) - 1);
            } else {
                snprintf(tempRules.rules[i].output_id, sizeof(tempRules.rules[i].output_id), "%d", out_id->valueint);
            }
            strncpy(tempRules.rules[i].action, act->valuestring, sizeof(tempRules.rules[i].action) - 1);
            tempRules.rules[i].active = 1;
            tempRules.rule_count++;
        }

        cJSON_Delete(root);

        // Save rules into pending structure safely
        if (osMutexAcquire(rulesMutex, osWaitForever) == osOK) {
            pendingRules = tempRules;
            hasPendingRules = 1;
            osMutexRelease(rulesMutex);
        }

        char update_log[128];
        snprintf(update_log, sizeof(update_log), "New rules version %s received (pending approval).", tempRules.version_id);
        Log_Event("SYS", update_log);

        // Build Response JSON indicating pending status and send with CORS headers
        char resp_body[128];
        int resp_len = snprintf(resp_body, sizeof(resp_body),
            "{\"status\":\"pending\",\"version_id\":\"%s\"}",
            tempRules.version_id);

        char resp_hdr[320];
        snprintf(resp_hdr, sizeof(resp_hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n",
            resp_len);
            
        send(sn, (uint8_t *)resp_hdr, (uint16_t)strlen(resp_hdr));
        send(sn, (uint8_t *)resp_body, (uint16_t)resp_len);
        return;
    }

    /* -----------------------------------------------------------------
     * GET /api/status  → JSON sensor + relay snapshot (with CORS)
     * ----------------------------------------------------------------- */
    if (strncmp(line, "GET /api/status", 15) == 0) {
        int n = JSON_StatusResponse(tx_buf, sizeof(tx_buf));
        char status_hdr[320];
        snprintf(status_hdr, sizeof(status_hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n",
            n);
        send(sn, (uint8_t *)status_hdr, (uint16_t)strlen(status_hdr));
        Send_Chunked(sn, (const uint8_t *)tx_buf, (uint32_t)n);
        return;
    }

    if (strncmp(line, "GET /api/logs", 13) == 0) {
        Send_JSON_Logs(sn);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/logs/clear  → Clear log buffer
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/logs/clear", 20) == 0) {
        taskENTER_CRITICAL();
        g_sys_log_count = 0;
        g_log_head = 0;
        g_log_tail = 0;
        g_total_logs_written = 0;
        g_sys_log_full = 0;
        memset(g_log_ring, 0, sizeof(g_log_ring));
        taskEXIT_CRITICAL();

        Log_Event("SYS", "Event log cleared. Resuming recording.");

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/relay?id=N&state=S  → Toggle individual relay
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/relay?", 16) == 0) {
        int id    = ParseQueryInt(line, "id");
        int state = ParseQueryInt(line, "state");
        Get_Shared_Config(&s_http_cfg_temp);
        if (id >= 0 && id < (int)s_http_cfg_temp.actuator_count && id < MAX_RELAYS && state >= 0) {
            char api_log[64];
            snprintf(api_log, sizeof(api_log), "HTTP API: Actuator %d set to %s", id+1, state ? "ON" : "OFF");
            Log_Event("SYS", api_log);
            Relay_SetState((uint8_t)id, (uint8_t)state);
        }
        snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":true,\"id\":%d,\"state\":%d}", id, relayStates[id]);
        
        // Include CORS headers in the response!
        char api_hdr[240];
        snprintf(api_hdr, sizeof(api_hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n",
            (int)strlen(tx_buf));
        send(sn, (uint8_t *)api_hdr, (uint16_t)strlen(api_hdr));
        send(sn, (uint8_t *)tx_buf, (uint16_t)strlen(tx_buf));
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/relay/all?state=S  → All relays
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/relay/all", 19) == 0) {
        int state = ParseQueryInt(line, "state");
        if (state >= 0) {
            char api_log[64];
            snprintf(api_log, sizeof(api_log), "HTTP API: All actuators set to %s", state ? "ON" : "OFF");
            Log_Event("SYS", api_log);
            for (int i = 0; i < MAX_RELAYS; i++) Relay_SetState((uint8_t)i, (uint8_t)state);
        }
        
        char api_resp[] = "{\"ok\":true}";
        char api_hdr[240];
        snprintf(api_hdr, sizeof(api_hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n",
            (int)strlen(api_resp));
        send(sn, (uint8_t *)api_hdr, (uint16_t)strlen(api_hdr));
        send(sn, (uint8_t *)api_resp, (uint16_t)strlen(api_resp));
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/relays  → Full replace: {"relays":[{"name","pin","nc"},...]}
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/relays", 23) == 0) {
        char *body = strstr(line, "\r\n\r\n");
        if (!body) { Send_Response(sn, HTTP_400, NULL); return; }
        body += 4;
        Get_Shared_Config(&s_http_cfg_temp);

        /* De-assert old local pins first */
        for (int i = 0; i < (int)s_http_cfg_temp.actuator_count && i < MAX_RELAYS; i++) {
            if (s_http_cfg_temp.actuators[i].type == ACTUATOR_TYPE_LOCAL_GPIO) {
                int port = GPIO_GetPortId(s_http_cfg_temp.actuators[i].port_or_ip);
                uint8_t pin = s_http_cfg_temp.actuators[i].pin_or_slave;
                if (port >= 0 && port <= 4 && pin <= 15) {
                    GPIO_TypeDef *gpio = GPIO_Ports[port];
                    gpio->MODER &= ~(3U << (pin * 2));
                    gpio->PUPDR &= ~(3U << (pin * 2));
                }
            }
        }

        uint8_t actuator_count = 0;
        Actuator_Config_t temp_actuators[MAX_RELAYS];
        memset(temp_actuators, 0, sizeof(temp_actuators));

        const char *cursor = body;
        while (actuator_count < MAX_RELAYS) {
            const char *obj = strstr(cursor, "\"name\":");
            if (!obj) break;
            const char *obj_start = obj;
            while (obj_start > body && *obj_start != '{') obj_start--;

            char name[16] = {0};
            JSON_ReadStr(obj_start, "name", name, sizeof(name));
            
            // Read Type: 0 = GPIO, 1 = Modbus TCP, 2 = OPC UA
            int type = JSON_ReadInt(obj_start, "type");
            
            temp_actuators[actuator_count].id = actuator_count;
            strncpy(temp_actuators[actuator_count].name, name, sizeof(temp_actuators[actuator_count].name) - 1);
            temp_actuators[actuator_count].type = (uint8_t)type;
            
            int active_low = JSON_ReadInt(obj_start, "nc");
            if (active_low < 0) active_low = JSON_ReadInt(obj_start, "active_low");
            temp_actuators[actuator_count].is_active_low = (active_low == 1) ? 1 : 0;
            temp_actuators[actuator_count].state = 0;

            if (type == ACTUATOR_TYPE_LOCAL_GPIO) {
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
                
                snprintf(temp_actuators[actuator_count].port_or_ip, sizeof(temp_actuators[actuator_count].port_or_ip), "P%c", 'A' + port_id);
                temp_actuators[actuator_count].pin_or_slave = pin_num;
            } 
            else if (type == ACTUATOR_TYPE_MODBUS_TCP) {
                char ip[16] = {0};
                JSON_ReadStr(obj_start, "ip", ip, sizeof(ip));
                int port = JSON_ReadInt(obj_start, "port");
                int slave = JSON_ReadInt(obj_start, "slave_id");
                int reg = JSON_ReadInt(obj_start, "reg_addr");
                
                if (port <= 0) port = 502;
                if (slave < 0) slave = 1;
                
                strncpy(temp_actuators[actuator_count].port_or_ip, ip, sizeof(temp_actuators[actuator_count].port_or_ip) - 1);
                temp_actuators[actuator_count].port = (uint16_t)port;
                temp_actuators[actuator_count].pin_or_slave = (uint8_t)slave;
                temp_actuators[actuator_count].reg_addr = (uint16_t)reg;
            } 
            else if (type == ACTUATOR_TYPE_OPC_UA_CLIENT) {
                char endpoint[16] = {0};
                JSON_ReadStr(obj_start, "ip", endpoint, sizeof(endpoint));
                int port = JSON_ReadInt(obj_start, "port");
                if (port <= 0) port = 4840;
                
                char node[32] = {0};
                JSON_ReadStr(obj_start, "opc_node_id", node, sizeof(node));
                
                strncpy(temp_actuators[actuator_count].port_or_ip, endpoint, sizeof(temp_actuators[actuator_count].port_or_ip) - 1);
                temp_actuators[actuator_count].port = (uint16_t)port;
                strncpy(temp_actuators[actuator_count].opc_node_id, node, sizeof(temp_actuators[actuator_count].opc_node_id) - 1);
            }

            actuator_count++;

            const char *close = strchr(obj_start, '}');
            if (!close) break;
            cursor = close + 1;
        }

        /* Allow empty relay list (clears config) */

        /* Build and update new config */
        Get_Shared_Config(&s_http_cfg_temp);
        
        s_http_cfg_temp.actuator_count = actuator_count;
        memcpy(s_http_cfg_temp.actuators, temp_actuators, sizeof(s_http_cfg_temp.actuators));
        s_http_cfg_temp.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&s_http_cfg_temp);

        /* Configure local GPIO pins for actuators of type GPIO */
        for (int i = 0; i < actuator_count; i++) {
            if (s_http_cfg_temp.actuators[i].type == ACTUATOR_TYPE_LOCAL_GPIO) {
                int port = GPIO_GetPortId(s_http_cfg_temp.actuators[i].port_or_ip);
                uint8_t pin = s_http_cfg_temp.actuators[i].pin_or_slave;
                if (port >= 0 && port <= 4 && pin <= 15) {
                    GPIO_InitOutput(GPIO_Ports[port], pin);
                    Relay_SetState((uint8_t)i, 0);
                }
            }
        }

        Safe_Write_Config_To_Flash(&s_http_cfg_temp, "actuators");
        
        char api_resp[] = "{\"ok\":true}";
        char api_hdr[240];
        snprintf(api_hdr, sizeof(api_hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n",
            (int)strlen(api_resp));
        send(sn, (uint8_t *)api_hdr, (uint16_t)strlen(api_hdr));
        send(sn, (uint8_t *)api_resp, (uint16_t)strlen(api_resp));
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/mqtt/confirm_topic  → Confirm pending topic from API
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/mqtt/confirm_topic", 35) == 0) {
        Get_Shared_Config(&s_http_cfg_temp);
        if (s_http_cfg_temp.pending_sparkplug_topic[0] != '\0') {
            strncpy(s_http_cfg_temp.sparkplug_topic, s_http_cfg_temp.pending_sparkplug_topic, sizeof(s_http_cfg_temp.sparkplug_topic) - 1);
            s_http_cfg_temp.sparkplug_topic[sizeof(s_http_cfg_temp.sparkplug_topic) - 1] = '\0';
            s_http_cfg_temp.pending_sparkplug_topic[0] = '\0';
            s_http_cfg_temp.magic = CONFIG_MAGIC_CURRENT;
            Update_Shared_Config(&s_http_cfg_temp);

            /* Persist to flash */
            Safe_Write_Config_To_Flash(&s_http_cfg_temp, "proposed topic approval");
            Log_Event("SYS", "HTTP API: MQTT topic proposal approved.");
        }
        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/mqtt/reject_topic  → Reject pending topic from API
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/mqtt/reject_topic", 34) == 0) {
        Get_Shared_Config(&s_http_cfg_temp);
        s_http_cfg_temp.pending_sparkplug_topic[0] = '\0';
        s_http_cfg_temp.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&s_http_cfg_temp);

        /* Persist to flash */
        Safe_Write_Config_To_Flash(&s_http_cfg_temp, "proposed topic rejection");
        Log_Event("SYS", "HTTP API: MQTT topic proposal rejected.");

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/mqtt/delete  → Clear MQTT broker settings & disconnect
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/mqtt/delete", 28) == 0) {
        Get_Shared_Config(&s_http_cfg_temp);
        
        memset(s_http_cfg_temp.mqtt_broker, 0, sizeof(s_http_cfg_temp.mqtt_broker));
        s_http_cfg_temp.mqtt_port = 1883;
        memset(s_http_cfg_temp.mqtt_client_id, 0, sizeof(s_http_cfg_temp.mqtt_client_id));
        memset(s_http_cfg_temp.mqtt_username, 0, sizeof(s_http_cfg_temp.mqtt_username));
        memset(s_http_cfg_temp.mqtt_password, 0, sizeof(s_http_cfg_temp.mqtt_password));
        memset(s_http_cfg_temp.sparkplug_topic, 0, sizeof(s_http_cfg_temp.sparkplug_topic));
        s_http_cfg_temp.mqtt_interval = 5;
        s_http_cfg_temp.mqtt_send_mode = 0;
        
        memset(s_http_cfg_temp.provision_status, 0, sizeof(s_http_cfg_temp.provision_status));
        memset(s_http_cfg_temp.provision_message, 0, sizeof(s_http_cfg_temp.provision_message));
        
        s_http_cfg_temp.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&s_http_cfg_temp);

        /* Persist to flash */
        Safe_Write_Config_To_Flash(&s_http_cfg_temp, "MQTT clear");
        Log_Event("SYS", "HTTP API: Reset MQTT settings & disconnect.");

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
        Get_Shared_Config(&s_http_cfg_temp);

        JSON_ReadStr(body, "broker",    s_http_cfg_temp.mqtt_broker,    sizeof(s_http_cfg_temp.mqtt_broker));
        JSON_ReadStr(body, "client_id", s_http_cfg_temp.mqtt_client_id, sizeof(s_http_cfg_temp.mqtt_client_id));
        JSON_ReadStr(body, "username",  s_http_cfg_temp.mqtt_username,  sizeof(s_http_cfg_temp.mqtt_username));
        JSON_ReadStr(body, "password",  s_http_cfg_temp.mqtt_password,  sizeof(s_http_cfg_temp.mqtt_password));
        JSON_ReadStr(body, "sparkplug_topic", s_http_cfg_temp.sparkplug_topic, sizeof(s_http_cfg_temp.sparkplug_topic));

        const char *port_v = JSON_FindValue(body, "port");
        if (port_v) {
            uint16_t p = 0;
            while (*port_v >= '0' && *port_v <= '9') { p = p * 10 + (*port_v - '0'); port_v++; }
            s_http_cfg_temp.mqtt_port = p;
        }

        const char *int_v = JSON_FindValue(body, "interval");
        if (int_v) {
            uint32_t iv = 0;
            while (*int_v >= '0' && *int_v <= '9') { iv = iv * 10 + (*int_v - '0'); int_v++; }
            if (iv > 0) s_http_cfg_temp.mqtt_interval = iv;
        }

        const char *sm_v = JSON_FindValue(body, "send_mode");
        if (sm_v && *sm_v >= '0' && *sm_v <= '9') {
            s_http_cfg_temp.mqtt_send_mode = (uint8_t)(*sm_v - '0');
        }

        if (s_http_cfg_temp.mqtt_broker[0] != '\0') {
            strncpy(s_http_cfg_temp.provision_status, "Active", sizeof(s_http_cfg_temp.provision_status) - 1);
            s_http_cfg_temp.provision_status[sizeof(s_http_cfg_temp.provision_status) - 1] = '\0';
            snprintf(s_http_cfg_temp.provision_message, sizeof(s_http_cfg_temp.provision_message), "Provisioned manually via Cloud MQTT settings");
            
            if (s_http_cfg_temp.device_id[0] == '\0') {
                strncpy(s_http_cfg_temp.device_id, s_http_cfg_temp.mqtt_client_id, sizeof(s_http_cfg_temp.device_id) - 1);
                s_http_cfg_temp.device_id[sizeof(s_http_cfg_temp.device_id) - 1] = '\0';
            }
        }

        s_http_cfg_temp.magic = CONFIG_MAGIC_CURRENT; /* Ensure CONFIG_MAGIC is saved */
        Update_Shared_Config(&s_http_cfg_temp);

        /* Persist */
        Safe_Write_Config_To_Flash(&s_http_cfg_temp, "MQTT");
        Log_Event("SYS", "HTTP API: Saved MQTT config.");

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/provision/delete  → Clear provisioning data
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/provision/delete", 26) == 0) {
        Get_Shared_Config(&s_http_cfg_temp);
        
        memset(s_http_cfg_temp.device_id, 0, sizeof(s_http_cfg_temp.device_id));
        memset(s_http_cfg_temp.provision_status, 0, sizeof(s_http_cfg_temp.provision_status));
        memset(s_http_cfg_temp.provision_message, 0, sizeof(s_http_cfg_temp.provision_message));
        memset(s_http_cfg_temp.sparkplug_topic, 0, sizeof(s_http_cfg_temp.sparkplug_topic));
        memset(s_http_cfg_temp.pending_sparkplug_topic, 0, sizeof(s_http_cfg_temp.pending_sparkplug_topic));
        
        s_http_cfg_temp.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&s_http_cfg_temp);

        /* Persist to flash */
        Safe_Write_Config_To_Flash(&s_http_cfg_temp, "delete provisioning");

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
        Get_Shared_Config(&s_http_cfg_temp);
        
        JSON_ReadStr(body, "device_id", s_http_cfg_temp.device_id, sizeof(s_http_cfg_temp.device_id));
        JSON_ReadStr(body, "sparkplug_topic", s_http_cfg_temp.sparkplug_topic, sizeof(s_http_cfg_temp.sparkplug_topic));
        
        /* If manual topic is empty, generate standard Sparkplug B topic path */
        if (s_http_cfg_temp.sparkplug_topic[0] == '\0') {
            snprintf(s_http_cfg_temp.sparkplug_topic, sizeof(s_http_cfg_temp.sparkplug_topic), "spBv1.0/KontrxGroup/DDATA/%s", s_http_cfg_temp.device_id);
        }
        
        strncpy(s_http_cfg_temp.provision_status, "Active", sizeof(s_http_cfg_temp.provision_status) - 1);
        s_http_cfg_temp.provision_status[sizeof(s_http_cfg_temp.provision_status) - 1] = '\0';
        
        snprintf(s_http_cfg_temp.provision_message, sizeof(s_http_cfg_temp.provision_message), "Manually provisioned via Web UI");
        
        s_http_cfg_temp.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&s_http_cfg_temp);

        /* Persist to flash */
        Safe_Write_Config_To_Flash(&s_http_cfg_temp, "manual provisioning");

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
        Get_Shared_Config(&s_http_cfg_temp);

        JSON_ReadStr(body, "deviceId",       s_http_cfg_temp.device_id,         sizeof(s_http_cfg_temp.device_id));
        JSON_ReadStr(body, "status",         s_http_cfg_temp.provision_status,  sizeof(s_http_cfg_temp.provision_status));
        JSON_ReadStr(body, "message",        s_http_cfg_temp.provision_message, sizeof(s_http_cfg_temp.provision_message));

        /* Parse MQTT Broker IP — try all field name variants from mobile app */
        char broker[64] = {0};
        JSON_ReadStr(body, "mqttBroker", broker, sizeof(broker));
        if (broker[0] == '\0') JSON_ReadStr(body, "brokerUrl", broker, sizeof(broker));
        if (broker[0] == '\0') JSON_ReadStr(body, "broker",    broker, sizeof(broker));
        if (broker[0] != '\0') {
            strncpy(s_http_cfg_temp.mqtt_broker, broker, sizeof(s_http_cfg_temp.mqtt_broker) - 1);
            s_http_cfg_temp.mqtt_broker[sizeof(s_http_cfg_temp.mqtt_broker) - 1] = '\0';
        }

        /* Parse broker port — default to 1883 (standard MQTT) if not provided */
        const char *port_v = JSON_FindValue(body, "port");
        if (port_v && *port_v >= '0' && *port_v <= '9') {
            uint16_t p = 0;
            while (*port_v >= '0' && *port_v <= '9') { p = p * 10 + (*port_v - '0'); port_v++; }
            if (p > 0) s_http_cfg_temp.mqtt_port = p;
        }
        if (s_http_cfg_temp.mqtt_port == 0) {
            s_http_cfg_temp.mqtt_port = 1883; /* default MQTT port */
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
                strncpy(s_http_cfg_temp.pending_sparkplug_topic, prov_topic, sizeof(s_http_cfg_temp.pending_sparkplug_topic) - 1);
                s_http_cfg_temp.pending_sparkplug_topic[sizeof(s_http_cfg_temp.pending_sparkplug_topic) - 1] = '\0';
            }
        }

        s_http_cfg_temp.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&s_http_cfg_temp);

        /* Persist to Flash */
        Safe_Write_Config_To_Flash(&s_http_cfg_temp, "mobile provisioning");

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
        Get_Shared_Config(&s_http_cfg_temp);
        s_http_cfg_temp.serial = s;
        s_http_cfg_temp.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&s_http_cfg_temp);

        /* Persist */
        Safe_Write_Config_To_Flash(&s_http_cfg_temp, "serial");

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
        Get_Shared_Config(&s_http_cfg_temp);

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

        s_http_cfg_temp.sensors.count = s_count;
        memset(s_http_cfg_temp.sensors.entries, 0, sizeof(s_http_cfg_temp.sensors.entries));
        for (int i = 0; i < s_count; i++) {
            s_http_cfg_temp.sensors.entries[i].type = s_types[i];
            s_http_cfg_temp.sensors.entries[i].id   = s_ids[i];
        }

        s_http_cfg_temp.magic = CONFIG_MAGIC_CURRENT;
        Update_Shared_Config(&s_http_cfg_temp);

        /* Persist */
        Safe_Write_Config_To_Flash(&s_http_cfg_temp, "sensors");

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

        /* Backup configuration from Sector 7 tail before it is erased */
        Get_Shared_Config(&s_http_cfg_temp);

        FLASH_EraseSector(6);
        FLASH_EraseSector(7);

        /* Restore configuration to Sector 7 tail immediately */
        s_http_cfg_temp.magic = CONFIG_MAGIC_CURRENT;
        FLASH_WriteBuffer(CONFIG_FLASH_ADDR, (uint8_t *)&s_http_cfg_temp, sizeof(Gateway_Config_t));

        Log_Event("OTA", "Flash sectors 6 & 7 prepared for update.");

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
        /* Suspend Modbus and MQTT tasks during firmware streaming to prevent CPU starvation and SPI contention */
        extern osThreadId_t g_tid_modbus;
        extern osThreadId_t g_tid_mqtt;
        if (g_tid_modbus) vTaskSuspend((TaskHandle_t)g_tid_modbus);
        if (g_tid_mqtt)   vTaskSuspend((TaskHandle_t)g_tid_mqtt);

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
            Log_Event("OTA", "Firmware upload successful. Device is rebooting...");
            const char *resp = "HTTP/1.1 200 OK\r\nContent-Type:text/plain\r\nConnection:close\r\n\r\n"
                               "OTA OK. Rebooting...";
            send(sn, (uint8_t *)resp, strlen(resp));
        } else {
            Log_Event("OTA", "Firmware upload failed (CRC check failed).");
            const char *resp = "HTTP/1.1 500 Internal Server Error\r\nConnection:close\r\n\r\nOTA CRC FAIL";
            send(sn, (uint8_t *)resp, strlen(resp));
            /* Resume suspended tasks since update failed */
            if (g_tid_modbus) vTaskResume((TaskHandle_t)g_tid_modbus);
            if (g_tid_mqtt)   vTaskResume((TaskHandle_t)g_tid_mqtt);
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
        uint8_t state = getSn_SR(HTTP_SOCK);
        static uint32_t last_heartbeat = 0;
        uint32_t now = osKernelGetTickCount();
        if (now - last_heartbeat >= 2000) {
            printf("[HTTP] Heartbeat: socket state = 0x%02X\r\n", state);
            last_heartbeat = now;
        }

        switch (state) {

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
            osDelay(20);
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
            printf("[HTTP] SOCK_ESTABLISHED: Browser connected!\r\n");
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
                    printf("[HTTP] Received %u bytes of request data\r\n", size);
                    if (size > (uint16_t)(sizeof(rx_buf) - 1))
                        size = (uint16_t)(sizeof(rx_buf) - 1);
                    recv(HTTP_SOCK, rx_buf, size);
                    Dispatch_Request(HTTP_SOCK, rx_buf, size);
                    
                    /* Flush any leftover/overflow bytes in the socket RX buffer
                     * before closing. If we close a socket with unread data in the
                     * RX buffer, the W5500 will send a TCP RST packet to the browser,
                     * causing ERR_CONNECTION_RESET or ERR_CONNECTION_REFUSED. */
                    uint16_t rem_size;
                    while ((rem_size = getSn_RX_RSR(HTTP_SOCK)) > 0) {
                        uint16_t discard_len = (rem_size > sizeof(rx_buf)) ? sizeof(rx_buf) : rem_size;
                        recv(HTTP_SOCK, rx_buf, discard_len);
                        osDelay(1);
                    }
                    
                    /* Wait for W5500 hardware TX buffer to be fully sent and ACKed by the client.
                     * For a 2KB socket buffer, getSn_TX_FSR returning 2048 means the buffer is empty. */
                    for (int wait_ack = 0; wait_ack < 200; wait_ack++) {
                        if (getSn_TX_FSR(HTTP_SOCK) >= 2048) {
                            break;
                        }
                        osDelay(1);
                    }
                    disconnect(HTTP_SOCK);
                } else {
                    printf("[HTTP] No request bytes received (size=0), disconnecting...\r\n");
                    /* Socket leak fix: disconnect if no request bytes received */
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

        /* Transient states (handshake & teardown): do nothing and yield */
        case SOCK_SYNSENT:
        case SOCK_SYNRECV:
        case SOCK_LAST_ACK:
            break;

        default:
            printf("[HTTP] Unknown socket state: 0x%02X\r\n", state);
            osDelay(1000);
            break;
        }

        osDelay(1); /* 1ms yield per loop — keeps the W5500 state machine
                     * responsive (~1KB API responses) without starving the
                     * 1ms Control Engine. Exact 1ms, does not exceed budget. */
    }
}

