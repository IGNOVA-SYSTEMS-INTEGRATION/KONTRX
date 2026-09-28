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

#include <stdlib.h>
#include "freertos_tasks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "modbus_dma.h"
#include "flash_partition.h"
#include "http_server_task.h"
#include "ota_task.h"
#include "rtc_stm32.h"
#include "MQTTClient.h"
#include "mqtt_interface.h"
#include "sparkplug_b_enc.h"
#include "modbus_tcp_server.h"
#include "interface_discovery.h"
#include "pwm_controller.h"
#include "pto_motion.h"
#include "dac_420ma.h"
#include "analog_010v.h"

static Gateway_Config_t s_tasks_cfg;  /* Shared config scratch for MQTT/Relay tasks */
#include "socket.h"
#include "led.h"
#include "dns.h"
#include "wizchip_conf.h"
#include "cJSON.h"

RuleConfig_t activeRules;
RuleConfig_t pendingRules;
volatile uint8_t hasPendingRules = 0;
osMutexId_t rulesMutex = NULL;
volatile uint32_t rulesTestTicks = 0;
volatile uint8_t rulesTesting = 0;


/* Sparkplug B Topic Helper
 * - If base_topic contains "/DDATA/", treat it as a Sparkplug B template and
 *   replace DDATA with msg_type (NBIRTH, NDEATH, etc.).
 * - If base_topic is a plain custom topic (e.g. "test/topic/12345"), use it
 *   as-is for DDATA publishes. For NBIRTH/NDEATH, append "/msg_type".
 * - If base_topic is empty, fall back to standard Sparkplug B format.
 */
static void get_sparkplug_topic(const char *base_topic, const char *msg_type, char *out_topic, size_t max_len) {
    /* Empty topic → standard Sparkplug B fallback */
    if (base_topic[0] == '\0') {
        snprintf(out_topic, max_len, "spBv1.0/KontrxGroup/%s/kontrx-%07lu", msg_type, (unsigned long)s_tasks_cfg.serial);
        return;
    }

    /* Sparkplug B template: contains "/DDATA/" → replace message type */
    char *ddata_ptr_check = strstr(base_topic, "/DDATA/");
    if (ddata_ptr_check != NULL) {
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

    /* Plain custom topic:
     * - DDATA -> use base_topic as-is
     * - NBIRTH/NDEATH -> append "/msg_type" (e.g. topic/NBIRTH, topic/NDEATH)
     */
    if (strcmp(msg_type, "DDATA") == 0) {
        strncpy(out_topic, base_topic, max_len - 1);
        out_topic[max_len - 1] = '\0';
    } else {
        snprintf(out_topic, max_len, "%s/%s", base_topic, msg_type);
    }
}
#include "stm32f407_regs.h"
#include "gpio_stm32.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "portable.h"
#include <stdio.h>

volatile MqttStatus_t g_mqtt_status __attribute__((section(".ccmram"))) = {0};

uint8_t g_log_ring[LOG_BUFFER_SIZE] = {0};
volatile uint32_t g_log_head = 0;
volatile uint32_t g_log_tail = 0;
volatile uint32_t g_log_used_bytes = 0;
volatile uint32_t g_total_logs_written = 0;
volatile uint32_t g_sys_log_count = 0;
volatile uint8_t  g_sys_log_full = 0;

#include "sdcard.h"

static void Do_Offline_Telemetry_Queueing(void);

void Log_Event(const char *category, const char *message) {
    if (!category || !message) return;

    uint8_t cat_id = 0;
    if (strcmp(category, "MQTT") == 0) cat_id = 1;
    else if (strcmp(category, "MODBUS") == 0) cat_id = 2;
    else if (strcmp(category, "RELAY") == 0) cat_id = 3;
    else if (strcmp(category, "OTA") == 0) cat_id = 4;
    else if (strcmp(category, "SD") == 0) cat_id = 5;
    else if (strcmp(category, "AUTH") == 0) cat_id = 6;
    else cat_id = 0; // SYS

    uint32_t ts = RTC_GetUptimeSeconds();
    if (ts == 0) ts = g_uptime_seconds;
    SDCard_Log_Append(ts, cat_id, message);
}

/* ======================================================================
 *  Global uptime counter
 * ====================================================================== */
volatile uint32_t g_uptime_seconds = 0;
volatile uint8_t g_ota_in_progress = 0;

/* CPU usage (0-100%) — updated once per second by tick hook */
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000U)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004U)
#define DEM_CR     (*(volatile uint32_t *)0xE000EDFCU)
#define DEM_CR_TRCENA (1UL << 24)

volatile uint8_t g_cpu_usage_pct = 0;

static volatile uint32_t g_idle_task_start_time = 0;
static volatile uint32_t g_total_idle_cycles = 0;

void KontrxTaskSwitchedIn(void *tcb) {
    if (tcb == xTaskGetIdleTaskHandle()) {
        g_idle_task_start_time = DWT_CYCCNT;
    }
}

void KontrxTaskSwitchedOut(void *tcb) {
    if (tcb == xTaskGetIdleTaskHandle()) {
        if (g_idle_task_start_time != 0) {
            g_total_idle_cycles += (DWT_CYCCNT - g_idle_task_start_time);
            g_idle_task_start_time = 0;
        }
    }
}

void Update_CPU_Usage(void) {
    static uint32_t last_cyccnt = 0;
    static uint32_t last_idle_cycles = 0;

    uint32_t now_cyc = DWT_CYCCNT;
    uint32_t now_idle = g_total_idle_cycles;

    /* If the idle task is currently running, add its accumulated cycles so far */
    if (xTaskGetCurrentTaskHandle() == xTaskGetIdleTaskHandle()) {
        if (g_idle_task_start_time != 0) {
            now_idle += (now_cyc - g_idle_task_start_time);
        }
    }

    uint32_t total_cycles = now_cyc - last_cyccnt;
    uint32_t idle_cycles = now_idle - last_idle_cycles;

    if (total_cycles > 0) {
        uint32_t idle_pct = (uint32_t)(((uint64_t)idle_cycles * 100ULL) / total_cycles);
        if (idle_pct > 100U) idle_pct = 100U;
        g_cpu_usage_pct = (uint8_t)(100U - idle_pct);
    }

    last_cyccnt = now_cyc;
    last_idle_cycles = now_idle;
}

/* FreeRTOS tick hook — increments uptime every second */
static volatile uint32_t g_tick_counter = 0;

void vApplicationTickHook(void) {
    /* Tick the MQTT MilliTimer every 1ms */
    extern void MilliTimer_Handler(void);
    MilliTimer_Handler();

    g_tick_counter++;
    if (g_tick_counter >= (uint32_t)configTICK_RATE_HZ) {
        g_tick_counter = 0;
        g_uptime_seconds++;
        Update_CPU_Usage();
        
        /* Tick the DNS time handler */
        extern void DNS_time_handler(void);
        DNS_time_handler();
    }
}

/* ======================================================================
 *  CPU Usage via Cortex-M DWT cycle counter
 * ====================================================================== */
void KontrxDWT_Init(void) {
    DEM_CR   |= DEM_CR_TRCENA; /* enable trace subsystem */
    /* Unlock DWT LAR (Lock Access Register) to allow write access to DWT registers */
    volatile uint32_t *dwt_lar = (volatile uint32_t *)0xE0001FB0U;
    *dwt_lar = 0xC5ACCE55U;
    DWT_CYCCNT = 0;
    DWT_CTRL  |= 1UL;           /* enable cycle counter */
}

void vApplicationIdleHook(void) {
    /* Kept active for FreeRTOS idle loop */
}




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
static int Get_Port_Id(const char *port_str) {
    return GPIO_GetPortId(port_str);
}

void Relay_Init(void) {
    Get_Shared_Config(&s_tasks_cfg);

    for (int i = 0; i < (int)s_tasks_cfg.actuator_count && i < MAX_RELAYS; i++) {
        if (s_tasks_cfg.actuators[i].type != ACTUATOR_TYPE_LOCAL_GPIO &&
            s_tasks_cfg.actuators[i].type != ACTUATOR_TYPE_DIGITAL_OUT) {
            continue; // Skip non-local actuators in physical pin init
        }

        int port = Get_Port_Id(s_tasks_cfg.actuators[i].port_or_ip);
        uint8_t pin = s_tasks_cfg.actuators[i].pin_or_slave;

        if (port < 0 || port > 4) continue;
        if (Relay_Pin_Is_Reserved(port, pin)) {
            printf("[Relay] %s pin P%c%u is reserved; relay disabled\r\n",
                   s_tasks_cfg.actuators[i].name[0] ? s_tasks_cfg.actuators[i].name : "Relay",
                   'A' + port, pin);
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

void Relay_SetState(uint8_t idx, uint8_t state) {
    if (idx >= MAX_RELAYS) return;
    
    // Safety guard: return immediately if state is already in target state.
    // This prevents endless blocking SPI flash writes inside the Control Engine loop.
    if (relayStates[idx] == state) {
        return;
    }

    Get_Shared_Config(&s_tasks_cfg);

    if (idx >= s_tasks_cfg.actuator_count) return;

    if (s_tasks_cfg.actuators[idx].type == ACTUATOR_TYPE_LOCAL_GPIO ||
        s_tasks_cfg.actuators[idx].type == ACTUATOR_TYPE_DIGITAL_OUT) {
        int port = Get_Port_Id(s_tasks_cfg.actuators[idx].port_or_ip);
        uint8_t pin = s_tasks_cfg.actuators[idx].pin_or_slave;
        uint8_t is_nc = s_tasks_cfg.actuators[idx].is_active_low;

        if (port < 0 || port > 4) return;
        if (Relay_Pin_Is_Reserved(port, pin)) return;

        GPIO_TypeDef *gpio = GPIO_Ports[port];
        uint8_t drive = (is_nc ? !state : state);

        if (drive) {
            gpio->BSRR = (1U << pin);            /* Set pin HIGH */
        } else {
            gpio->BSRR = (1U << (pin + 16));     /* Set pin LOW */
        }
    } else if (s_tasks_cfg.actuators[idx].type == ACTUATOR_TYPE_MODBUS_TCP) {
        extern uint8_t Modbus_TCP_WriteCoil(const char *ip, uint16_t port, uint8_t slave_id, uint16_t coil_addr, uint8_t state);
        Modbus_TCP_WriteCoil(s_tasks_cfg.actuators[idx].port_or_ip,
                             s_tasks_cfg.actuators[idx].port,
                             s_tasks_cfg.actuators[idx].pin_or_slave,
                             s_tasks_cfg.actuators[idx].reg_addr,
                             state);
    } else if (s_tasks_cfg.actuators[idx].type == ACTUATOR_TYPE_OPC_UA_CLIENT) {
        extern uint8_t OPC_UA_Client_WriteNode(const char *endpoint, uint16_t ns, const char *node_id, uint8_t state);
        OPC_UA_Client_WriteNode(s_tasks_cfg.actuators[idx].port_or_ip,
                                s_tasks_cfg.actuators[idx].port,
                                s_tasks_cfg.actuators[idx].opc_node_id,
                                state);
    }

    uint8_t old_state = relayStates[idx];
    relayStates[idx] = state;

    if (old_state != state) {
        char r_log[64];
        const char *act_name = s_tasks_cfg.actuators[idx].name[0] ? s_tasks_cfg.actuators[idx].name : "Unnamed";
        snprintf(r_log, sizeof(r_log), "Actuator %d (%s) set to %s.",
                 idx + 1, act_name,
                 state ? "ON" : "OFF");
        printf("[RELAY] State Change: Actuator %d (%s) set to %s\r\n", idx + 1, act_name, state ? "ON" : "OFF");
        Log_Event("RELAY", r_log);

        /* Cache relay event to offline queue when MQTT broker is disconnected */
        if (s_tasks_cfg.mqtt_broker[0] != '\0' && !g_mqtt_status.connected) {
            OfflineRecord_t evt_rec;
            evt_rec.timestamp = g_uptime_seconds;
            evt_rec.source_type = MAP_SOURCE_ACTUATOR;
            evt_rec.source_id = idx;
            evt_rec.valid = 1;
            evt_rec.value = (float)state;
            evt_rec.temp = 0.0f;
            SDCard_Queue_Push(&evt_rec);
        }
    }

    s_tasks_cfg.actuators[idx].state = state;
    Update_Shared_Config(&s_tasks_cfg);
}

extern osMutexId_t configMutex;

static int string_equals_ci(const char *s1, const char *s2) {
    if (!s1 || !s2) return 0;
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
        s1++;
        s2++;
    }
    return *s1 == *s2;
}

static float Resolve_Input_Value(const char *input_id, const Gateway_Config_t *cfg, const Modbus_SensorData_t *sd, uint8_t *found) {
    *found = 0;
    
    // 1. Try to find mapping in mqtt_mappings by matching json_key
    for (int i = 0; i < cfg->mqtt_mapping_count; i++) {
        if (cfg->mqtt_mappings[i].enabled && strcmp(cfg->mqtt_mappings[i].json_key, input_id) == 0) {
            uint8_t sid = cfg->mqtt_mappings[i].source_id;
            // Find this sensor in readings
            for (int j = 0; j < sd->readings_count && j < MAX_SENSORS; j++) {
                if (sd->readings[j].id == sid && (activeRules.bypass_validation || !sd->readings[j].stale)) {
                    *found = 1;
                    if (strstr(input_id, "temp") != NULL || strstr(input_id, "Temp") != NULL) {
                        return sd->readings[j].temp;
                    }
                    return sd->readings[j].value;
                }
            }
        }
    }
    
    // 2. Try to match standard type names
    uint8_t target_type = 0;
    uint8_t is_temp = 0;
    if (string_equals_ci(input_id, "ph")) target_type = 1;
    else if (string_equals_ci(input_id, "orp")) target_type = 2;
    else if (string_equals_ci(input_id, "ec")) target_type = 3;
    else if (string_equals_ci(input_id, "do")) target_type = 4;
    else if (string_equals_ci(input_id, "ammonia")) target_type = 5;
    else if (string_equals_ci(input_id, "ultasonic") || string_equals_ci(input_id, "ultrasonic")) target_type = 6;
    else if (string_equals_ci(input_id, "multi_us")) target_type = 7;
    else if (strstr(input_id, "temp") != NULL || strstr(input_id, "Temp") != NULL) {
        is_temp = 1;
    }
    
    if (target_type > 0) {
        for (int j = 0; j < sd->readings_count && j < MAX_SENSORS; j++) {
            if (sd->readings[j].type == target_type && (activeRules.bypass_validation || !sd->readings[j].stale)) {
                *found = 1;
                return sd->readings[j].value;
            }
        }
    } else if (is_temp) {
        // Return temperature from first valid sensor
        for (int j = 0; j < sd->readings_count && j < MAX_SENSORS; j++) {
            if (activeRules.bypass_validation || !sd->readings[j].stale) {
                *found = 1;
                return sd->readings[j].temp;
            }
        }
    }
    
    // 3. Extract numeric sensor ID from string (e.g. "1", "Sensor (ID 1)", "sensor_1")
    int target_id = -1;
    const char *p = input_id;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            target_id = (int)strtol(p, NULL, 10);
            break;
        }
        p++;
    }
    
    if (target_id >= 0) {
        for (int j = 0; j < sd->readings_count && j < MAX_SENSORS; j++) {
            if (sd->readings[j].id == (uint8_t)target_id && (activeRules.bypass_validation || !sd->readings[j].stale)) {
                *found = 1;
                return sd->readings[j].value;
            }
        }
    }
    
    // 4. Fallback when bypass_validation is active: allow testing rules when sensors are offline
    if (activeRules.bypass_validation) {
        *found = 1;
        return 0.0f;
    }
    
    return -9999.0f;
}

static int8_t Resolve_Output_Id(const char *output_id, const Gateway_Config_t *cfg) {
    if (!output_id || output_id[0] == '\0') return -1;

    // 1. Try exact match on actuator name
    for (int i = 0; i < cfg->actuator_count && i < MAX_RELAYS; i++) {
        if (strcmp(cfg->actuators[i].name, output_id) == 0) {
            return i;
        }
    }
    
    // 2. Try match on "(ID X)" or "ID X" in string (e.g. "Relay3 (ID 2)" or "Pump (ID 0)")
    const char *id_pos = strstr(output_id, "ID ");
    if (!id_pos) id_pos = strstr(output_id, "id ");
    if (!id_pos) id_pos = strstr(output_id, "ID_");
    if (!id_pos) id_pos = strstr(output_id, "id_");
    if (id_pos) {
        id_pos += 3;
        while (*id_pos == ' ' || *id_pos == '_') id_pos++;
        long val = strtol(id_pos, NULL, 10);
        if (val >= 0 && val < MAX_RELAYS) {
            return (int8_t)val;
        }
    }
    
    // 3. Match "relay_X" or "relayX" or "Relay X"
    if (strncasecmp(output_id, "relay", 5) == 0) {
        const char *p = output_id + 5;
        while (*p == ' ' || *p == '_') p++;
        if (*p >= '0' && *p <= '9') {
            long val = strtol(p, NULL, 10);
            if (val >= 0 && val < MAX_RELAYS) {
                return (int8_t)val;
            }
        }
    }

    // 4. Extract digit from output_id string as fallback
    const char *p = output_id;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            long val = strtol(p, NULL, 10);
            if (val >= 0 && val < MAX_RELAYS) {
                return (int8_t)val;
            }
            break;
        }
        p++;
    }
    
    return -1;
}

/* ======================================================================
 *  TASK 1: High-Speed Control Engine — Priority: Real-Time (5)
 * ====================================================================== */
static void Task_ControlEngine(void *arg) {
    (void)arg;

    Modbus_SensorData_t sd = { .last_update_time = 0 };

    for (;;) {
        if (g_ota_in_progress) {
            osDelay(50);
            continue;
        }

        /* Poll Modbus TCP Server periodically (50ms) so it does not hog spiMutex and starve HTTP server */
        static uint32_t last_mb_tcp_poll = 0;
        uint32_t now_tick = osKernelGetTickCount();
        if (now_tick - last_mb_tcp_poll >= 50) {
            const Hardware_Interface_Desc_t *if10 = Interface_GetDesc(10);
            if (!if10 || if10->enabled) {
                Modbus_TCP_Server_Poll();
            }
            last_mb_tcp_poll = now_tick;
        }

        /* --- Read latest sensor snapshot (mutex-protected, non-blocking) --- */
        if (osMutexAcquire(sensorMutex, 0) == osOK) {
            sd = sharedSensorData;
            osMutexRelease(sensorMutex);
        }

        if (sd.last_update_time == 0 && !activeRules.bypass_validation) {
            osDelay(25);
            continue;
        }
        /* If mutex was unavailable, we proceed with the last known snapshot — 
         * this is intentional; the control engine never stalls for sensor data. */

/* ================================================================
         * --- CONTROL LOGIC ZONE (user-defined) ---
         * ================================================================ */
        
        /* 1. Stability watchdog for newly updated rules */
        if (rulesTesting) {
            uint32_t now = osKernelGetTickCount();
            if ((int32_t)(now - rulesTestTicks) >= 0) {
                if (osMutexAcquire(rulesMutex, 0) == osOK) {
                    rulesTesting = 0;
                    activeRules.rules_valid = 1;
                    Partition_SaveRules(&activeRules);
                    char stable_log[64];
                    snprintf(stable_log, sizeof(stable_log), "Rules version %s verified stable.", activeRules.version_id);
                    Log_Event("SYS", stable_log);
                    osMutexRelease(rulesMutex);
                }
            }
        }

        /* 2. Execute active rules */
        static RuleConfig_t localRules;
        uint8_t has_rules = 0;
        if (osMutexAcquire(rulesMutex, 0) == osOK) {
            localRules = activeRules;
            has_rules = (localRules.rule_count > 0);
            osMutexRelease(rulesMutex);
        }

        if (has_rules) {
            // Non-blocking snapshot of current config to map names
            static Gateway_Config_t cfg_snap;
            static uint32_t local_config_version = 0;
            if (local_config_version != g_config_version) {
                if (configMutex && osMutexAcquire(configMutex, 0) == osOK) {
                    memcpy(&cfg_snap, &sharedConfig, sizeof(Gateway_Config_t));
                    local_config_version = g_config_version;
                    osMutexRelease(configMutex);
                }
            }

            /* If Test Mode is active, bypass automated rules to allow user manual test/override */
            if (cfg_snap.test_mode == 1) {
                /* Test mode: rules paused */
            } else {
                for (uint32_t i = 0; i < localRules.rule_count; i++) {
                    const Rule_t *rule = &localRules.rules[i];
                    if (!rule->active) continue;

                    // Resolve input value from string input_id
                    uint8_t found = 0;
                    float val = Resolve_Input_Value(rule->input_id, &cfg_snap, &sd, &found);

                    if (found) {
                        uint8_t cond_met = 0;
                        float thr = rule->threshold;
                        if (localRules.bypass_validation) {
                            /* When sensor validation bypass is active, rule fires to test actuators */
                            cond_met = 1;
                        } else {
                            if (strcmp(rule->operator, ">") == 0)       cond_met = (val > thr);
                            else if (strcmp(rule->operator, "<") == 0)  cond_met = (val < thr);
                            else if (strcmp(rule->operator, "==") == 0) cond_met = (val == thr);
                            else if (strcmp(rule->operator, ">=") == 0) cond_met = (val >= thr);
                            else if (strcmp(rule->operator, "<=") == 0) cond_met = (val <= thr);
                        }

                        if (cond_met) {
                            int8_t relay_idx = Resolve_Output_Id(rule->output_id, &cfg_snap);
                            if (relay_idx >= 0 && relay_idx < MAX_RELAYS) {
                                uint8_t action_val = 0;
                                if (string_equals_ci(rule->action, "ON") || strcmp(rule->action, "1") == 0) {
                                    action_val = 1;
                                }
                                Relay_SetState(relay_idx, action_val);
                            }
                        }
                    }
                }
            }
        }
        /* ================================================================ */

        /* Optimized 25ms delay — 40Hz is optimal for industrial PLC control rules */
        osDelay(25);
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
        if (g_ota_in_progress) {
            osDelay(50);
            continue;
        }

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
            /* Poll sensors if RS485 Bus 1 (index 3) is enabled; yield 350ms between cycles */
            const Hardware_Interface_Desc_t *if3 = Interface_GetDesc(3);
            if (!if3 || if3->enabled) {
                Modbus_DMA_PollSensors();
            }
            osDelay(350);
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

static void Log_Mqtt_Topic(const char *topic, const char *payload, uint8_t success) {
    uint8_t locked = 0;
    if (sensorMutex && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        if (osMutexAcquire(sensorMutex, 10) == osOK) {
            locked = 1;
        }
    }
    /* Shift log entries down */
    for (int i = MQTT_LOG_MAX - 1; i > 0; i--) {
        g_mqtt_status.log[i] = g_mqtt_status.log[i - 1];
    }
    /* Insert new entry at index 0 */
    memset((void *)&g_mqtt_status.log[0], 0, sizeof(g_mqtt_status.log[0]));
    strncpy((char *)g_mqtt_status.log[0].topic, topic, sizeof(g_mqtt_status.log[0].topic) - 1);
    g_mqtt_status.log[0].topic[sizeof(g_mqtt_status.log[0].topic) - 1] = '\0';

    if (payload) {
        strncpy((char *)g_mqtt_status.log[0].payload, payload, sizeof(g_mqtt_status.log[0].payload) - 1);
        g_mqtt_status.log[0].payload[sizeof(g_mqtt_status.log[0].payload) - 1] = '\0';
    } else {
        g_mqtt_status.log[0].payload[0] = '\0';
    }

    g_mqtt_status.log[0].success = success;
    g_mqtt_status.log[0].timestamp = g_uptime_seconds;
    if (g_mqtt_status.log_count < MQTT_LOG_MAX) {
        g_mqtt_status.log_count++;
    }
    if (locked) {
        osMutexRelease(sensorMutex);
    }
}

static void Build_Flat_Telemetry_JSON(char *json_buf, size_t max_len, const TelemetryBatch_t *batch, const Gateway_Config_t *cfg) {
    char ts_str[32];
    RTC_GetTimeString(ts_str, sizeof(ts_str));

    int pos = 0;
    pos += snprintf(json_buf + pos, max_len - pos, "{\"timestamp\":\"%s\"", ts_str);

    if (cfg->mqtt_mapping_count > 0) {
        for (int m = 0; m < cfg->mqtt_mapping_count; m++) {
            if (!cfg->mqtt_mappings[m].enabled) continue;
            if (cfg->mqtt_mappings[m].source_type == MAP_SOURCE_SENSOR) {
                float val = 0.0f;
                uint8_t sensor_valid = 0;
                for (int s = 0; s < batch->count; s++) {
                    if (batch->records[s].id == cfg->mqtt_mappings[m].source_id && batch->records[s].valid) {
                        val = batch->records[s].avg_value;
                        sensor_valid = 1;
                        break;
                    }
                }
                /* Only include key if sensor is online and valid */
                if (sensor_valid) {
                    pos += snprintf(json_buf + pos, max_len - pos, ",\"%s\":%.2f", cfg->mqtt_mappings[m].json_key, val);
                }
            } else if (cfg->mqtt_mappings[m].source_type == MAP_SOURCE_ACTUATOR) {
                uint8_t r_id = cfg->mqtt_mappings[m].source_id;
                uint8_t st = (r_id < MAX_RELAYS) ? relayStates[r_id] : 0;
                pos += snprintf(json_buf + pos, max_len - pos, ",\"%s\":%u", cfg->mqtt_mappings[m].json_key, st);
            }
        }
    } else {
        /* Dynamic key-value mapping: ONLY emit sensors that are currently online and valid */
        for (int s = 0; s < batch->count; s++) {
            if (!batch->records[s].valid) continue;
            const char *tname = SensorTypeName(batch->records[s].type);
            pos += snprintf(json_buf + pos, max_len - pos, ",\"%s\":%.2f", tname, batch->records[s].avg_value);
            if (batch->records[s].avg_temp > -50.0f && batch->records[s].avg_temp < 150.0f) {
                pos += snprintf(json_buf + pos, max_len - pos, ",\"%s_temp\":%.2f", tname, batch->records[s].avg_temp);
            }
        }
        
        uint8_t relay_count = (cfg->actuator_count > 0) ? cfg->actuator_count : 2;
        for (int r = 0; r < relay_count && r < MAX_RELAYS; r++) {
            pos += snprintf(json_buf + pos, max_len - pos, ",\"relay_%d\":%u", r + 1, relayStates[r]);
        }
    }

    pos += snprintf(json_buf + pos, max_len - pos, "}");
}

static void Build_Structured_Telemetry_JSON(char *json_buf, size_t max_len, const TelemetryBatch_t *batch, const Gateway_Config_t *cfg) {
    char dev_id[40];
    if (cfg->device_id[0] != '\0') {
        strncpy(dev_id, cfg->device_id, sizeof(dev_id) - 1);
        dev_id[sizeof(dev_id) - 1] = '\0';
    } else if (cfg->mqtt_client_id[0] != '\0') {
        strncpy(dev_id, cfg->mqtt_client_id, sizeof(dev_id) - 1);
        dev_id[sizeof(dev_id) - 1] = '\0';
    } else {
        snprintf(dev_id, sizeof(dev_id), "kontrx-%07lu", (unsigned long)cfg->serial);
    }

    char ts_str[32];
    RTC_GetTimeString(ts_str, sizeof(ts_str));

    int pos = 0;
    pos += snprintf(json_buf + pos, max_len - pos,
                    "{\"deviceId\":\"%s\","
                    "\"timestamp\":\"%s\","
                    "\"status\":\"Online\","
                    "\"test_mode\":%u,"
                    "\"sensors\":{",
                    dev_id, ts_str, cfg->test_mode);

    uint8_t sens_emitted = 0;
    uint8_t total_sensors = (cfg->sensors.count > 0 && cfg->sensors.count <= MAX_SENSORS) ? cfg->sensors.count : 0;

    for (uint8_t i = 0; i < total_sensors; i++) {
        uint8_t stype = cfg->sensors.entries[i].type;
        uint8_t sid   = cfg->sensors.entries[i].id;
        const char *sname = SensorTypeName(stype);

        int b_idx = -1;
        for (int b = 0; b < batch->count; b++) {
            if (batch->records[b].type == stype && batch->records[b].id == sid) {
                b_idx = b;
                break;
            }
        }

        if (b_idx >= 0 && batch->records[b_idx].valid) {
            pos += snprintf(json_buf + pos, max_len - pos,
                            "%s\"%s_%u\":{\"value\":%.2f,\"temp\":%.2f,\"status\":\"online\"}",
                            sens_emitted ? "," : "",
                            sname, sid,
                            batch->records[b_idx].avg_value, batch->records[b_idx].avg_temp);
            sens_emitted = 1;
        } else if (!cfg->mqtt_skip_offline) {
            pos += snprintf(json_buf + pos, max_len - pos,
                            "%s\"%s_%u\":{\"value\":0.00,\"temp\":0.00,\"status\":\"offline\"}",
                            sens_emitted ? "," : "",
                            sname, sid);
            sens_emitted = 1;
        }
    }

    if (!sens_emitted && batch->count > 0) {
        for (int s = 0; s < batch->count; s++) {
            if (cfg->mqtt_skip_offline && !batch->records[s].valid) continue;
            const char *sname = SensorTypeName(batch->records[s].type);
            pos += snprintf(json_buf + pos, max_len - pos,
                            "%s\"%s_%u\":{\"value\":%.2f,\"temp\":%.2f,\"status\":\"%s\"}",
                            sens_emitted ? "," : "",
                            sname, batch->records[s].id,
                            batch->records[s].avg_value, batch->records[s].avg_temp,
                            batch->records[s].valid ? "online" : "offline");
            sens_emitted = 1;
        }
    }

    pos += snprintf(json_buf + pos, max_len - pos, "},\"actuators\":{");

    uint8_t act_emitted = 0;
    uint8_t act_cnt = (cfg->actuator_count > 0) ? cfg->actuator_count : 2;
    if (act_cnt > MAX_RELAYS) act_cnt = MAX_RELAYS;

    for (int i = 0; i < act_cnt; i++) {
        char aname[32];
        if (cfg->actuators[i].name[0] != '\0') {
            snprintf(aname, sizeof(aname), "%s", cfg->actuators[i].name);
        } else {
            snprintf(aname, sizeof(aname), "actuator_%d", i + 1);
        }
        uint8_t atype = cfg->actuators[i].type;
        uint8_t ch = cfg->actuators[i].pin_or_slave;

        if (atype == ACTUATOR_TYPE_PWM) {
            const PWM_Channel_Info_t *pw = PWM_GetChannelInfo(ch < MAX_PWM_CHANNELS ? ch : 0);
            const char *ppin = pw ? pw->pin_name : (ch == 0 ? "PD12" : "PD13");
            pos += snprintf(json_buf + pos, max_len - pos,
                            "%s\"%s\":{\"type\":\"PWM\",\"channel\":%u,\"pin\":\"%s\",\"duty\":%d,\"freq\":%u}",
                            act_emitted ? "," : "", aname, ch + 1, ppin,
                            pw ? (int)pw->duty_pct : 0, pw ? (unsigned)pw->freq_hz : 1000);
        } else if (atype == ACTUATOR_TYPE_PTO) {
            const PTO_Channel_Status_t *pt = PTO_GetStatus(ch < MAX_PTO_CHANNELS ? ch : 0);
            uint32_t freq = (pt && pt->speed_pps > 0) ? pt->speed_pps : (cfg->actuators[i].reg_addr ? cfg->actuators[i].reg_addr : 5000);
            int32_t pto_pos = pt ? pt->position : 0;
            const char *pul = (pt && pt->pul_pin) ? pt->pul_pin : (ch == 0 ? "PE9" : ch == 1 ? "PE5" : ch == 2 ? "PC8" : "PA3");
            const char *dir = (pt && pt->dir_pin) ? pt->dir_pin : (ch == 0 ? "PE8" : ch == 1 ? "PE3" : ch == 2 ? "PC9" : "PA5");
            pos += snprintf(json_buf + pos, max_len - pos,
                            "%s\"%s\":{\"type\":\"PTO\",\"axis\":%u,\"pin\":\"PUL:%s,DIR:%s\",\"pul_pin\":\"%s\",\"dir_pin\":\"%s\",\"frequency\":%lu,\"location\":%ld,\"steps\":%ld,\"moving\":%u}",
                            act_emitted ? "," : "", aname, ch + 1, pul, dir, pul, dir,
                            (unsigned long)freq, (long)pto_pos, (long)pto_pos, pt ? pt->moving : 0);
        } else if (atype == ACTUATOR_TYPE_ANALOG_MA) {
            float ma = DAC_420MA_GetCurrent(ch < MAX_420MA_CHANNELS ? ch : 0);
            const char *apin = (ch == 0 ? "PA4" : ch == 1 ? "PA5" : "PA4");
            pos += snprintf(json_buf + pos, max_len - pos,
                            "%s\"%s\":{\"type\":\"4-20mA\",\"channel\":%u,\"pin\":\"%s\",\"current_ma\":%.2f}",
                            act_emitted ? "," : "", aname, ch + 1, apin, ma);
        } else if (atype == ACTUATOR_TYPE_ANALOG_V) {
            float v = Analog_010V_GetVoltage(ch < MAX_010V_CHANNELS ? ch : 0);
            const char *vpin = (ch == 0 ? "PD14" : ch == 1 ? "PD15" : "PD14");
            pos += snprintf(json_buf + pos, max_len - pos,
                            "%s\"%s\":{\"type\":\"0-10V\",\"channel\":%u,\"pin\":\"%s\",\"voltage_v\":%.2f}",
                            act_emitted ? "," : "", aname, ch + 1, vpin, v);
        } else if (atype == ACTUATOR_TYPE_MODBUS_TCP) {
            pos += snprintf(json_buf + pos, max_len - pos,
                            "%s\"%s\":{\"type\":\"Modbus\",\"ip\":\"%s\",\"slave_id\":%u,\"reg_addr\":%u,\"state\":%u}",
                            act_emitted ? "," : "", aname,
                            cfg->actuators[i].port_or_ip,
                            cfg->actuators[i].pin_or_slave,
                            cfg->actuators[i].reg_addr,
                            relayStates[i]);
        } else if (atype == ACTUATOR_TYPE_OPC_UA_CLIENT) {
            pos += snprintf(json_buf + pos, max_len - pos,
                            "%s\"%s\":{\"type\":\"OPC UA\",\"ip\":\"%s\",\"node_id\":\"%s\",\"state\":%u}",
                            act_emitted ? "," : "", aname,
                            cfg->actuators[i].port_or_ip,
                            cfg->actuators[i].opc_node_id,
                            relayStates[i]);
        } else {
            const char *tname = (atype == ACTUATOR_TYPE_DIGITAL_OUT) ? "DO" : "Relay";
            char rpin[16];
            snprintf(rpin, sizeof(rpin), "%s%u", cfg->actuators[i].port_or_ip, cfg->actuators[i].pin_or_slave);
            pos += snprintf(json_buf + pos, max_len - pos,
                            "%s\"%s\":{\"type\":\"%s\",\"state\":%u,\"pin\":\"%s\",\"mode\":\"%s\"}",
                            act_emitted ? "," : "", aname, tname,
                            relayStates[i],
                            rpin,
                            cfg->actuators[i].is_active_low ? "NC" : "NO");
        }
        act_emitted = 1;
    }

    pos += snprintf(json_buf + pos, max_len - pos,
                    "},\"diagnostics\":{\"uptime_s\":%lu}}",
                    (unsigned long)g_uptime_seconds);
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
            
            Get_Shared_Config(&s_tasks_cfg);
            if (s_tasks_cfg.test_mode != 1) {
                printf("[MQTT] Manual actuator command ignored: Test Mode is OFF (Rules active)\r\n");
                return;
            }
            
            int relay_idx = -1;
            /* Look for relay_0, relay_1 etc */
            if (strncmp(target_str, "relay_", 6) == 0) {
                relay_idx = target_str[6] - '0';
            } else if (strncmp(target_str, "led", 3) == 0) {
                relay_idx = 0; /* Fallback for 'led' target */
            } else {
                /* Try to match relay name */
                for (int i=0; i<s_tasks_cfg.actuator_count; i++) {
                    int name_len = strlen(s_tasks_cfg.actuators[i].name);
                    if (name_len > 0 && strncmp(target_str, s_tasks_cfg.actuators[i].name, name_len) == 0) {
                        relay_idx = i;
                        break;
                    }
                }
            }
            
            if (relay_idx >= 0 && relay_idx < s_tasks_cfg.actuator_count) {
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

void Ensure_W5500_Network_Alive(void) {
    wiz_NetInfo ni;
    ctlnetwork(CN_GET_NETINFO, &ni);
    uint8_t ver = getVERSIONR();

    /* If W5500 lost its IP address (0.0.0.0) or SPI read is corrupted */
    if (ver != 0x04U || (ni.ip[0] == 0 && ni.ip[1] == 0 && ni.ip[2] == 0 && ni.ip[3] == 0)) {
        printf("[NET] ⚠️ W5500 Health Alert: IP=%d.%d.%d.%d, VER=0x%02X! Self-healing W5500 network stack...\r\n",
               ni.ip[0], ni.ip[1], ni.ip[2], ni.ip[3], ver);
        
        Log_Event("SYS", "W5500 self-healing triggered (IP loss detected)");

        wizchip_sw_reset();
        uint8_t rx_tx_buf_sizes[8] = {2, 2, 2, 2, 2, 2, 2, 2};
        wizchip_init(rx_tx_buf_sizes, rx_tx_buf_sizes);

        wiz_NetInfo net = {
            .mac  = {0x00, 0x08, 0xDC, 0x11, 0x22, 0x33},
            .ip   = {192, 168, 1, 200},
            .sn   = {255, 255, 255, 0},
            .gw   = {192, 168, 1, 1},
            .dns  = {8, 8, 8, 8},
            .dhcp = NETINFO_STATIC
        };
        ctlnetwork(CN_SET_NETINFO, &net);
        setMR(0);

        wiz_NetInfo rb;
        ctlnetwork(CN_GET_NETINFO, &rb);
        printf("[NET] ✅ W5500 self-healing complete! IP re-assigned to %d.%d.%d.%d\r\n",
               rb.ip[0], rb.ip[1], rb.ip[2], rb.ip[3]);
    }
}

static void Do_Offline_Telemetry_Queueing(void) {
    TelemetryBatch_t batch = {0};
    Modbus_DMA_ConsumeBatch(&batch);
    
    uint8_t cached_any = 0;
    if (s_tasks_cfg.mqtt_mapping_count > 0) {
        for (int m = 0; m < s_tasks_cfg.mqtt_mapping_count; m++) {
            if (!s_tasks_cfg.mqtt_mappings[m].enabled) continue;
            
            OfflineRecord_t rec;
            rec.timestamp = g_uptime_seconds;
            rec.source_type = s_tasks_cfg.mqtt_mappings[m].source_type;
            rec.source_id = s_tasks_cfg.mqtt_mappings[m].source_id;
            rec.valid = 0;
            rec.value = 0.0f;
            rec.temp = 0.0f;
            
            if (rec.source_type == MAP_SOURCE_SENSOR) {
                for (int s = 0; s < batch.count; s++) {
                    if (batch.records[s].id == rec.source_id) {
                        if (batch.records[s].valid) {
                            rec.value = batch.records[s].avg_value;
                            rec.temp = batch.records[s].avg_temp;
                            rec.valid = 1;
                        }
                        break;
                    }
                }
                if (!rec.valid && s_tasks_cfg.mqtt_skip_offline) continue;
            } else if (rec.source_type == MAP_SOURCE_ACTUATOR) {
                if (rec.source_id < MAX_RELAYS) {
                    rec.value = (float)relayStates[rec.source_id];
                    rec.valid = 1;
                }
            }

            SDCard_Queue_Push(&rec);
            cached_any = 1;
        }
    } else {
        /* Auto-cache all sensors & relays if no custom mapping set */
        for (int s = 0; s < batch.count; s++) {
            if (!batch.records[s].valid) {
                if (s_tasks_cfg.mqtt_skip_offline) continue;
                OfflineRecord_t rec;
                rec.timestamp = g_uptime_seconds;
                rec.source_type = MAP_SOURCE_SENSOR;
                rec.source_id = batch.records[s].id;
                rec.valid = 0;
                rec.value = 0.0f;
                rec.temp = 0.0f;
                SDCard_Queue_Push(&rec);
                cached_any = 1;
                continue;
            }

            OfflineRecord_t rec;
            rec.timestamp = g_uptime_seconds;
            rec.source_type = MAP_SOURCE_SENSOR;
            rec.source_id = batch.records[s].id;
            rec.valid = 1;
            rec.value = batch.records[s].avg_value;
            rec.temp = batch.records[s].avg_temp;
            SDCard_Queue_Push(&rec);
            cached_any = 1;
        }

        uint8_t relay_cnt = (s_tasks_cfg.actuator_count > 0) ? s_tasks_cfg.actuator_count : MAX_RELAYS;
        for (int r = 0; r < relay_cnt && r < MAX_RELAYS; r++) {
            OfflineRecord_t rec;
            rec.timestamp = g_uptime_seconds;
            rec.source_type = MAP_SOURCE_ACTUATOR;
            rec.source_id = r;
            rec.valid = 1;
            rec.value = (float)relayStates[r];
            SDCard_Queue_Push(&rec);
            cached_any = 1;
        }
    }

    if (cached_any) {
        char cache_msg[64];
        snprintf(cache_msg, sizeof(cache_msg), "Broker offline. Telemetry cached. Queue: %lu",
                 (unsigned long)Partition_Queue_Count());
        Log_Event("MQTT", cache_msg);
    }
}

static void Task_MQTTClient(void *arg) {
    (void)arg;
    
    Network n;
    MQTTClient client;
    static unsigned char tx_buf[3072];
    static unsigned char rx_buf[2048];

    printf("[MQTT] Task entered, calling NewNetwork...\r\n");
    NewNetwork(&n, 1);
    
    printf("[MQTT] NewNetwork done, entering 5-second link delay...\r\n");
    /* Wait 5 seconds after boot to let W5500 acquire link/IP */
    osDelay(5000);
    
    printf("[MQTT] Client task woke up! Starting connection loop...\r\n");

    /* Track the broker+port we are currently connected to so we can detect
     * provisioning changes and reconnect automatically. */
    char  connected_broker[64] = {0};
    uint16_t connected_port   = 0;
    uint32_t last_publish     = 0;
    uint8_t broker_empty_notified = 0;
    
    for (;;) {
        if (g_ota_in_progress) {
            osDelay(50);
            continue;
        }

        Get_Shared_Config(&s_tasks_cfg);

        /* Default port to 1883 if unset */
        if (s_tasks_cfg.mqtt_port == 0) s_tasks_cfg.mqtt_port = 1883;
        if (s_tasks_cfg.mqtt_interval == 0 || s_tasks_cfg.mqtt_interval > 86400) {
            s_tasks_cfg.mqtt_interval = 5;
        }
        
        if (s_tasks_cfg.mqtt_broker[0] == '\0') {
            if (!broker_empty_notified) {
                printf("[MQTT] Broker address is unconfigured. MQTT task idle; waiting for configuration via Web UI...\r\n");
                broker_empty_notified = 1;
            }
            g_mqtt_status.connected = 0;
            memset((void*)g_mqtt_status.active_topic, 0, sizeof(g_mqtt_status.active_topic));
            snprintf((char*)g_mqtt_status.last_error, sizeof(g_mqtt_status.last_error), "Broker address is unconfigured.");

            /* Do NOT queue offline telemetry to SPI flash when MQTT is unconfigured.
             * Offline queuing is only intended for temporary connection loss to a configured broker. */
            osDelay(3000);
            continue;
        }
        
        /* Reset notification flag once a broker is configured */
        broker_empty_notified = 0;
        
        uint8_t broker_ip[4];
        if (!Parse_IP(s_tasks_cfg.mqtt_broker, broker_ip)) {
            /* Check W5500 network health before attempting DNS */
            Ensure_W5500_Network_Alive();
            printf("[MQTT] Resolving broker hostname via DNS: %s ...\r\n", s_tasks_cfg.mqtt_broker);
            char log_msg[64];
            snprintf(log_msg, sizeof(log_msg), "Resolving hostname: %s", s_tasks_cfg.mqtt_broker);
            Log_Event("MQTT", log_msg);
            
            wiz_NetInfo net_info;
            ctlnetwork(CN_GET_NETINFO, &net_info);
            
            static uint8_t dns_buf[MAX_DNS_BUF_SIZE];
            static uint8_t dns_inited = 0;
            if (!dns_inited) {
                DNS_init(5, dns_buf);
                dns_inited = 1;
            }
            
            int8_t dns_res = DNS_run(net_info.dns, (uint8_t *)s_tasks_cfg.mqtt_broker, broker_ip);
            if (dns_res != 1) {
                printf("[MQTT] DNS resolution failed for: %s\r\n", s_tasks_cfg.mqtt_broker);
                snprintf(log_msg, sizeof(log_msg), "DNS fail: %s", s_tasks_cfg.mqtt_broker);
                Log_Event("MQTT", log_msg);
                g_mqtt_status.connected = 0;
                snprintf((char*)g_mqtt_status.last_error, sizeof(g_mqtt_status.last_error), "DNS fail: %s", s_tasks_cfg.mqtt_broker);

                uint32_t interval_sec = (s_tasks_cfg.mqtt_interval > 0) ? s_tasks_cfg.mqtt_interval : 5;
                if ((osKernelGetTickCount() - last_publish) >= pdMS_TO_TICKS(interval_sec * 1000)) {
                    Do_Offline_Telemetry_Queueing();
                    last_publish = osKernelGetTickCount();
                }
                osDelay(5000); /* 5-second backoff on DNS failure */
                continue;
            }
            printf("[MQTT] DNS Resolved %s -> %d.%d.%d.%d\r\n", 
                   s_tasks_cfg.mqtt_broker, broker_ip[0], broker_ip[1], broker_ip[2], broker_ip[3]);
            snprintf(log_msg, sizeof(log_msg), "DNS Resolved %s -> %d.%d.%d.%d", 
                     s_tasks_cfg.mqtt_broker, broker_ip[0], broker_ip[1], broker_ip[2], broker_ip[3]);
            Log_Event("MQTT", log_msg);
        }
        
        /* Set active topic: sparkplug_topic takes priority if configured */
        char active_topic[128];
        if (s_tasks_cfg.sparkplug_topic[0] != '\0') {
            strncpy(active_topic, s_tasks_cfg.sparkplug_topic, sizeof(active_topic) - 1);
            active_topic[sizeof(active_topic) - 1] = '\0';
        } else {
            snprintf(active_topic, sizeof(active_topic), "telemetry/kontrx-%07lu", (unsigned long)s_tasks_cfg.serial);
        }
        strncpy((char *)g_mqtt_status.active_topic, active_topic, sizeof(g_mqtt_status.active_topic) - 1);
        g_mqtt_status.active_topic[sizeof(g_mqtt_status.active_topic) - 1] = '\0';
        
        /* ── LWT (Last Will and Testament) ─────────────────────────────────
         * In Sparkplug B, the Node Death (NDEATH) payload is a binary protobuf message
         * configured as the LWT topic: spBv1.0/{group_id}/NDEATH/{node_id}
         * ───────────────────────────────────────────────────────────────── */
        char lwt_topic[128];
        get_sparkplug_topic(s_tasks_cfg.sparkplug_topic, "NDEATH", lwt_topic, sizeof(lwt_topic));
        
        static uint8_t lwt_payload[256];
        size_t lwt_len = 0;
        if (strncmp(s_tasks_cfg.sparkplug_topic, "spBv1.0/", 8) == 0 || strstr(s_tasks_cfg.sparkplug_topic, "/DDATA/") != NULL) {
            uint64_t initial_ts = (uint64_t)g_uptime_seconds * 1000;
            lwt_len = sparkplug_encode_ndeath(lwt_payload, sizeof(lwt_payload), initial_ts, 0);
        } else {
            const char *dev_id = s_tasks_cfg.device_id[0] ? s_tasks_cfg.device_id : "3fa85f64-5717-4562-b3fc-2c963f66afa6";
            snprintf((char*)lwt_payload, sizeof(lwt_payload),
                     "{\"deviceId\":\"%s\",\"tenantId\":\"11111111-2222-3333-4444-555555555555\",\"facilityType\":\"Aquaculture\",\"status\":\"Offline\",\"messageType\":\"NDEATH\"}",
                     dev_id);
            lwt_len = strlen((char*)lwt_payload);
        }

        MQTTPacket_connectData connect_data = MQTTPacket_connectData_initializer;
        connect_data.MQTTVersion = 3;
        connect_data.clientID.cstring = s_tasks_cfg.mqtt_client_id[0] ? s_tasks_cfg.mqtt_client_id : "kontrx-gateway";
        if (s_tasks_cfg.mqtt_username[0]) {
            connect_data.username.cstring = s_tasks_cfg.mqtt_username;
            connect_data.password.cstring = s_tasks_cfg.mqtt_password;
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
        strncpy(connected_broker, s_tasks_cfg.mqtt_broker, sizeof(connected_broker) - 1);
        connected_broker[sizeof(connected_broker) - 1] = '\0';
        connected_port = s_tasks_cfg.mqtt_port;
        
        printf("[MQTT] Connecting to broker %d.%d.%d.%d:%d...\r\n",
               broker_ip[0], broker_ip[1], broker_ip[2], broker_ip[3], s_tasks_cfg.mqtt_port);
        char conn_msg[64];
        snprintf(conn_msg, sizeof(conn_msg), "Connecting to MQTT %s:%d...", s_tasks_cfg.mqtt_broker, s_tasks_cfg.mqtt_port);
        Log_Event("MQTT", conn_msg);
        
        if (ConnectNetwork(&n, broker_ip, s_tasks_cfg.mqtt_port) == SOCK_OK) {
            printf("[MQTT] TCP Connected! Initializing MQTT client...\r\n");
            Log_Event("MQTT", "TCP connection established.");
            
            MQTTClientInit(&client, &n, 5000, tx_buf, sizeof(tx_buf), rx_buf, sizeof(rx_buf));
            
            int rc = MQTTConnect(&client, &connect_data);
            if (rc == SUCCESSS) {
                printf("[MQTT] Connected successfully!\r\n");
                Log_Event("MQTT", "MQTT session connected successfully.");
                g_mqtt_status.connected = 1;
                g_mqtt_status.last_error[0] = '\0';

                /* ── Send NBIRTH (Node Birth) ──────────────────────────────────
                 * Immediately after connecting, announce online state and all metric
                 * definitions/initial values as a binary protobuf NBIRTH payload.
                 * ───────────────────────────────────────────────────────────── */
                char nbirth_topic[128];
                get_sparkplug_topic(s_tasks_cfg.sparkplug_topic, "NBIRTH", nbirth_topic, sizeof(nbirth_topic));
                
                static uint8_t nbirth_buf[2560];
                TelemetryBatch_t nbirth_batch = {0};
                Modbus_DMA_ConsumeBatch(&nbirth_batch); // get current sensors
                size_t nbirth_len = 0;

                if (strncmp(s_tasks_cfg.sparkplug_topic, "spBv1.0/", 8) == 0 || strstr(s_tasks_cfg.sparkplug_topic, "/DDATA/") != NULL) {
                    uint64_t ts_ms = (uint64_t)g_uptime_seconds * 1000;
                    nbirth_len = sparkplug_encode_nbirth(nbirth_buf, sizeof(nbirth_buf), ts_ms, 0,
                                                                &nbirth_batch, &s_tasks_cfg, relayStates);
                } else {
                    Build_Structured_Telemetry_JSON((char*)nbirth_buf, sizeof(nbirth_buf), &nbirth_batch, &s_tasks_cfg);
                    nbirth_len = strlen((char*)nbirth_buf);
                }
                if (nbirth_len > 0) {
                    MQTTMessage birth_msg;
                    birth_msg.qos        = QOS1;
                    birth_msg.retained   = 0;
                    birth_msg.dup        = 0;
                    birth_msg.payload    = (void*)nbirth_buf;
                    birth_msg.payloadlen = nbirth_len;
                    
                    printf("[MQTT] Publishing NBIRTH (%d bytes) to %s\r\n", (int)nbirth_len, nbirth_topic);
                    int b_rc = MQTTPublish(&client, nbirth_topic, &birth_msg);
                    if (strncmp(s_tasks_cfg.sparkplug_topic, "spBv1.0/", 8) == 0 || strstr(s_tasks_cfg.sparkplug_topic, "/DDATA/") != NULL) {
                        char nbirth_log_str[128];
                        snprintf(nbirth_log_str, sizeof(nbirth_log_str), "{\"protocol\":\"Sparkplug B\",\"msg\":\"NBIRTH\",\"status\":\"ONLINE\",\"bytes\":%d}", (int)nbirth_len);
                        Log_Mqtt_Topic(nbirth_topic, nbirth_log_str, b_rc == SUCCESSS);
                    } else {
                        Log_Mqtt_Topic(nbirth_topic, (const char*)nbirth_buf, b_rc == SUCCESSS);
                    }
                    if (b_rc == SUCCESSS) {
                        Log_Event("MQTT", "Announced NBIRTH online state.");
                    } else {
                        char err_msg[64];
                        snprintf(err_msg, sizeof(err_msg), "NBIRTH publish failed (err %d)", b_rc);
                        Log_Event("MQTT", err_msg);
                    }
                }
                
                /* Subscribe to ThingsBoard RPC and generic commands */
                MQTTSubscribe(&client, "v1/devices/me/rpc/request/+", QOS1, messageArrived);
                MQTTSubscribe(&client, "commands/#", QOS1, messageArrived);
                
                last_publish = osKernelGetTickCount();
                
                while (client.isconnected) {
                    uint8_t live_event_published = 0;
                    /* ── Detect any MQTT configuration changes (from provisioning or web UI) ── */
                    static uint32_t local_mqtt_version = 0;
                    if (local_mqtt_version != g_config_version) {
                        /* Save current config before reload to detect changes */
                        Gateway_Config_t prev_cfg;
                        memcpy(&prev_cfg, &s_tasks_cfg, sizeof(prev_cfg));

                        Get_Shared_Config(&s_tasks_cfg);
                        local_mqtt_version = g_config_version;
                        
                        if (s_tasks_cfg.mqtt_port == 0) s_tasks_cfg.mqtt_port = 1883;
                        if (s_tasks_cfg.mqtt_interval == 0 || s_tasks_cfg.mqtt_interval > 86400) {
                            s_tasks_cfg.mqtt_interval = 5;
                        }

                        if (strncmp(s_tasks_cfg.mqtt_broker, connected_broker, sizeof(connected_broker)) != 0 ||
                            s_tasks_cfg.mqtt_port != connected_port ||
                            s_tasks_cfg.mqtt_interval != prev_cfg.mqtt_interval ||
                            s_tasks_cfg.mqtt_send_mode != prev_cfg.mqtt_send_mode ||
                            strcmp(s_tasks_cfg.mqtt_client_id, prev_cfg.mqtt_client_id) != 0 ||
                            strcmp(s_tasks_cfg.mqtt_username, prev_cfg.mqtt_username) != 0 ||
                            strcmp(s_tasks_cfg.mqtt_password, prev_cfg.mqtt_password) != 0 ||
                            strcmp(s_tasks_cfg.sparkplug_topic, prev_cfg.sparkplug_topic) != 0 ||
                            strcmp(s_tasks_cfg.provision_status, prev_cfg.provision_status) != 0) {
                            
                            printf("[MQTT] Config/provision status changed — reconnecting/stopping...\r\n");
                            break; /* exit inner loop → reconnect or sleep with new settings */
                        }
                    }

                    /* Call MQTTYield to maintain keep-alives and process incoming.
                     * Note: MQTTYield returns FAILURE on read timeout (no data available), which is normal.
                     * Check actual W5500 socket state to detect connection loss. */
                    MQTTYield(&client, 100);
                    uint8_t sock_state = getSn_SR(n.my_socket);
                    if (!client.isconnected || (sock_state != SOCK_ESTABLISHED && sock_state != SOCK_CLOSE_WAIT)) {
                        printf("[MQTT] Network socket disconnected (state=0x%02X)\r\n", sock_state);
                        break;
                    }
                    /* Send updates as soon as changes occur */
                    if (1) {
                        /* Refresh config in case interval changed */
                        if (local_mqtt_version != g_config_version) {
                            Get_Shared_Config(&s_tasks_cfg);
                            local_mqtt_version = g_config_version;
                            if (s_tasks_cfg.mqtt_port == 0) s_tasks_cfg.mqtt_port = 1883;
                            if (s_tasks_cfg.mqtt_interval == 0 || s_tasks_cfg.mqtt_interval > 86400) {
                                s_tasks_cfg.mqtt_interval = 5;
                            }
                        }

                        /* Refresh active topic from latest config */
                        if (strcmp(s_tasks_cfg.provision_status, "Active") == 0 && s_tasks_cfg.sparkplug_topic[0] != '\0') {
                            strncpy(active_topic, s_tasks_cfg.sparkplug_topic, sizeof(active_topic) - 1);
                            active_topic[sizeof(active_topic) - 1] = '\0';
                        }
                        strncpy((char *)g_mqtt_status.active_topic, active_topic, sizeof(g_mqtt_status.active_topic) - 1);

                        /* ── State (persistent across publish cycles) ────────────── */
                        static float    cov_last_val[MAX_SENSORS]    = {0};
                        static float    cov_last_tmp[MAX_SENSORS]    = {0};
                        static uint8_t  cov_last_relay[MAX_RELAYS]   = {0};
                        static uint8_t  cov_last_pwm[MAX_PWM_CHANNELS] = {0};
                        static int32_t  cov_last_pto_pos[MAX_PTO_CHANNELS] = {0};
                        static uint8_t  cov_last_pto_mov[MAX_PTO_CHANNELS] = {0};
                        static float    cov_last_ma[MAX_420MA_CHANNELS] = {0};
                        static float    cov_last_v[MAX_010V_CHANNELS] = {0};
                        static uint8_t  cov_last_test_mode           = 0;
                        static uint8_t  cov_ever_pub[MAX_SENSORS]    = {0};
                        static uint8_t  cov_above_db[MAX_SENSORS]    = {0}; /* hysteresis state */
                        static uint32_t cov_last_heartbeat_s         = 0;
                        static uint32_t cov_last_pub_tick            = 0;
                        static uint32_t cov_seq                      = 0;

                        TelemetryBatch_t batch = {0};
                        Modbus_DMA_ConsumeBatch(&batch);
                        
                        /* Check for changes across all actuator types (Relay, PWM, PTO, 4-20mA, 0-10V, Test Mode) */
                        uint8_t actuator_changed = 0;
                        uint8_t act_cnt = (s_tasks_cfg.actuator_count > 0) ? s_tasks_cfg.actuator_count : MAX_RELAYS;
                        for (int i = 0; i < act_cnt && i < MAX_RELAYS; i++) {
                            if (relayStates[i] != cov_last_relay[i]) {
                                actuator_changed = 1;
                                break;
                            }
                        }
                        for (uint8_t c = 0; !actuator_changed && c < PWM_GetChannelCount() && c < MAX_PWM_CHANNELS; c++) {
                            const PWM_Channel_Info_t *pw = PWM_GetChannelInfo(c);
                            if (pw && (uint8_t)pw->duty_pct != cov_last_pwm[c]) {
                                actuator_changed = 1; break;
                            }
                        }
                        for (uint8_t c = 0; !actuator_changed && c < PTO_GetChannelCount() && c < MAX_PTO_CHANNELS; c++) {
                            const PTO_Channel_Status_t *pt = PTO_GetStatus(c);
                            if (pt && (pt->position != cov_last_pto_pos[c] || pt->moving != cov_last_pto_mov[c])) {
                                actuator_changed = 1; break;
                            }
                        }
                        for (uint8_t c = 0; !actuator_changed && c < DAC_420MA_GetChannelCount() && c < MAX_420MA_CHANNELS; c++) {
                            float ma = DAC_420MA_GetCurrent(c);
                            float dma = ma - cov_last_ma[c];
                            if (dma < 0) dma = -dma;
                            if (dma >= 0.05f) { actuator_changed = 1; break; }
                        }
                        for (uint8_t c = 0; !actuator_changed && c < Analog_010V_GetChannelCount() && c < MAX_010V_CHANNELS; c++) {
                            float v = Analog_010V_GetVoltage(c);
                            float dv = v - cov_last_v[c];
                            if (dv < 0) dv = -dv;
                            if (dv >= 0.05f) { actuator_changed = 1; break; }
                        }
                        if (s_tasks_cfg.test_mode != cov_last_test_mode) {
                            actuator_changed = 1;
                        }

                        if (s_tasks_cfg.mqtt_tx_enabled == 0) {
                            /* Publishing paused by Master Transmission Switch */
                            osDelay(100);
                            continue;
                        }

                        uint8_t has_change = 0;
                        const char *trigger = "periodic";
                        
                        if (actuator_changed) {
                            has_change = 1;
                            trigger = "actuator";
                        }

                        if (s_tasks_cfg.mqtt_send_mode == 0) {
                            /* Periodic mode: check if interval has elapsed */
                            if ((osKernelGetTickCount() - last_publish) >= pdMS_TO_TICKS(s_tasks_cfg.mqtt_interval * 1000)) {
                                has_change = 1;
                                trigger = "periodic";
                            }
                        } else if (s_tasks_cfg.mqtt_send_mode == 1) {
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
                                cov_last_heartbeat_s = g_uptime_seconds;
                                has_change = 1;
                                trigger = "heartbeat";
                            }

                            if (!has_change) {
                                for (int i = 0; i < batch.count && i < MAX_SENSORS; i++) {
                                    if (!batch.records[i].valid) continue;
                                    int idx = Get_Sensor_Config_Index(&s_tasks_cfg, batch.records[i].type, batch.records[i].id);
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
                        }

                        if (!has_change) {
                            /* ── Only drain offline queue in Periodic mode ── */
                            if (s_tasks_cfg.mqtt_send_mode == 0) {
                                static uint32_t s_last_queue_drain_tick = 0;
                                uint32_t now_drain_tick = osKernelGetTickCount();
                                uint32_t drain_interval_ticks = pdMS_TO_TICKS(s_tasks_cfg.mqtt_interval > 0 ? (s_tasks_cfg.mqtt_interval * 1000) : 2000);
                                if (drain_interval_ticks < pdMS_TO_TICKS(2000)) drain_interval_ticks = pdMS_TO_TICKS(2000);

                                if (s_tasks_cfg.mqtt_tx_enabled && Partition_Queue_Count() > 0 &&
                                    (now_drain_tick - s_last_queue_drain_tick) >= drain_interval_ticks) {
                                    s_last_queue_drain_tick = now_drain_tick;
                                    static char cache_json[512];
                                    const char *d_id = s_tasks_cfg.device_id[0] ? s_tasks_cfg.device_id :
                                                       (s_tasks_cfg.mqtt_client_id[0] ? s_tasks_cfg.mqtt_client_id : "kontrx-gateway-01");
                                    int pos = snprintf(cache_json, sizeof(cache_json),
                                                       "{\"deviceId\":\"%s\",\"status\":\"Queued\",\"sensors\":{", d_id);
                                    uint8_t count = 0;
                                    uint32_t last_ts = 0;
                                    OfflineRecord_t rec;
                                    char emitted_keys[16][24];
                                    uint8_t emitted_count = 0;

                                    while (count < 8 && Partition_Queue_Count() > 0 && Partition_Queue_Pop(&rec)) {
                                        if (!rec.valid && s_tasks_cfg.mqtt_skip_offline) continue;
                                        char key_name[24] = {0};
                                        for (int m = 0; m < s_tasks_cfg.mqtt_mapping_count; m++) {
                                            if (s_tasks_cfg.mqtt_mappings[m].source_type == rec.source_type &&
                                                s_tasks_cfg.mqtt_mappings[m].source_id == rec.source_id &&
                                                s_tasks_cfg.mqtt_mappings[m].enabled) {
                                                strncpy(key_name, s_tasks_cfg.mqtt_mappings[m].json_key, sizeof(key_name) - 1);
                                                break;
                                            }
                                        }
                                        if (key_name[0] == '\0') {
                                            if (rec.source_type == MAP_SOURCE_SENSOR)
                                                snprintf(key_name, sizeof(key_name), "sensor_%u", rec.source_id);
                                            else
                                                snprintf(key_name, sizeof(key_name), "relay_%u", rec.source_id);
                                        }

                                        /* Ensure key is unique within this batch snapshot */
                                        uint8_t duplicate = 0;
                                        for (uint8_t k = 0; k < emitted_count; k++) {
                                            if (strcmp(emitted_keys[k], key_name) == 0) {
                                                duplicate = 1;
                                                break;
                                            }
                                        }
                                        if (duplicate) {
                                            char suffixed_key[28];
                                            snprintf(suffixed_key, sizeof(suffixed_key), "%s_%u", key_name, count + 1);
                                            pos += snprintf(cache_json + pos, sizeof(cache_json) - pos,
                                                            "%s\"%s\":%.2f", count ? "," : "", suffixed_key, rec.value);
                                        } else {
                                            strncpy(emitted_keys[emitted_count++], key_name, sizeof(emitted_keys[0]) - 1);
                                            pos += snprintf(cache_json + pos, sizeof(cache_json) - pos,
                                                            "%s\"%s\":%.2f", count ? "," : "", key_name, rec.value);
                                        }

                                        if (rec.timestamp > last_ts) last_ts = rec.timestamp;
                                        count++;
                                    }
                                    if (count > 0) {
                                        pos += snprintf(cache_json + pos, sizeof(cache_json) - pos,
                                                        "},\"timestamp\":%lu}", (unsigned long)last_ts);
                                        MQTTMessage msg;
                                        msg.qos = QOS1;
                                        msg.retained = 0;
                                        msg.dup = 0;
                                        msg.payload = (void*)cache_json;
                                        msg.payloadlen = strlen(cache_json);
                                        int cache_pub_rc = MQTTPublish(&client, active_topic, &msg);
                                        Log_Mqtt_Topic(active_topic, cache_json, cache_pub_rc == SUCCESSS);
                                    }
                                }
                            }
                            /* In On Change mode (mqtt_send_mode == 1): do NOT drain queue.
                               Queue will drain when a real change or heartbeat triggers a publish. */
                            osDelay(50);
                            continue;
                        }

                        /* ── 6. RTC Timestamp (seconds since epoch / uptime) ─────── */
                        char ts_str[20];
                        RTC_GetTimeString(ts_str, sizeof(ts_str));

                        /* Update baselines */
                        for (int i = 0; i < batch.count && i < MAX_SENSORS; i++) {
                            if (batch.records[i].valid) {
                                int idx = Get_Sensor_Config_Index(&s_tasks_cfg, batch.records[i].type, batch.records[i].id);
                                if (idx >= 0) {
                                    cov_last_val[idx] = batch.records[i].avg_value;
                                    cov_last_tmp[idx] = batch.records[i].avg_temp;
                                    cov_ever_pub[idx] = 1;
                                }
                            }
                        }
                        
                        /* Update actuator baselines */
                        for (int i = 0; i < act_cnt && i < MAX_RELAYS; i++) {
                            cov_last_relay[i] = relayStates[i];
                        }
                        for (uint8_t c = 0; c < PWM_GetChannelCount() && c < MAX_PWM_CHANNELS; c++) {
                            const PWM_Channel_Info_t *pw = PWM_GetChannelInfo(c);
                            if (pw) cov_last_pwm[c] = (uint8_t)pw->duty_pct;
                        }
                        for (uint8_t c = 0; c < PTO_GetChannelCount() && c < MAX_PTO_CHANNELS; c++) {
                            const PTO_Channel_Status_t *pt = PTO_GetStatus(c);
                            if (pt) { cov_last_pto_pos[c] = pt->position; cov_last_pto_mov[c] = pt->moving; }
                        }
                        for (uint8_t c = 0; c < DAC_420MA_GetChannelCount() && c < MAX_420MA_CHANNELS; c++) {
                            cov_last_ma[c] = DAC_420MA_GetCurrent(c);
                        }
                        for (uint8_t c = 0; c < Analog_010V_GetChannelCount() && c < MAX_010V_CHANNELS; c++) {
                            cov_last_v[c] = Analog_010V_GetVoltage(c);
                        }
                        cov_last_test_mode = s_tasks_cfg.test_mode;

                        int pub_rc = -1;
                        static char pub_payload_str[2048];
                        memset(pub_payload_str, 0, sizeof(pub_payload_str));
                        int is_spb = (s_tasks_cfg.mqtt_payload_shape == 2 ||
                                      strncmp(active_topic, "spBv1.0/", 8) == 0 ||
                                      s_tasks_cfg.sparkplug_topic[0] != '\0');

                        if (is_spb) {
                            static uint8_t ddata_buf[2560];
                            uint64_t ts_ms = (uint64_t)g_uptime_seconds * 1000;
                            
                            /* Increment Sparkplug sequence */
                            cov_seq = (cov_seq + 1) & 0xFF;
                            if (cov_seq == 0) cov_seq = 1;

                            size_t ddata_len = sparkplug_encode_ddata(ddata_buf, sizeof(ddata_buf), ts_ms, cov_seq,
                                                                      &batch, &s_tasks_cfg, relayStates);
                                                                      
                            char ddata_topic[128];
                            get_sparkplug_topic(s_tasks_cfg.sparkplug_topic, "DDATA", ddata_topic, sizeof(ddata_topic));

                            MQTTMessage message;
                            message.qos        = QOS1;  /* PUBACK required — no silent loss */
                            message.retained   = 1;     /* new subscribers get last value   */
                            message.dup        = 0;
                            message.payload    = (void*)ddata_buf;
                            message.payloadlen = ddata_len;

                            /* Build a detailed JSON representation of the Sparkplug B payload for the log */
                            int log_pos = snprintf(pub_payload_str, sizeof(pub_payload_str),
                                                   "{\"protocol\":\"Sparkplug B\",\"msg\":\"DDATA\",\"seq\":%lu,\"bytes\":%d,\"sensors\":{",
                                                   (unsigned long)cov_seq, (int)ddata_len);

                            uint8_t sens_logged = 0;
                            for (int si = 0; si < batch.count && log_pos < (int)sizeof(pub_payload_str) - 80; si++) {
                                if (s_tasks_cfg.mqtt_skip_offline && !batch.records[si].valid) continue;
                                const char *sname = SensorTypeName(batch.records[si].type);
                                if (batch.records[si].valid) {
                                    log_pos += snprintf(pub_payload_str + log_pos, sizeof(pub_payload_str) - log_pos,
                                                        "%s\"%s_%u\":{\"val\":%.2f,\"temp\":%.2f,\"status\":\"online\"}",
                                                        sens_logged ? "," : "",
                                                        sname, batch.records[si].id,
                                                        batch.records[si].avg_value, batch.records[si].avg_temp);
                                } else {
                                    log_pos += snprintf(pub_payload_str + log_pos, sizeof(pub_payload_str) - log_pos,
                                                        "%s\"%s_%u\":{\"val\":0.0,\"temp\":0.0,\"status\":\"offline\"}",
                                                        sens_logged ? "," : "",
                                                        sname, batch.records[si].id);
                                }
                                sens_logged = 1;
                            }

                            log_pos += snprintf(pub_payload_str + log_pos, sizeof(pub_payload_str) - log_pos, "},\"actuators\":{");

                            uint8_t act_logged = 0;
                            for (int i = 0; i < act_cnt && log_pos < (int)sizeof(pub_payload_str) - 60; i++) {
                                char aname[32];
                                if (s_tasks_cfg.actuators[i].name[0] != '\0') {
                                    snprintf(aname, sizeof(aname), "%s", s_tasks_cfg.actuators[i].name);
                                } else {
                                    snprintf(aname, sizeof(aname), "actuator_%d", i + 1);
                                }
                                uint8_t atype = s_tasks_cfg.actuators[i].type;
                                uint8_t ch = s_tasks_cfg.actuators[i].pin_or_slave;

                                if (atype == ACTUATOR_TYPE_PWM) {
                                    const PWM_Channel_Info_t *pw = PWM_GetChannelInfo(ch < MAX_PWM_CHANNELS ? ch : 0);
                                    uint32_t pw_freq = s_tasks_cfg.actuators[i].reg_addr ? s_tasks_cfg.actuators[i].reg_addr : 1000;
                                    const char *ppin = pw ? pw->pin_name : (ch == 0 ? "PD12" : "PD13");
                                    log_pos += snprintf(pub_payload_str + log_pos, sizeof(pub_payload_str) - log_pos,
                                                        "%s\"%s\":{\"type\":\"PWM\",\"channel\":%u,\"pin\":\"%s\",\"duty\":%d,\"freq\":%lu}",
                                                        act_logged ? "," : "", aname, ch + 1, ppin, pw ? (int)pw->duty_pct : 0, (unsigned long)pw_freq);
                                } else if (atype == ACTUATOR_TYPE_ANALOG_MA) {
                                    float ma = DAC_420MA_GetCurrent(ch < MAX_420MA_CHANNELS ? ch : 0);
                                    const char *apin = (ch == 0 ? "PA4" : ch == 1 ? "PA5" : "PA4");
                                    log_pos += snprintf(pub_payload_str + log_pos, sizeof(pub_payload_str) - log_pos,
                                                        "%s\"%s\":{\"type\":\"4-20mA\",\"channel\":%u,\"pin\":\"%s\",\"current_ma\":%.2f}",
                                                        act_logged ? "," : "", aname, ch + 1, apin, ma);
                                } else if (atype == ACTUATOR_TYPE_ANALOG_V) {
                                    float v = Analog_010V_GetVoltage(ch < MAX_010V_CHANNELS ? ch : 0);
                                    const char *vpin = (ch == 0 ? "PD14" : ch == 1 ? "PD15" : "PD14");
                                    log_pos += snprintf(pub_payload_str + log_pos, sizeof(pub_payload_str) - log_pos,
                                                        "%s\"%s\":{\"type\":\"0-10V\",\"channel\":%u,\"pin\":\"%s\",\"voltage_v\":%.2f}",
                                                        act_logged ? "," : "", aname, ch + 1, vpin, v);
                                } else if (atype == ACTUATOR_TYPE_PTO) {
                                    const PTO_Channel_Status_t *pt = PTO_GetStatus(ch < MAX_PTO_CHANNELS ? ch : 0);
                                    uint32_t freq = (pt && pt->speed_pps > 0) ? pt->speed_pps : (s_tasks_cfg.actuators[i].reg_addr ? s_tasks_cfg.actuators[i].reg_addr : 5000);
                                    int32_t pto_pos = pt ? pt->position : 0;
                                    const char *pul = (pt && pt->pul_pin) ? pt->pul_pin : (ch == 0 ? "PE9" : ch == 1 ? "PE5" : ch == 2 ? "PC8" : "PA3");
                                    const char *dir = (pt && pt->dir_pin) ? pt->dir_pin : (ch == 0 ? "PE8" : ch == 1 ? "PE3" : ch == 2 ? "PC9" : "PA5");
                                    log_pos += snprintf(pub_payload_str + log_pos, sizeof(pub_payload_str) - log_pos,
                                                        "%s\"%s\":{\"type\":\"PTO\",\"axis\":%u,\"pin\":\"PUL:%s,DIR:%s\",\"pul_pin\":\"%s\",\"dir_pin\":\"%s\",\"frequency\":%lu,\"location\":%ld,\"steps\":%ld,\"moving\":%u}",
                                                        act_logged ? "," : "", aname, ch + 1, pul, dir, pul, dir,
                                                        (unsigned long)freq, (long)pto_pos, (long)pto_pos, pt ? pt->moving : 0);
                                } else if (atype == ACTUATOR_TYPE_MODBUS_TCP) {
                                    log_pos += snprintf(pub_payload_str + log_pos, sizeof(pub_payload_str) - log_pos,
                                                        "%s\"%s\":{\"type\":\"Modbus\",\"ip\":\"%s\",\"slave\":%u,\"reg\":%u,\"state\":%u}",
                                                        act_logged ? "," : "", aname,
                                                        s_tasks_cfg.actuators[i].port_or_ip,
                                                        s_tasks_cfg.actuators[i].pin_or_slave,
                                                        s_tasks_cfg.actuators[i].reg_addr,
                                                        relayStates[i]);
                                } else if (atype == ACTUATOR_TYPE_OPC_UA_CLIENT) {
                                    log_pos += snprintf(pub_payload_str + log_pos, sizeof(pub_payload_str) - log_pos,
                                                        "%s\"%s\":{\"type\":\"OPC UA\",\"ip\":\"%s\",\"node\":\"%s\",\"state\":%u}",
                                                        act_logged ? "," : "", aname,
                                                        s_tasks_cfg.actuators[i].port_or_ip,
                                                        s_tasks_cfg.actuators[i].opc_node_id,
                                                        relayStates[i]);
                                } else {
                                    char rpin[16];
                                    snprintf(rpin, sizeof(rpin), "%s%u", s_tasks_cfg.actuators[i].port_or_ip, s_tasks_cfg.actuators[i].pin_or_slave);
                                    log_pos += snprintf(pub_payload_str + log_pos, sizeof(pub_payload_str) - log_pos,
                                                        "%s\"%s\":{\"type\":\"%s\",\"state\":%u,\"pin\":\"%s\",\"mode\":\"%s\"}",
                                                        act_logged ? "," : "", aname,
                                                        (atype == ACTUATOR_TYPE_DIGITAL_OUT) ? "DO" : "Relay",
                                                        relayStates[i],
                                                        rpin,
                                                        s_tasks_cfg.actuators[i].is_active_low ? "NC" : "NO");
                                }
                                act_logged = 1;
                            }

                            snprintf(pub_payload_str + log_pos, sizeof(pub_payload_str) - log_pos, "}}");

                            printf("[MQTT] Pub Sparkplug B DDATA (%d bytes, seq=%lu) to %s\r\n",
                                   (int)ddata_len, (unsigned long)cov_seq, ddata_topic);
                            pub_rc = MQTTPublish(&client, ddata_topic, &message);
                        } else {
                            static char json_buf[1024];
                            if (s_tasks_cfg.mqtt_payload_shape == 1) {
                                Build_Flat_Telemetry_JSON(json_buf, sizeof(json_buf), &batch, &s_tasks_cfg);
                            } else {
                                Build_Structured_Telemetry_JSON(json_buf, sizeof(json_buf), &batch, &s_tasks_cfg);
                            }

                            strncpy(pub_payload_str, json_buf, sizeof(pub_payload_str) - 1);

                            MQTTMessage message;
                            message.qos        = QOS1;
                            message.retained   = 1;
                            message.dup        = 0;
                            message.payload    = (void*)json_buf;
                            message.payloadlen = strlen(json_buf);

                            printf("[MQTT] Pub Telemetry JSON (shape=%u) size=%d: %s\r\n",
                                   s_tasks_cfg.mqtt_payload_shape, (int)strlen(json_buf), ts_str);
                            pub_rc = MQTTPublish(&client, active_topic, &message);
                        }

                        if (pub_rc == SUCCESSS) {
                            live_event_published = 1;
                            cov_last_pub_tick = osKernelGetTickCount();
                            last_publish = cov_last_pub_tick;
                            char pub_msg[128];
                            snprintf(pub_msg, sizeof(pub_msg), "Pub %s", active_topic);
                            Log_Event("MQTT", pub_msg);
                            Log_Mqtt_Topic(active_topic, pub_payload_str, 1);
                        } else {
                            Log_Mqtt_Topic(active_topic, pub_payload_str, 0);
                            printf("[MQTT] Publish failed, caching telemetry...\r\n");
                            for (int m = 0; m < s_tasks_cfg.mqtt_mapping_count; m++) {
                                if (!s_tasks_cfg.mqtt_mappings[m].enabled) continue;
                                
                                OfflineRecord_t rec;
                                rec.timestamp = g_uptime_seconds;
                                rec.source_type = s_tasks_cfg.mqtt_mappings[m].source_type;
                                rec.source_id = s_tasks_cfg.mqtt_mappings[m].source_id;
                                rec.valid = 0;
                                rec.value = 0.0f;
                                rec.temp = 0.0f;
                                
                                if (rec.source_type == MAP_SOURCE_SENSOR) {
                                    for (int s = 0; s < batch.count; s++) {
                                        if (batch.records[s].id == rec.source_id && batch.records[s].valid) {
                                            rec.value = batch.records[s].avg_value;
                                            rec.temp = batch.records[s].avg_temp;
                                            rec.valid = 1;
                                            break;
                                        }
                                    }
                                } else if (rec.source_type == MAP_SOURCE_ACTUATOR) {
                                    rec.value = relayStates[rec.source_id];
                                    rec.valid = 1;
                                }
                                
                                if (rec.valid) {
                                    SDCard_Queue_Push(&rec);
                                }
                            }
                            break; // Trigger reconnect
                        }
                    }

                    osDelay(100);
                }
            } else {
                printf("[MQTT] Connect failed, rc = %d\r\n", rc);
                Log_Event("MQTT", "MQTT connect rejected");
                snprintf((char*)g_mqtt_status.last_error, sizeof(g_mqtt_status.last_error), "MQTT rejected (code %d)", rc);
            }
            g_mqtt_status.connected = 0;
            memset((void*)g_mqtt_status.active_topic, 0, sizeof(g_mqtt_status.active_topic));
            MQTTDisconnect(&client);
            client.isconnected = 0;
            disconnect(n.my_socket);
            close(n.my_socket);
        } else {
            printf("[MQTT] TCP Connection failed.\r\n");
            Log_Event("MQTT", "TCP conn failed");
            g_mqtt_status.connected = 0;
            snprintf((char*)g_mqtt_status.last_error, sizeof(g_mqtt_status.last_error), "TCP fail: %s:%d", s_tasks_cfg.mqtt_broker, s_tasks_cfg.mqtt_port);
            close(n.my_socket);
            osDelay(3000);
        }
        
        // Cache telemetry data locally while disconnected (always active when MQTT is disconnected)
        {
            uint32_t start_retry_ms = osKernelGetTickCount();
            uint32_t interval_sec = (s_tasks_cfg.mqtt_interval > 0) ? s_tasks_cfg.mqtt_interval : 5;
            while (osKernelGetTickCount() - start_retry_ms < pdMS_TO_TICKS(5000)) {
                if ((osKernelGetTickCount() - last_publish) >= pdMS_TO_TICKS(interval_sec * 1000)) {
                    Do_Offline_Telemetry_Queueing();
                    last_publish = osKernelGetTickCount();
                }
                osDelay(100);
            }
        }
    }
}

osThreadId_t g_tid_control = NULL;
osThreadId_t g_tid_modbus  = NULL;
osThreadId_t g_tid_mqtt    = NULL;

/* ======================================================================
 *  RTOS_Tasks_Init — Creates all tasks and starts the scheduler
 *  Called from main().  Does NOT return.
 * ====================================================================== */
void RTOS_Tasks_Init(void) {
    // 1. Initialize cJSON hooks with FreeRTOS heap memory functions
    cJSON_Hooks cjson_hooks = {
        .malloc_fn = pvPortMalloc,
        .free_fn = vPortFree
    };
    cJSON_InitHooks(&cjson_hooks);

    // 2. Initialize rules mutex and load rules configuration
    if (!rulesMutex) {
        rulesMutex = osMutexNew(NULL);
    }
    Partition_LoadRules(&activeRules);

    osThreadAttr_t attr = {0};

    /* Task 1 — Control Engine */
    attr.name       = "ControlEng";
    attr.stack_size = 6144; // Increased to 6KB to guarantee stack safety for string resolution and printfs
    attr.priority   = osPriorityRealtime;
    g_tid_control   = osThreadNew(Task_ControlEngine, NULL, &attr);

    /* Task 2 — Modbus Sensor Poll */
    attr.name       = "ModbusPoll";
    attr.stack_size = 4096; 
    attr.priority   = osPriorityNormal;
    g_tid_modbus    = osThreadNew(Task_ModbusSensorPoll, NULL, &attr);

    /* Task 3 — HTTP Server */
    attr.name       = "HTTPServer";
    attr.stack_size = 12288;  /* Increased to 12KB to guarantee absolute stack overflow protection */
    attr.priority   = osPriorityNormal;
    osThreadNew(Task_HTTPServer, NULL, &attr);

    /* Task 4 — OTA Background */
    attr.name       = "OTAUpdate";
    attr.stack_size = 4096; // Increased to 4KB for firmware write stack buffer safety
    attr.priority   = osPriorityLow;
    osThreadNew(Task_OTAUpdate, NULL, &attr);

    /* Task 5 — MQTT Client */
    attr.name       = "MQTTClient";
    attr.stack_size = 8192;  /* Increased from 4096: MQTTClient+Network structs +
                              * LWT payload + sparkplug encode buffers + tx/rx[512]
                              * + deep MQTTConnect/MQTTPublish call frames. */
    attr.priority   = osPriorityAboveNormal;
    g_tid_mqtt      = osThreadNew(Task_MQTTClient, NULL, &attr);

    osKernelStart();
}
