/**
 * @file    freertos_tasks.c
 * @brief   Kontrx — FreeRTOS Task Architecture & IPC Coordinator
 *
 * Task Map:
 * ┌─────────────────────────────────┬──────────┬─────────┬─────────────────────────────┐
 * │ Task                            │ Priority │ Stack   │ Period / Trigger             │
 * ├─────────────────────────────────┼──────────┼─────────┼─────────────────────────────┤
 * │ Task_ControlEngine              │ RT (5)   │ 512 B   │ Every 1ms (1000 Hz) ← ONLY  │
 * │ Task_ModbusSensorPoll           │ High (3) │ 4096 B  │ Continuous — no artificial   │
 * │                                 │          │         │ sleep; one poll cycle = N ×  │
 * │                                 │          │         │ (tx+rx+35ms gap) @ 9600 baud │
 * │ Task_HTTPServer                 │ Norm (2) │ 8192 B  │ W5500 socket event           │
 * │ Task_OTAUpdate                  │ Low (1)  │ 2048 B  │ sem_ota_start from HTTP      │
 * └─────────────────────────────────┴──────────┴─────────┴─────────────────────────────┘
 *
 * NOTE: Task_ControlEngine runs at exactly 1ms (vTaskDelayUntil). The Modbus poll
 * task runs continuously — each cycle takes N × ~75ms (transaction+gap per sensor).
 * No artificial osDelay is inserted between cycles: the goal is to maximise the
 * sample count that ConsumeBatch() will average over the MQTT publish interval.
 * The 1ms Control Engine is never blocked: priority 5 > 3, and it uses a
 * non-blocking osMutexAcquire(..., 0) + vTaskDelayUntil.
 *
 * IPC:
 *   sensorMutex  — guards Modbus_SensorData_t (modbus_dma.c ↔ http_server_task.c)
 *   configMutex  — guards Gateway_Config_t    (modbus_dma.c ↔ http_server_task.c)
 *   sem_ota_start— HTTP → OTA task handoff
 *   sem_ota_done — OTA → HTTP task completion signal
 *
 * 1ms Scan Cycle Design:
 *   The Control Engine reads the latest sensor snapshot atomically (no DMA wait),
 *   evaluates the PID/threshold control logic, and sets relay outputs.
 *   It blocks on vTaskDelayUntil() to achieve a strict 1000 Hz period.
 *   ALL blocking or I/O operations are FORBIDDEN inside this task.
 */

#include "freertos_tasks.h"
#include "modbus_dma.h"
#include "http_server_task.h"
#include "ota_task.h"
#include "rtc_stm32.h"
#include "MQTTClient.h"
#include "mqtt_interface.h"
#include "sparkplug_b_enc.h"
#include "socket.h"
#include "led.h"


/* Sparkplug B Topic Helper */
static void get_sparkplug_topic(const char *base_topic, const char *msg_type, char *out_topic, size_t max_len) {
    if (base_topic[0] != '\0' && strstr(base_topic, "/DDATA/") != NULL) {
        char temp[128];
        strncpy(temp, base_topic, sizeof(temp)-1);
        temp[sizeof(temp)-1] = '\0';
        char *ddata_ptr = strstr(temp, "/DDATA/");
        if (ddata_ptr) {
            *ddata_ptr = '\0';
            snprintf(out_topic, max_len, "%s/%s/%s", temp, msg_type, ddata_ptr + 7);
            return;
        }
    }
    /* Fallback standard Sparkplug B topic format */
    snprintf(out_topic, max_len, "spBv1.0/KontrxGroup/%s/kontrx-%07lu", msg_type, (unsigned long)sharedConfig.serial);
}
#include "stm32f407_regs.h"
#include "gpio_stm32.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "portable.h"
#include <stdio.h>

volatile MqttStatus_t g_mqtt_status = {0};

/* ======================================================================
 *  Global uptime counter
 * ====================================================================== */
volatile uint32_t g_uptime_seconds = 0;

/* CPU usage (0-100%) — updated once per second by idle hook */
volatile uint8_t g_cpu_usage_pct = 0;

/* FreeRTOS tick hook — increments uptime every second */
static volatile uint32_t g_tick_counter = 0;

void vApplicationTickHook(void) {
    g_tick_counter++;
    if (g_tick_counter >= (uint32_t)configTICK_RATE_HZ) {
        g_tick_counter = 0;
        g_uptime_seconds++;
    }
}

/* ======================================================================
 *  CPU Usage via Cortex-M DWT cycle counter
 *  The idle hook accumulates idle cycles; once per second the tick hook
 *  would need to snapshot — but simpler: measure idle cycles in the hook
 *  itself over a rolling 1-second window using uptime.
 * ====================================================================== */
static volatile uint32_t g_idle_cycles      = 0; /* idle cycles accumulated this window */
static volatile uint32_t g_idle_window_s    = 0; /* uptime snapshot at window start */
static volatile uint32_t g_idle_start_cyc   = 0; /* DWT CYCCNT at window start (for total) */

/* DWT registers — Cortex-M4 Data Watchpoint and Trace unit */
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000U)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004U)
#define DEM_CR     (*(volatile uint32_t *)0xE000EDFCU)
#define DEM_CR_TRCENA (1UL << 24)

void KontrxDWT_Init(void) {
    DEM_CR   |= DEM_CR_TRCENA; /* enable trace subsystem */
    /* Unlock DWT LAR (Lock Access Register) to allow write access to DWT registers */
    volatile uint32_t *dwt_lar = (volatile uint32_t *)0xE0001FB0U;
    *dwt_lar = 0xC5ACCE55U;
    DWT_CYCCNT = 0;
    DWT_CTRL  |= 1UL;           /* enable cycle counter */
}

void vApplicationIdleHook(void) {
    /* Accumulate idle cycles using DWT cycle counter */
    static uint32_t last_cyc = 0;
    uint32_t now = DWT_CYCCNT;
    uint32_t delta = now - last_cyc; /* wraps safely on 32-bit */
    last_cyc = now;
    g_idle_cycles += delta;

    /* Once per second: compute CPU % from idle/total cycle ratio */
    uint32_t cur_s = g_uptime_seconds;
    if (cur_s != g_idle_window_s) {
        uint32_t window_cyc = now - g_idle_start_cyc;
        if (window_cyc > 0) {
            /* idle% = idle_cycles / total_cycles */
            uint32_t idle_pct = (g_idle_cycles * 100U) / window_cyc;
            if (idle_pct > 100U) idle_pct = 100U;
            g_cpu_usage_pct = (uint8_t)(100U - idle_pct);
        }
        g_idle_cycles    = 0;
        g_idle_start_cyc = now;
        g_idle_window_s  = cur_s;
    }
}



/* Default relay pin table (mirrors web_assets.h JS initial state) */
/* Format: {port_id, pin_num}  A=0 B=1 C=2 D=3 E=4 */
static const struct { uint8_t port; uint8_t pin; const char *name; } relay_defaults[MAX_RELAYS] = {
    {4, 2,  "Relay1"},  /* R1  PE2 */
    {4, 4,  "Relay2"},  /* R2  PE4 */
    {4, 6,  "Relay3"},  /* R3  PE6 */
    {2, 0,  "Relay4"},  /* R4  PC0 */
    {2, 2,  "Relay5"},  /* R5  PC2 */
    {0, 0,  "Relay6"},  /* R6  PA0 */
    {0, 2,  "Relay7"},  /* R7  PA2 */
    {0, 4,  "Relay8"},  /* R8  PA4 */
    {0xFF, 0xFF, "Relay9"},   /* R9 disabled by default */
    {3, 8,  "Relay10"}, /* R10 PD8 */
    {0xFF, 0xFF, ""},   /* R11+ unused */
    {0xFF, 0xFF, ""},
    {0xFF, 0xFF, ""},
    {0xFF, 0xFF, ""},
    {0xFF, 0xFF, ""},
    {0xFF, 0xFF, ""},
};

static uint8_t Relay_Pin_Is_Reserved(uint8_t port, uint8_t pin) {
    if (port > 4 || pin > 15) return 1;

    if (port == 0 && (pin == 6 || pin == 9 || pin == 10)) return 1;  /* LED, USART1 */
    if (port == 1 && pin >= 10 && pin <= 15) return 1;                /* Modbus, W5500 SPI */
    if (port == 2 && (pin == 6 || pin == 7)) return 1;                /* USART6 */
    if (port == 3 && (pin == 2 || pin == 3)) return 1;                /* MAX485 RE#/DE */
    return 0;
}

/* ======================================================================
 *  Relay GPIO Initialisation
 * ====================================================================== */
void Relay_Init(void) {
    Gateway_Config_t cfg;
    Get_Shared_Config(&cfg);

    if (cfg.magic != CONFIG_MAGIC_CURRENT) {
        memset(&cfg, 0, sizeof(cfg));
        cfg.magic = CONFIG_MAGIC_CURRENT;
        cfg.sensors.count = 0;
        cfg.relay_count = 10;
        cfg.serial = 1; /* first device gets serial 1 => "KX-0000001" */
        cfg.mqtt_port = 1883;
        cfg.mqtt_interval = 1; /* default to 1 second */
        cfg.mqtt_send_mode = 0; /* default to 0 = On Interval (Periodic) */
        for (int i = 0; i < MAX_RELAYS; i++) {
            cfg.relays[i].port_id = relay_defaults[i].port;
            cfg.relays[i].pin_num = relay_defaults[i].pin;
            cfg.relays[i].is_nc   = 0;
            cfg.relays[i].state   = 0;
            strncpy(cfg.relays[i].name, relay_defaults[i].name, sizeof(cfg.relays[i].name) - 1);
        }
        Update_Shared_Config(&cfg);
    }

    for (int i = 0; i < (int)cfg.relay_count && i < MAX_RELAYS; i++) {
        uint8_t port = cfg.relays[i].port_id;
        uint8_t pin  = cfg.relays[i].pin_num;

        if (Relay_Pin_Is_Reserved(port, pin)) {
            printf("[Relay] %s pin P%c%u is reserved; relay disabled\r\n",
                   cfg.relays[i].name[0] ? cfg.relays[i].name : "Relay",
                   (port <= 4) ? ('A' + port) : '?', pin);
            continue;
        }

        GPIO_TypeDef *gpio = GPIO_Ports[port];

        /* Enable GPIO clock: AHB1ENR bit position = port index */
        RCC->AHB1ENR |= (1U << port);

        /* Output push-pull */
        gpio->MODER  &= ~(3U << (pin * 2));
        gpio->MODER  |=  (1U << (pin * 2));
        gpio->OTYPER &= ~(1U << pin);
        gpio->OSPEEDR|=  (2U << (pin * 2)); /* High speed */
        gpio->PUPDR  &= ~(3U << (pin * 2));

        /* Initialise to OFF state (deenergised) */
        Relay_SetState((uint8_t)i, 0);
    }
}

/* ======================================================================
 *  Relay_SetState — thread-safe relay drive with NC inversion support
 * ====================================================================== */
void Relay_SetState(uint8_t idx, uint8_t state) {
    if (idx >= MAX_RELAYS) return;

    Gateway_Config_t cfg;
    Get_Shared_Config(&cfg);

    uint8_t port   = cfg.relays[idx].port_id;
    uint8_t pin    = cfg.relays[idx].pin_num;
    uint8_t is_nc  = cfg.relays[idx].is_nc;

    if (Relay_Pin_Is_Reserved(port, pin)) return;

    GPIO_TypeDef *gpio = GPIO_Ports[port];

    /* NC logic: for NC relay, invert the drive signal */
    uint8_t drive = (is_nc ? !state : state);

    if (drive) {
        gpio->BSRR = (1U << pin);            /* Set pin HIGH */
    } else {
        gpio->BSRR = (1U << (pin + 16));     /* Set pin LOW */
    }

    relayStates[idx] = state;

    cfg.relays[idx].state = state;
    Update_Shared_Config(&cfg);
}

/* ======================================================================
 *  TASK 1: High-Speed Control Engine — Priority: Real-Time (5)
 *  Period: 1ms (1000 Hz) — the ONLY strict 1ms path in the system.
 *  CONSTRAINT: NO blocking calls, NO I/O, NO DMA waits.
 *
 *  WHY IT STAYS EXACTLY AT 1ms:
 *   - vTaskDelayUntil() (not vTaskDelay) = absolute deadline, no drift.
 *   - The body is a mutex copy (non-blocking, timeout=0) + user logic.
 *   - Priority 5 > Modbus(3) > HTTP(2) > OTA(1), so slower tasks can only
 *     run in the scheduler slots BETWEEN 1ms cycles, never inside one.
 * ====================================================================== */
static void Task_ControlEngine(void *arg) {
    (void)arg;

    TickType_t   xLastWakeTime = xTaskGetTickCount();
    const TickType_t xPeriod   = pdMS_TO_TICKS(1); /* 1ms */

    Modbus_SensorData_t sd = { .last_update_time = 0 };

    for (;;) {
        /* --- Read latest sensor snapshot (mutex-protected, non-blocking) --- */
        /* Use TryAcquire so we NEVER block if Modbus task holds the mutex */
        if (osMutexAcquire(sensorMutex, 0) == osOK) {
            /* Take a local copy of the shared data */
            sd = sharedSensorData;
            osMutexRelease(sensorMutex);
        }

        if (sd.last_update_time == 0) {
            vTaskDelayUntil(&xLastWakeTime, xPeriod);
            continue;
        }
        /* If mutex was unavailable, we proceed with the last known snapshot — 
         * this is intentional; the control engine never stalls for sensor data. */

/* ================================================================
         * --- CONTROL LOGIC ZONE (user-defined) ---
         * ================================================================ */

        /* ================================================================ */

        /* Strict 1ms delay — yields back to scheduler until next cycle */
        vTaskDelayUntil(&xLastWakeTime, xPeriod);
    }
}

/* ======================================================================
 *  TASK 2: Modbus Sensor Polling — Priority: Above Normal (3)
 *  Runs CONTINUOUSLY with no artificial inter-cycle sleep.
 *
 *  Design:
 *    Each call to Modbus_DMA_PollSensors() polls all configured sensors
 *    once. Successful reads are accumulated into sum_value / sample_count
 *    inside sharedSensorData. The MQTT task calls Modbus_DMA_ConsumeBatch()
 *    every mqtt_interval seconds to compute the average over ALL samples
 *    collected since the last publish, then resets the accumulators.
 *
 *  Timing at 9600 baud (per sensor per cycle):
 *    Fast sensor (pH/EC/ORP/Ammonia/US) : ~20ms tx+rx + 35ms gap = ~55ms
 *    Slow sensor (DO KWS-630)           : ~20ms tx+rx + 35ms gap = ~55ms
 *    Multi-US (2 channels per cycle)    : 2×(~20ms+15ms gap) + 35ms = ~105ms
 *  => 5 sensors → ~275ms per cycle → ~3–4 samples/sec per sensor.
 *  => Over mqtt_interval=5s → ~15–20 samples averaged per MQTT publish.
 * ====================================================================== */
static void Task_ModbusSensorPoll(void *arg) {
    (void)arg;
    osDelay(2000); /* 2s boot delay: W5500 link negotiation + sensor power-up.
                    * One-time only. */
    
    printf("[Task] ModbusSensorPoll starting...\r\n");

    /* One-time driver init */
    Modbus_DMA_Init();
    osDelay(1000); /* 1s post-init: settle UART/MAX485 before first read. */

    printf("[Task] ModbusSensorPoll entering continuous poll loop\r\n");

    for (;;) {
        if (g_scan_status.is_scanning) {
            /* Drop to low priority during the one-shot network scan
             * so it doesn't starve real-time operations. */
            printf("[Task] Scan mode active: lowering priority to 1\r\n");
            vTaskPrioritySet(NULL, 1);
            Modbus_DMA_PerformScan();
            printf("[Task] Scan mode done: restoring priority to 3\r\n");
            vTaskPrioritySet(NULL, 3);
            osDelay(2000); /* 2s cool-down after a full 247-address scan. */
        } else {
            /* Poll sensors and yield 1000ms to reduce CPU load and keep the controller lightweight. */
            Modbus_DMA_PollSensors();
            osDelay(1000);
        }
    }
}

static uint8_t Parse_IP(const char *str, uint8_t *ip) {
    int parts[4];
    if (sscanf(str, "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]) == 4) {
        for (int i = 0; i < 4; i++) {
            if (parts[i] < 0 || parts[i] > 255) return 0;
            ip[i] = (uint8_t)parts[i];
        }
        return 1;
    }
    return 0;
}

static void Log_Mqtt_Topic(const char *topic, uint8_t success) {
    /* Shift log entries down */
    for (int i = MQTT_LOG_MAX - 1; i > 0; i--) {
        g_mqtt_status.log[i] = g_mqtt_status.log[i - 1];
    }
    /* Insert new entry at index 0 */
    strncpy((char *)g_mqtt_status.log[0].topic, topic, sizeof(g_mqtt_status.log[0].topic) - 1);
    g_mqtt_status.log[0].success = success;
    g_mqtt_status.log[0].timestamp = g_uptime_seconds;
    if (g_mqtt_status.log_count < MQTT_LOG_MAX) {
        g_mqtt_status.log_count++;
    }
}

static int Get_Sensor_Config_Index(const Gateway_Config_t *cfg, uint8_t type, uint8_t id) {
    for (int i = 0; i < cfg->sensors.count && i < MAX_SENSORS; i++) {
        if (cfg->sensors.entries[i].type == type && cfg->sensors.entries[i].id == id) {
            return i;
        }
    }
    return -1;
}

static void messageArrived(MessageData* data) {
    char topic[128];
    char payload[512];
    
    int t_len = (data->topicName->lenstring.len < sizeof(topic) - 1) ? data->topicName->lenstring.len : sizeof(topic) - 1;
    strncpy(topic, data->topicName->lenstring.data, t_len);
    topic[t_len] = '\0';
    
    int p_len = (data->message->payloadlen < sizeof(payload) - 1) ? data->message->payloadlen : sizeof(payload) - 1;
    strncpy(payload, data->message->payload, p_len);
    payload[p_len] = '\0';
    
    printf("[MQTT] Cmd received on %s: %s\r\n", topic, payload);

    /* Very simple JSON parsing for SET_ACTUATOR */
    if (strstr(payload, "\"SET_ACTUATOR\"") != NULL) {
        char *target_str = strstr(payload, "\"target\":\"");
        char *value_str = strstr(payload, "\"value\":\"");
        
        if (target_str && value_str) {
            target_str += 10; /* move past "target":" */
            value_str += 9;   /* move past "value":" */
            
            Gateway_Config_t live_cfg;
            Get_Shared_Config(&live_cfg);
            
            int relay_idx = -1;
            /* Look for relay_0, relay_1 etc */
            if (strncmp(target_str, "relay_", 6) == 0) {
                relay_idx = target_str[6] - '0';
            } else if (strncmp(target_str, "led", 3) == 0) {
                relay_idx = 0; /* Fallback for 'led' target */
            } else {
                /* Try to match relay name */
                for (int i=0; i<live_cfg.relay_count; i++) {
                    int name_len = strlen(live_cfg.relays[i].name);
                    if (name_len > 0 && strncmp(target_str, live_cfg.relays[i].name, name_len) == 0) {
                        relay_idx = i;
                        break;
                    }
                }
            }
            
            if (relay_idx >= 0 && relay_idx < live_cfg.relay_count) {
                if (strncmp(value_str, "ON", 2) == 0 || strncmp(value_str, "1", 1) == 0) {
                    Relay_SetState(relay_idx, 1);
                    printf("[MQTT] Actuator %d ON\r\n", relay_idx);
                } else if (strncmp(value_str, "OFF", 3) == 0 || strncmp(value_str, "0", 1) == 0) {
                    Relay_SetState(relay_idx, 0);
                    printf("[MQTT] Actuator %d OFF\r\n", relay_idx);
                }
            } else {
                printf("[MQTT] Unknown actuator target\r\n");
            }
        }
    }
}

static void Task_MQTTClient(void *arg) {
    (void)arg;
    
    Network n;
    MQTTClient client;
    static unsigned char tx_buf[2048];
    static unsigned char rx_buf[2048];

    /* CRITICAL: Assign W5500 socket 1 to MQTT.
     * Socket 0 is reserved exclusively for the HTTP server (HTTP_SOCK = 0).
     * Without this call, n.my_socket is uninitialized stack garbage, which
     * may randomly equal 0 and silently steal / corrupt the HTTP socket,
     * causing ERR_CONNECTION_REFUSED on port 80. */
    NewNetwork(&n, 1);
    
    /* Wait 5 seconds after boot to let W5500 acquire link/IP */
    osDelay(5000);
    
    printf("[MQTT] Client task starting on socket 1...\r\n");

    /* Track the broker+port we are currently connected to so we can detect
     * provisioning changes and reconnect automatically. */
    char  connected_broker[64] = {0};
    uint16_t connected_port   = 0;
    
    for (;;) {
        Gateway_Config_t cfg;
        Get_Shared_Config(&cfg);

        /* Default port to 1883 if unset */
        if (cfg.mqtt_port == 0) cfg.mqtt_port = 1883;
        
        /* Stop connecting/sending MQTT data unless provision status is "Active" */
        if (strcmp(cfg.provision_status, "Active") != 0) {
            g_mqtt_status.connected = 0;
            osDelay(2000);
            continue;
        }

        if (cfg.mqtt_broker[0] == '\0') {
            g_mqtt_status.connected = 0;
            osDelay(5000);
            continue;
        }
        
        uint8_t broker_ip[4];
        if (!Parse_IP(cfg.mqtt_broker, broker_ip)) {
            printf("[MQTT] Invalid broker IP: %s\r\n", cfg.mqtt_broker);
            g_mqtt_status.connected = 0;
            osDelay(5000);
            continue;
        }
        
        /* Set active topic: provisioned sparkplug_topic takes priority */
        char active_topic[128];
        if (strcmp(cfg.provision_status, "Active") == 0 && cfg.sparkplug_topic[0] != '\0') {
            strncpy(active_topic, cfg.sparkplug_topic, sizeof(active_topic) - 1);
            active_topic[sizeof(active_topic) - 1] = '\0';
        } else {
            snprintf(active_topic, sizeof(active_topic), "telemetry/kontrx-%07lu", (unsigned long)cfg.serial);
        }
        strncpy((char *)g_mqtt_status.active_topic, active_topic, sizeof(g_mqtt_status.active_topic) - 1);
        g_mqtt_status.active_topic[sizeof(g_mqtt_status.active_topic) - 1] = '\0';
        
        /* ── LWT (Last Will and Testament) ─────────────────────────────────
         * In Sparkplug B, the Node Death (NDEATH) payload is a binary protobuf message
         * configured as the LWT topic: spBv1.0/{group_id}/NDEATH/{node_id}
         * ───────────────────────────────────────────────────────────────── */
        char lwt_topic[128];
        get_sparkplug_topic(cfg.sparkplug_topic, "NDEATH", lwt_topic, sizeof(lwt_topic));
        
        static uint8_t lwt_payload[64];
        uint64_t initial_ts = (uint64_t)g_uptime_seconds * 1000;
        size_t lwt_len = sparkplug_encode_ndeath(lwt_payload, sizeof(lwt_payload), initial_ts, 0);

        MQTTPacket_connectData connect_data = MQTTPacket_connectData_initializer;
        connect_data.MQTTVersion = 3;
        connect_data.clientID.cstring = cfg.mqtt_client_id[0] ? cfg.mqtt_client_id : "kontrx-gateway";
        if (cfg.mqtt_username[0]) {
            connect_data.username.cstring = cfg.mqtt_username;
            connect_data.password.cstring = cfg.mqtt_password;
        }
        connect_data.keepAliveInterval = 60;
        connect_data.cleansession = 0; // Persistent session

        MQTTString lwt_topic_str = MQTTString_initializer;
        lwt_topic_str.cstring = lwt_topic;
        
        connect_data.willFlag = 1;
        connect_data.will.topicName = lwt_topic_str;
        connect_data.will.message.cstring = NULL;
        connect_data.will.message.lenstring.data = (char*)lwt_payload;
        connect_data.will.message.lenstring.len = lwt_len;
        connect_data.will.retained = 1;
        connect_data.will.qos = QOS1;

        /* Remember what broker we are about to connect to */
        strncpy(connected_broker, cfg.mqtt_broker, sizeof(connected_broker) - 1);
        connected_broker[sizeof(connected_broker) - 1] = '\0';
        connected_port = cfg.mqtt_port;
        
        printf("[MQTT] Connecting to broker %d.%d.%d.%d:%d...\r\n",
               broker_ip[0], broker_ip[1], broker_ip[2], broker_ip[3], cfg.mqtt_port);
        
        if (ConnectNetwork(&n, broker_ip, cfg.mqtt_port) == SOCK_OK) {
            printf("[MQTT] TCP Connected! Initializing MQTT client...\r\n");
            
            MQTTClientInit(&client, &n, 5000, tx_buf, sizeof(tx_buf), rx_buf, sizeof(rx_buf));
            
            int rc = MQTTConnect(&client, &connect_data);
            if (rc == SUCCESSS) {
                printf("[MQTT] Connected successfully!\r\n");
                g_mqtt_status.connected = 1;

                /* ── Send NBIRTH (Node Birth) ──────────────────────────────────
                 * Immediately after connecting, announce online state and all metric
                 * definitions/initial values as a binary protobuf NBIRTH payload.
                 * ───────────────────────────────────────────────────────────── */
                char nbirth_topic[128];
                get_sparkplug_topic(cfg.sparkplug_topic, "NBIRTH", nbirth_topic, sizeof(nbirth_topic));
                
                static uint8_t nbirth_buf[1024];
                TelemetryBatch_t nbirth_batch = {0};
                Modbus_DMA_ConsumeBatch(&nbirth_batch); // get current sensors
                
                uint64_t ts_ms = (uint64_t)g_uptime_seconds * 1000;
                size_t nbirth_len = sparkplug_encode_nbirth(nbirth_buf, sizeof(nbirth_buf), ts_ms, 0,
                                                            &nbirth_batch, &cfg, relayStates);
                if (nbirth_len > 0) {
                    MQTTMessage birth_msg;
                    birth_msg.qos        = QOS1;
                    birth_msg.retained   = 0;
                    birth_msg.dup        = 0;
                    birth_msg.payload    = (void*)nbirth_buf;
                    birth_msg.payloadlen = nbirth_len;
                    
                    printf("[MQTT] Publishing NBIRTH (%d bytes) to %s\r\n", (int)nbirth_len, nbirth_topic);
                    MQTTPublish(&client, nbirth_topic, &birth_msg);
                }
                
                /* Subscribe to ThingsBoard RPC and generic commands */
                MQTTSubscribe(&client, "v1/devices/me/rpc/request/+", QOS1, messageArrived);
                MQTTSubscribe(&client, "commands/#", QOS1, messageArrived);
                
                uint32_t last_publish = osKernelGetTickCount();
                
                while (client.isconnected) {
                    /* ── Detect any MQTT configuration changes (from provisioning or web UI) ── */
                    {
                        Gateway_Config_t live_cfg;
                        Get_Shared_Config(&live_cfg);
                        if (live_cfg.mqtt_port == 0) live_cfg.mqtt_port = 1883;
                        if (live_cfg.mqtt_interval == 0 || live_cfg.mqtt_interval > 86400) {
                            live_cfg.mqtt_interval = 5;
                        }

                        if (strncmp(live_cfg.mqtt_broker, connected_broker, sizeof(connected_broker)) != 0 ||
                            live_cfg.mqtt_port != connected_port ||
                            live_cfg.mqtt_interval != cfg.mqtt_interval ||
                            live_cfg.mqtt_send_mode != cfg.mqtt_send_mode ||
                            strcmp(live_cfg.mqtt_client_id, cfg.mqtt_client_id) != 0 ||
                            strcmp(live_cfg.mqtt_username, cfg.mqtt_username) != 0 ||
                            strcmp(live_cfg.mqtt_password, cfg.mqtt_password) != 0 ||
                            strcmp(live_cfg.sparkplug_topic, cfg.sparkplug_topic) != 0 ||
                            strcmp(live_cfg.provision_status, cfg.provision_status) != 0) {
                            
                            printf("[MQTT] Config/provision status changed — reconnecting/stopping...\r\n");
                            break; /* exit inner loop → reconnect or sleep with new settings */
                        }
                    }

                    /* Call MQTTYield to maintain keep-alives and process incoming */
                    int yield_rc = MQTTYield(&client, 10);
                    if (yield_rc != SUCCESSS) {
                        printf("[MQTT] MQTTYield error: %d\r\n", yield_rc);
                        break;
                    }
                    /* Send updates as soon as changes occur */
                    if (1) {
                        /* Refresh config in case interval changed */
                        Get_Shared_Config(&cfg);
                        if (cfg.mqtt_port == 0) cfg.mqtt_port = 1883;

                        /* Refresh active topic from latest config */
                        if (strcmp(cfg.provision_status, "Active") == 0 && cfg.sparkplug_topic[0] != '\0') {
                            strncpy(active_topic, cfg.sparkplug_topic, sizeof(active_topic) - 1);
                            active_topic[sizeof(active_topic) - 1] = '\0';
                        }
                        strncpy((char *)g_mqtt_status.active_topic, active_topic, sizeof(g_mqtt_status.active_topic) - 1);

                        /* ── State (persistent across publish cycles) ────────────── */
                        static float    cov_last_val[MAX_SENSORS]    = {0};
                        static float    cov_last_tmp[MAX_SENSORS]    = {0};
                        static uint8_t  cov_last_relay[MAX_RELAYS]   = {0};
                        static uint8_t  cov_ever_pub[MAX_SENSORS]    = {0};
                        static uint8_t  cov_above_db[MAX_SENSORS]    = {0}; /* hysteresis state */
                        static uint32_t cov_last_heartbeat_s         = 0;
                        static uint32_t cov_last_pub_tick            = 0;
                        static uint32_t cov_seq                      = 0;

                        TelemetryBatch_t batch = {0};
                        Modbus_DMA_ConsumeBatch(&batch);
                        
                        /* Check for relay changes before skipping on batch.count == 0 */
                        uint8_t relay_changed = 0;
                        for (int i = 0; i < cfg.relay_count && i < MAX_RELAYS; i++) {
                            if (relayStates[i] != cov_last_relay[i]) {
                                relay_changed = 1;
                                break;
                            }
                        }

                        /* ── Skip if no sensors and no relay changes ───────────────── */
                        if (batch.count == 0 && !relay_changed && (g_uptime_seconds - cov_last_heartbeat_s) < 300U) {
                            osDelay(20);
                            continue;
                        }

                        uint8_t has_change = 0;
                        uint8_t force_heartbeat = 0;
                        const char *trigger = "periodic";
                        
                        if (relay_changed) {
                            has_change = 1;
                            trigger = "relay";
                        }

                        if (cfg.mqtt_send_mode == 1) {
                            /* On Change / CoV mode: check if values changed or heartbeat expired */
                            #define COV_DB_PH      0.05f
                            #define COV_DB_ORP     5.0f
                            #define COV_DB_EC      10.0f
                            #define COV_DB_DO      0.1f
                            #define COV_DB_AMM     1.0f
                            #define COV_DB_US      5.0f
                            #define COV_DB_DEF     0.01f

                            #define COV_PCT_PH     0.5f
                            #define COV_PCT_ORP    0.5f
                            #define COV_PCT_EC     1.0f
                            #define COV_PCT_DO     1.0f
                            #define COV_PCT_AMM    1.0f
                            #define COV_PCT_US     0.5f

                            #define COV_RNG_PH     14.0f
                            #define COV_RNG_ORP    4000.0f
                            #define COV_RNG_EC     20000.0f
                            #define COV_RNG_DO     20.0f
                            #define COV_RNG_AMM    100.0f
                            #define COV_RNG_US     5000.0f

                            #define COV_HYST_FACTOR  0.5f
                            #define COV_HEARTBEAT_S  300U

                            /* Heartbeat logic */
                            if ((g_uptime_seconds - cov_last_heartbeat_s) >= COV_HEARTBEAT_S) {
                                force_heartbeat = 1;
                                cov_last_heartbeat_s = g_uptime_seconds;
                                has_change = 1;
                                trigger = "heartbeat";
                            }

                            if (!has_change) {
                                for (int i = 0; i < batch.count && i < MAX_SENSORS; i++) {
                                    if (!batch.records[i].valid) continue;
                                    int idx = Get_Sensor_Config_Index(&cfg, batch.records[i].type, batch.records[i].id);
                                    if (idx < 0) continue; /* skip unconfigured sensors */

                                    if (!cov_ever_pub[idx]) { has_change = 1; trigger = "birth"; break; }

                                    float db_abs, pct, range;
                                    switch (batch.records[i].type) {
                                        case 1: db_abs=COV_DB_PH;  pct=COV_PCT_PH;  range=COV_RNG_PH;  break;
                                        case 2: db_abs=COV_DB_ORP; pct=COV_PCT_ORP; range=COV_RNG_ORP; break;
                                        case 3: db_abs=COV_DB_EC;  pct=COV_PCT_EC;  range=COV_RNG_EC;  break;
                                        case 4: db_abs=COV_DB_DO;  pct=COV_PCT_DO;  range=COV_RNG_DO;  break;
                                        case 5: db_abs=COV_DB_AMM; pct=COV_PCT_AMM; range=COV_RNG_AMM; break;
                                        case 6:
                                        case 7: db_abs=COV_DB_US;  pct=COV_PCT_US;  range=COV_RNG_US;  break;
                                        default:db_abs=COV_DB_DEF; pct=1.0f;        range=100.0f;      break;
                                    }

                                    float db_pct = (pct / 100.0f) * range;
                                    float db = (db_abs < db_pct) ? db_abs : db_pct;
                                    float db_hyst = db * COV_HYST_FACTOR;

                                    float dv = batch.records[i].avg_value - cov_last_val[idx];
                                    if (dv < 0.0f) dv = -dv;

                                    if (!cov_above_db[idx] && dv >= db) {
                                        cov_above_db[idx] = 1;
                                        has_change = 1;
                                        trigger = "cov";
                                        break;
                                    } else if (cov_above_db[idx] && dv < db_hyst) {
                                        cov_above_db[idx] = 0;
                                    }

                                    float dt = batch.records[i].avg_temp - cov_last_tmp[idx];
                                    if (dt < 0.0f) dt = -dt;
                                    if (dt >= 0.5f) { has_change = 1; trigger = "cov_temp"; break; }
                                }
                            }

                            /* Rate limit removed per user request: system now inherently 
                             * bounded to 1Hz max via Modbus polling delay (1000ms) */

                            if (!has_change) {
                                osDelay(20);
                                continue;
                            }
                        }

                        /* ── 6. RTC Timestamp (seconds since epoch / uptime) ─────── */
                        char ts_str[20];
                        RTC_GetTimeString(ts_str, sizeof(ts_str));

                        /* Update baselines */
                        for (int i = 0; i < batch.count && i < MAX_SENSORS; i++) {
                            if (batch.records[i].valid) {
                                int idx = Get_Sensor_Config_Index(&cfg, batch.records[i].type, batch.records[i].id);
                                if (idx >= 0) {
                                    cov_last_val[idx] = batch.records[i].avg_value;
                                    cov_last_tmp[idx] = batch.records[i].avg_temp;
                                    cov_ever_pub[idx] = 1;
                                }
                            }
                        }
                        
                        /* Update relay baseline */
                        for (int i = 0; i < cfg.relay_count && i < MAX_RELAYS; i++) {
                            cov_last_relay[i] = relayStates[i];
                        }

                        /* Build binary Sparkplug B DDATA payload */
                        static uint8_t ddata_buf[1024];
                        uint64_t ts_ms = (uint64_t)g_uptime_seconds * 1000;
                        
                        /* Increment Sparkplug sequence */
                        cov_seq = (cov_seq + 1) & 0xFF;
                        if (cov_seq == 0) cov_seq = 1;

                        size_t ddata_len = sparkplug_encode_ddata(ddata_buf, sizeof(ddata_buf), ts_ms, cov_seq,
                                                                  &batch, relayStates);
                                                                  
                        char ddata_topic[128];
                        get_sparkplug_topic(cfg.sparkplug_topic, "DDATA", ddata_topic, sizeof(ddata_topic));

                        /* ── 5+7. QoS1 + Retained publish ───────────────────────── */
                        MQTTMessage message;
                        message.qos        = QOS1;  /* PUBACK required — no silent loss */
                        message.retained   = 1;     /* new subscribers get last value   */
                        message.dup        = 0;
                        message.payload    = (void*)ddata_buf;
                        message.payloadlen = ddata_len;

                        printf("[MQTT] Pub DDATA(%s) seq=%lu size=%d: %s\r\n",
                               trigger, (unsigned long)cov_seq, (int)ddata_len, ts_str);

                        int pub_rc = MQTTPublish(&client, ddata_topic, &message);
                        if (pub_rc == SUCCESSS) {
                            cov_last_pub_tick = osKernelGetTickCount(); /* update rate-limit gate */
                            Log_Mqtt_Topic(ddata_topic, 1);
                        } else {
                            printf("[MQTT] Publish failed: %d\r\n", pub_rc);
                            Log_Mqtt_Topic(ddata_topic, 0);
                            break;
                        }
                    }

                    osDelay(20);

                }
            } else {
                printf("[MQTT] Connect failed, rc = %d\r\n", rc);
            }
            g_mqtt_status.connected = 0;
            disconnect(1);
            close(1);
        } else {
            printf("[MQTT] TCP Connection failed.\r\n");
            close(1); /* Ensure socket is closed/released on connection failure */
        }
        
        osDelay(5000); /* Delay before retry connection */
    }
}

osThreadId_t g_tid_modbus = NULL;
osThreadId_t g_tid_mqtt   = NULL;

/* ======================================================================
 *  RTOS_Tasks_Init — Creates all tasks and starts the scheduler
 *  Called from main().  Does NOT return.
 * ====================================================================== */
void RTOS_Tasks_Init(void) {
    osThreadAttr_t attr = {0};

    /* Task 1 — Control Engine */
    attr.name       = "ControlEng";
    attr.stack_size = 2048; // زدناها من 512 إلى 2048 لضمان الاستقرار
    attr.priority   = osPriorityRealtime;
    osThreadNew(Task_ControlEngine, NULL, &attr);

    /* Task 2 — Modbus Sensor Poll */
    attr.name       = "ModbusPoll";
    attr.stack_size = 4096; 
    attr.priority   = osPriorityAboveNormal;
    g_tid_modbus    = osThreadNew(Task_ModbusSensorPoll, NULL, &attr);

    /* Task 3 — HTTP Server */
    attr.name       = "HTTPServer";
    attr.stack_size = 8192;  /* Increased from 4096: Gateway_Config_t is ~1KB,
                              * up to 10 endpoint handlers allocate local copies,
                              * plus snprintf frames → must be 8KB minimum. */
    attr.priority   = osPriorityNormal;
    osThreadNew(Task_HTTPServer, NULL, &attr);

    /* Task 4 — OTA Background */
    attr.name       = "OTAUpdate";
    attr.stack_size = 2048; // زدناها من 1024 إلى 2048
    attr.priority   = osPriorityLow;
    osThreadNew(Task_OTAUpdate, NULL, &attr);

    /* Task 5 — MQTT Client */
    attr.name       = "MQTTClient";
    attr.stack_size = 8192;  /* Increased from 4096: MQTTClient+Network structs +
                              * LWT payload + sparkplug encode buffers + tx/rx[512]
                              * + deep MQTTConnect/MQTTPublish call frames. */
    attr.priority   = osPriorityNormal;
    g_tid_mqtt      = osThreadNew(Task_MQTTClient, NULL, &attr);

    osKernelStart();
}
