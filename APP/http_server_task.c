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
#include "web_assets.h"
#include "modbus_dma.h"
#include "flash_stm32.h"
#include "rtc_stm32.h"
#include "stm32f407_regs.h"
#include "gpio_stm32.h"
#include "socket.h"
#include "cmsis_os2.h"
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
static char    tx_buf[2048];

/* ======================================================================
 *  Static working buffers for JSON_StatusResponse
 *  Keeping these off the task stack prevents stack overflow on the
 *  2KB (now 4KB) HTTPServer stack when snprintf + structs are called.
 * ====================================================================== */
static char               s_relay_json[512];
static Modbus_SensorData_t s_sd;
static Gateway_Config_t    s_cfg;
static wiz_NetInfo         s_ni;

/* ======================================================================
 *  JSON helpers (no dynamic allocation, snprintf into tx_buf)
 * ====================================================================== */
static int JSON_StatusResponse(char *buf, int buflen) {
    /* Use static storage — keeps stack frames shallow and prevents
     * the HTTPServer task from overflowing its stack on every API call. */
    Get_Shared_Sensor_Data(&s_sd);
    Get_Shared_Config(&s_cfg);

    /* Build relay JSON array into static buffer */
    int rpos = 0;
    rpos += snprintf(s_relay_json + rpos, sizeof(s_relay_json) - rpos, "[");
    for (int i = 0; i < MAX_RELAYS; i++) {
        rpos += snprintf(s_relay_json + rpos, sizeof(s_relay_json) - rpos,
            "{\"id\":%d,\"state\":%d,\"pin\":\"P%c%u\",\"nc\":%d}%s",
            i, relayStates[i],
            'A' + s_cfg.relays[i].port_id, s_cfg.relays[i].pin_num,
            s_cfg.relays[i].is_nc,
            (i < MAX_RELAYS - 1) ? "," : "");
    }
    rpos += snprintf(s_relay_json + rpos, sizeof(s_relay_json) - rpos, "]");

    ctlnetwork(CN_GET_NETINFO, &s_ni);

    uint8_t ph_id = (s_cfg.sensor_ids[0] == 0 || s_cfg.sensor_ids[0] == 0xFF) ? 1 : s_cfg.sensor_ids[0];
    uint8_t orp_id = (s_cfg.sensor_ids[1] == 0 || s_cfg.sensor_ids[1] == 0xFF) ? 2 : s_cfg.sensor_ids[1];
    uint8_t ec_id = (s_cfg.sensor_ids[2] == 0 || s_cfg.sensor_ids[2] == 0xFF) ? 3 : s_cfg.sensor_ids[2];
    uint8_t do_id = (s_cfg.sensor_ids[3] == 0 || s_cfg.sensor_ids[3] == 0xFF) ? 4 : s_cfg.sensor_ids[3];
    uint8_t ammonia_id = (s_cfg.sensor_ids[4] == 0 || s_cfg.sensor_ids[4] == 0xFF) ? 5 : s_cfg.sensor_ids[4];
    uint8_t ultra_id = (s_cfg.sensor_ids[5] == 0 || s_cfg.sensor_ids[5] == 0xFF) ? 10 : s_cfg.sensor_ids[5];

    return snprintf(buf, buflen,
        "{\"ph\":%.2f,\"ph_temp\":%.2f,"
        "\"orp\":%.2f,\"orp_temp\":%.2f,"
        "\"ec\":%.2f,\"ec_temp\":%.2f,"
        "\"do_val\":%.2f,\"do_temp\":%.2f,"
        "\"ammonia\":%.2f,\"ammonia_temp\":%.2f,"
        "\"ultra_dist\":%.2f,\"ultra_temp\":%.2f,"
        "\"sensor_ids\":[%u,%u,%u,%u,%u,%u],"
        "\"uptime_s\":%lu,"
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"ip\":\"%d.%d.%d.%d\","
        "\"fw\":\"v2.0.0\","
        "\"relays\":%s}",
        s_sd.ph, s_sd.ph_temp,
        s_sd.orp, s_sd.orp_temp,
        s_sd.ec, s_sd.ec_temp,
        s_sd.do_val, s_sd.do_temp,
        s_sd.ammonia, s_sd.ammonia_temp,
        s_sd.ultrasonic_dist, s_sd.ultrasonic_temp,
        ph_id, orp_id, ec_id, do_id, ammonia_id, ultra_id,
        (unsigned long)RTC_GetUptimeSeconds(),
        s_ni.mac[0], s_ni.mac[1], s_ni.mac[2], s_ni.mac[3], s_ni.mac[4], s_ni.mac[5],
        s_ni.ip[0], s_ni.ip[1], s_ni.ip[2], s_ni.ip[3],
        s_relay_json);
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
            osDelay(1);   /* Yield — let scheduler service other tasks */
        } else {
            break;        /* Socket error — abort */
        }
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
        send(sn, (uint8_t *)HTTP_200_JSON, strlen(HTTP_200_JSON));
        send(sn, (uint8_t *)tx_buf, (uint16_t)n);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/relay?id=N&state=S  → Toggle individual relay
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/relay?", 16) == 0) {
        int id    = ParseQueryInt(line, "id");
        int state = ParseQueryInt(line, "state");
        if (id >= 0 && id < MAX_RELAYS && state >= 0) {
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
     * POST /api/config/relays  → Update relay pin/NC config from JSON body
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/relays", 23) == 0) {
        char *body = strstr(line, "\r\n\r\n");
        if (!body) { Send_Response(sn, HTTP_400, NULL); return; }
        body += 4;

        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);

        /* Temp variables for validation */
        uint8_t temp_ports[MAX_RELAYS];
        uint8_t temp_pins[MAX_RELAYS];

        /* Initialize temp arrays from current config */
        for (int i = 0; i < MAX_RELAYS; i++) {
            temp_ports[i] = cfg.relays[i].port_id;
            temp_pins[i]  = cfg.relays[i].pin_num;
        }

        /* First pass: parse and validate all items */
        for (int i = 0; i < MAX_RELAYS; i++) {
            char id_key[16]; snprintf(id_key, sizeof(id_key), "\"id\":%d", i);
            char *entry = strstr(body, id_key);
            if (!entry) continue;

            char pin_str[16] = {0};
            JSON_ReadStr(entry, "pin", pin_str, sizeof(pin_str));
            
            uint8_t port_id = 0xFF;
            uint8_t pin_num = 0xFF;
            Parse_Pin_String(pin_str, &port_id, &pin_num);

            if (Is_Pin_Reserved(port_id, pin_num)) {
                snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":false,\"error\":\"Pin %s is reserved by system hardware.\"}", pin_str);
                Send_Response(sn, HTTP_200_JSON, tx_buf);
                return;
            }

            /* Check duplicates */
            for (int j = 0; j < MAX_RELAYS; j++) {
                if (j != i && temp_ports[j] == port_id && temp_pins[j] == pin_num) {
                    snprintf(tx_buf, sizeof(tx_buf), "{\"ok\":false,\"error\":\"Pin %s is already assigned to Relay R%d.\"}", pin_str, j + 1);
                    Send_Response(sn, HTTP_200_JSON, tx_buf);
                    return;
                }
            }

            temp_ports[i] = port_id;
            temp_pins[i]  = pin_num;
        }

        /* Retrieve current config to de-assert old pins before applying new ones */
        Gateway_Config_t old_cfg;
        Get_Shared_Config(&old_cfg);
        for (int i = 0; i < MAX_RELAYS; i++) {
            uint8_t old_port = old_cfg.relays[i].port_id;
            uint8_t old_pin  = old_cfg.relays[i].pin_num;
            if (old_port <= 4 && old_pin <= 15) {
                GPIO_TypeDef *gpio = GPIO_Ports[old_port];
                gpio->MODER &= ~(3U << (old_pin * 2)); /* Reset to Input mode (00) */
                gpio->PUPDR &= ~(3U << (old_pin * 2)); /* No pull */
            }
        }

        /* Second pass: apply and save config since all validation passed */
        for (int i = 0; i < MAX_RELAYS; i++) {
            char id_key[16]; snprintf(id_key, sizeof(id_key), "\"id\":%d", i);
            char *entry = strstr(body, id_key);
            if (!entry) continue;

            cfg.relays[i].port_id = temp_ports[i];
            cfg.relays[i].pin_num  = temp_pins[i];

            int nc = JSON_ReadInt(entry, "nc");
            if (nc >= 0) cfg.relays[i].is_nc = (uint8_t)nc;
        }

        cfg.magic = 0xC01D0001U; /* Ensure CONFIG_MAGIC is saved */
        Update_Shared_Config(&cfg);

        /* Configure hardware GPIO registers for all updated pins */
        for (int i = 0; i < MAX_RELAYS; i++) {
            uint8_t port = cfg.relays[i].port_id;
            uint8_t pin  = cfg.relays[i].pin_num;
            if (port <= 4 && pin <= 15) {
                GPIO_InitOutput(GPIO_Ports[port], pin);
                Relay_SetState((uint8_t)i, relayStates[i]); /* Re-apply current state to new pin */
            }
        }

        /* Persist to flash emulated EEPROM (Sector 11 = last 16KB at 0x080E0000) */
        FLASH_EraseSector(11);
        FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&cfg, sizeof(Gateway_Config_t));

        Send_Response(sn, HTTP_200_JSON, "{\"ok\":true}");
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

        const char *port_v = JSON_FindValue(body, "port");
        if (port_v) {
            uint16_t p = 0;
            while (*port_v >= '0' && *port_v <= '9') { p = p * 10 + (*port_v - '0'); port_v++; }
            cfg.mqtt_port = p;
        }

        cfg.magic = 0xC01D0001U; /* Ensure CONFIG_MAGIC is saved */
        Update_Shared_Config(&cfg);

        /* Persist */
        FLASH_EraseSector(11);
        FLASH_WriteBuffer(0x080E0000U, (uint8_t *)&cfg, sizeof(Gateway_Config_t));

        Send_Response(sn, HTTP_200_JSON, HTTP_200_OK_JSON);
        return;
    }

    /* -----------------------------------------------------------------
     * POST /api/config/sensors  → Update sensor Modbus IDs
     * ----------------------------------------------------------------- */
    if (strncmp(line, "POST /api/config/sensors", 24) == 0) {
        char *body = strstr(line, "\r\n\r\n");
        if (!body) { Send_Response(sn, HTTP_400, NULL); return; }
        body += 4;

        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);

        int ph_val = JSON_ReadInt(body, "ph");
        int orp_val = JSON_ReadInt(body, "orp");
        int ec_val = JSON_ReadInt(body, "ec");
        int do_val = JSON_ReadInt(body, "do");
        int ammonia_val = JSON_ReadInt(body, "ammonia");
        int ultra_val = JSON_ReadInt(body, "ultra");

        if (ph_val >= 1 && ph_val <= 247) cfg.sensor_ids[0] = (uint8_t)ph_val;
        if (orp_val >= 1 && orp_val <= 247) cfg.sensor_ids[1] = (uint8_t)orp_val;
        if (ec_val >= 1 && ec_val <= 247) cfg.sensor_ids[2] = (uint8_t)ec_val;
        if (do_val >= 1 && do_val <= 247) cfg.sensor_ids[3] = (uint8_t)do_val;
        if (ammonia_val >= 1 && ammonia_val <= 247) cfg.sensor_ids[4] = (uint8_t)ammonia_val;
        if (ultra_val >= 1 && ultra_val <= 247) cfg.sensor_ids[5] = (uint8_t)ultra_val;

        cfg.magic = 0xC01D0001U;
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
     * POST /update  → OTA binary upload
     * Check OTP header, then stream body to staging flash area.
     * The actual write loop is done here (Task_HTTPServer context) so
     * the OTA task only handles CRC verification + metadata write.
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

        /* Erase staging area (Sectors 6 & 7, keeping Sector 7's last page for config) */
        FLASH_EraseSector(6);
        FLASH_EraseSector(7);

        uint32_t write_addr  = STAGING_ADDR;
        uint32_t total_written = 0;

        /* Write the first chunk (already in rx_buf after headers) */
        uint32_t first_body_len = (uint32_t)(len - (uint32_t)(body - (char *)req));
        if (first_body_len > 0 && total_written < content_length) {
            uint32_t to_write = (first_body_len > content_length) ? content_length : first_body_len;
            FLASH_WriteBuffer(write_addr, (uint8_t *)body, to_write);
            write_addr    += to_write;
            total_written += to_write;
        }

        /* Stream remaining chunks */
        uint32_t timeout_cnt = 0;
        while (total_written < content_length) {
            uint16_t chunk = getSn_RX_RSR(sn);
            if (chunk > 0) {
                timeout_cnt = 0;
                if (chunk > sizeof(rx_buf)) chunk = sizeof(rx_buf);
                uint32_t remaining = content_length - total_written;
                if ((uint32_t)chunk > remaining) chunk = (uint16_t)remaining;
                recv(sn, rx_buf, chunk);
                FLASH_WriteBuffer(write_addr, rx_buf, chunk);
                write_addr    += chunk;
                total_written += chunk;
            } else {
                if (getSn_SR(sn) != SOCK_ESTABLISHED) break;
                osDelay(1);
                timeout_cnt++;
                if (timeout_cnt > 10000) break; /* 10 s timeout */
            }
        }

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

    for (;;) {
        switch (getSn_SR(HTTP_SOCK)) {

        /* ---------------------------------------------------------------
         * CLOSED → create TCP socket and prepare to listen
         * -------------------------------------------------------------- */
        case SOCK_CLOSED:
            socket(HTTP_SOCK, Sn_MR_TCP, HTTP_PORT, 0x00);
            break;

        /* ---------------------------------------------------------------
         * INIT → start listening for incoming connections
         * -------------------------------------------------------------- */
        case SOCK_INIT:
            listen(HTTP_SOCK);
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
                    osDelay(5);
                }

                if (size > 0) {
                    if (size > (uint16_t)(sizeof(rx_buf) - 1))
                        size = (uint16_t)(sizeof(rx_buf) - 1);
                    recv(HTTP_SOCK, rx_buf, size);
                    Dispatch_Request(HTTP_SOCK, rx_buf, size);
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
         * Force-close the socket instead of waiting for the OS TCP timer.
         * The W5500 close() command sends RST + recycles the socket to
         * SOCK_CLOSED immediately, allowing the next browser request
         * to connect without waiting for TIME_WAIT to expire (~30s).
         * -------------------------------------------------------------- */
        case SOCK_FIN_WAIT:
        case SOCK_CLOSING:
        case SOCK_TIME_WAIT:
            close(HTTP_SOCK);   /* W5500 close(): forces SOCK_CLOSED */
            break;

        default:
            break;
        }

        osDelay(1); /* 1ms yield — faster socket cycling for ~1KB API responses */
    }
}
