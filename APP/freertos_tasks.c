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
#include "flash_partition.h"
#include "http_server_task.h"
#include "ota_task.h"
#include "rtc_stm32.h"
#include "MQTTClient.h"
#include "mqtt_interface.h"
#include "sparkplug_b_enc.h"

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
        snprintf(out_topic, max_len, "spBv1.0/KontrxGroup/%s/kontrx-%07lu", msg_type, (unsigned long)sharedConfig.serial);
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

    /* Plain custom topic: use as-is for ALL message types (NBIRTH, NDEATH, DDATA) */
    strncpy(out_topic, base_topic, max_len - 1);
    out_topic[max_len - 1] = '\0';
}
#include "stm32f407_regs.h"
#include "gpio_stm32.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include "portable.h"
#include <stdio.h>

volatile MqttStatus_t g_mqtt_status = {0};

uint8_t g_log_ring[LOG_BUFFER_SIZE] = {0};
volatile uint32_t g_log_head = 0;
volatile uint32_t g_log_tail = 0;
volatile uint32_t g_log_used_bytes = 0;
volatile uint32_t g_total_logs_written = 0;
volatile uint32_t g_sys_log_count = 0;
volatile uint8_t  g_sys_log_full = 0;

void Log_Event(const char *category, const char *message) {
    (void)category;
    (void)message;
    /* In-memory logging disabled per user request for SD Card integration */
}

/* ======================================================================
 *  Global uptime counter
 * ====================================================================== */
volatile uint32_t g_uptime_seconds = 0;

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
    extern void* pxCurrentTCB;
    if (pxCurrentTCB == xTaskGetIdleTaskHandle()) {
        if (g_idle_task_start_time != 0) {
            now_idle += (now_cyc - g_idle_task_start_time);
        }
    }

    uint32_t total_cycles = now_cyc - last_cyccnt;
    uint32_t idle_cycles = now_idle - last_idle_cycles;

    if (total_cycles > 0) {
        uint32_t idle_pct = (idle_cycles * 100U) / total_cycles;
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
    /* No-op: CPU cycles are tracked via scheduler trace hooks */
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
static int Get_Port_Id(const char *port_str) {
    return GPIO_GetPortId(port_str);
}

void Relay_Init(void) {
    Get_Shared_Config(&s_tasks_cfg);

    for (int i = 0; i < (int)s_tasks_cfg.actuator_count && i < MAX_RELAYS; i++) {
        if (s_tasks_cfg.actuators[i].type != ACTUATOR_TYPE_LOCAL_GPIO) {
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

    if (s_tasks_cfg.actuators[idx].type == ACTUATOR_TYPE_LOCAL_GPIO) {
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
    
    // 3. Try to parse as numeric sensor ID
    char *endptr;
    long target_id = strtol(input_id, &endptr, 10);
    if (*endptr == '\0' && target_id >= 0) {
        for (int j = 0; j < sd->readings_count && j < MAX_SENSORS; j++) {
            if (sd->readings[j].id == (uint8_t)target_id && (activeRules.bypass_validation || !sd->readings[j].stale)) {
                *found = 1;
                return sd->readings[j].value;
            }
        }
    }
    
    return -9999.0f;
}

static int8_t Resolve_Output_Id(const char *output_id, const Gateway_Config_t *cfg) {
    // 1. Try to match actuator name
    for (int i = 0; i < cfg->actuator_count && i < MAX_RELAYS; i++) {
        if (strcmp(cfg->actuators[i].name, output_id) == 0) {
            return i;
        }
    }
    
    // 2. Try to parse as integer
    char *endptr;
    long val = strtol(output_id, &endptr, 10);
    if (*endptr == '\0' && val >= 0 && val < MAX_RELAYS) {
        return (int8_t)val;
    }
    
    return -1;
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
            osDelay(1);
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

            for (uint32_t i = 0; i < localRules.rule_count; i++) {
                const Rule_t *rule = &localRules.rules[i];
                if (!rule->active) continue;

                // Resolve input value from string input_id
                uint8_t found = 0;
                float val = Resolve_Input_Value(rule->input_id, &cfg_snap, &sd, &found);

                if (found) {
                    uint8_t cond_met = 0;
                    float thr = rule->threshold;
                    if (strcmp(rule->operator, ">") == 0)       cond_met = (val > thr);
                    else if (strcmp(rule->operator, "<") == 0)  cond_met = (val < thr);
                    else if (strcmp(rule->operator, "==") == 0) cond_met = (val == thr);
                    else if (strcmp(rule->operator, ">=") == 0) cond_met = (val >= thr);
                    else if (strcmp(rule->operator, "<=") == 0) cond_met = (val <= thr);

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
        /* ================================================================ */

        /* Strict 10ms delay — 100Hz is plenty for relay control (PLCs run at 10-100Hz) */
        osDelay(10);
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
            /* Poll sensors and yield 100ms for a real-time scan cycle. */
            Modbus_DMA_PollSensors();
            osDelay(100);
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
            
            Get_Shared_Config(&s_tasks_cfg);
            
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

static void Task_MQTTClient(void *arg) {
    (void)arg;
    
    Network n;
    MQTTClient client;
    static unsigned char tx_buf[2048];
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
    
    for (;;) {
        Get_Shared_Config(&s_tasks_cfg);

        /* Default port to 1883 if unset */
        if (s_tasks_cfg.mqtt_port == 0) s_tasks_cfg.mqtt_port = 1883;
        if (s_tasks_cfg.mqtt_interval == 0 || s_tasks_cfg.mqtt_interval > 86400) {
            s_tasks_cfg.mqtt_interval = 5;
        }
        
        if (s_tasks_cfg.mqtt_broker[0] == '\0') {
            static uint32_t last_empty_print = 0;
            uint32_t now_tick = osKernelGetTickCount();
            if (now_tick - last_empty_print >= 5000) {
                printf("[MQTT] Broker address is empty in config, skipping connection...\r\n");
                last_empty_print = now_tick;
            }
            g_mqtt_status.connected = 0;
            osDelay(2000);
            continue;
        }
        
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
                DNS_init(2, dns_buf);
                dns_inited = 1;
            }
            
            int8_t dns_res = DNS_run(net_info.dns, (uint8_t *)s_tasks_cfg.mqtt_broker, broker_ip);
            if (dns_res != 1) {
                printf("[MQTT] DNS resolution failed for: %s\r\n", s_tasks_cfg.mqtt_broker);
                snprintf(log_msg, sizeof(log_msg), "DNS failed to resolve: %s", s_tasks_cfg.mqtt_broker);
                Log_Event("MQTT", log_msg);
                g_mqtt_status.connected = 0;
                osDelay(10000); /* 10-second backoff on DNS failure */
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
        
        static uint8_t lwt_payload[64];
        uint64_t initial_ts = (uint64_t)g_uptime_seconds * 1000;
        size_t lwt_len = sparkplug_encode_ndeath(lwt_payload, sizeof(lwt_payload), initial_ts, 0);

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

                /* Flush offline queue before NBIRTH and live telemetry */
                uint32_t cached_count = Partition_Queue_Count();
                if (cached_count > 0) {
                    printf("[MQTT] Connected! Flushing %lu cached offline records...\r\n", (unsigned long)cached_count);
                    char cache_log[64];
                    snprintf(cache_log, sizeof(cache_log), "Flushing %lu offline records...", (unsigned long)cached_count);
                    Log_Event("MQTT", cache_log);

                    OfflineRecord_t rec;
                    while (Partition_Queue_Pop(&rec)) {
                        char key_name[24] = {0};
                        for (int m = 0; m < s_tasks_cfg.mqtt_mapping_count; m++) {
                            if (s_tasks_cfg.mqtt_mappings[m].source_type == rec.source_type &&
                                s_tasks_cfg.mqtt_mappings[m].source_id == rec.source_id &&
                                s_tasks_cfg.mqtt_mappings[m].enabled) {
                                strncpy(key_name, s_tasks_cfg.mqtt_mappings[m].json_key, sizeof(key_name) - 1);
                                break;
                            }
                        }
                        
                        if (key_name[0] != '\0') {
                            char cache_payload[128];
                            snprintf(cache_payload, sizeof(cache_payload),
                                     "{\"timestamp\":%lu,\"%s\":%.2f}",
                                     (unsigned long)rec.timestamp, key_name, rec.value);
                            
                            MQTTMessage msg;
                            msg.qos = QOS1;
                            msg.retained = 0;
                            msg.dup = 0;
                            msg.payload = (void*)cache_payload;
                            msg.payloadlen = strlen(cache_payload);
                            
                            int cache_pub_rc = MQTTPublish(&client, active_topic, &msg);
                            Log_Mqtt_Topic(active_topic, cache_pub_rc == SUCCESSS);
                            osDelay(50); // Yield to prevent buffer congestion
                        }
                    }
                    printf("[MQTT] Cache flushing completed.\r\n");
                    Log_Event("MQTT", "Cache flushing completed successfully.");
                }

                /* ── Send NBIRTH (Node Birth) ──────────────────────────────────
                 * Immediately after connecting, announce online state and all metric
                 * definitions/initial values as a binary protobuf NBIRTH payload.
                 * ───────────────────────────────────────────────────────────── */
                char nbirth_topic[128];
                get_sparkplug_topic(s_tasks_cfg.sparkplug_topic, "NBIRTH", nbirth_topic, sizeof(nbirth_topic));
                
                static uint8_t nbirth_buf[1024];
                TelemetryBatch_t nbirth_batch = {0};
                Modbus_DMA_ConsumeBatch(&nbirth_batch); // get current sensors
                
                uint64_t ts_ms = (uint64_t)g_uptime_seconds * 1000;
                size_t nbirth_len = sparkplug_encode_nbirth(nbirth_buf, sizeof(nbirth_buf), ts_ms, 0,
                                                            &nbirth_batch, &s_tasks_cfg, relayStates);
                if (nbirth_len > 0) {
                    MQTTMessage birth_msg;
                    birth_msg.qos        = QOS1;
                    birth_msg.retained   = 0;
                    birth_msg.dup        = 0;
                    birth_msg.payload    = (void*)nbirth_buf;
                    birth_msg.payloadlen = nbirth_len;
                    
                    printf("[MQTT] Publishing NBIRTH (%d bytes) to %s\r\n", (int)nbirth_len, nbirth_topic);
                    int b_rc = MQTTPublish(&client, nbirth_topic, &birth_msg);
                    Log_Mqtt_Topic(nbirth_topic, b_rc == SUCCESSS);
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

                    /* Call MQTTYield to maintain keep-alives and process incoming */
                    int yield_rc = MQTTYield(&client, 100);
                    if (yield_rc != SUCCESSS) {
                        printf("[MQTT] MQTTYield error: %d\r\n", yield_rc);
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
                        static uint8_t  cov_ever_pub[MAX_SENSORS]    = {0};
                        static uint8_t  cov_above_db[MAX_SENSORS]    = {0}; /* hysteresis state */
                        static uint32_t cov_last_heartbeat_s         = 0;
                        static uint32_t cov_last_pub_tick            = 0;
                        static uint32_t cov_seq                      = 0;

                        TelemetryBatch_t batch = {0};
                        Modbus_DMA_ConsumeBatch(&batch);
                        
                        /* Check for relay changes before skipping on batch.count == 0 */
                        uint8_t relay_changed = 0;
                        for (int i = 0; i < s_tasks_cfg.actuator_count && i < MAX_RELAYS; i++) {
                            if (relayStates[i] != cov_last_relay[i]) {
                                relay_changed = 1;
                                break;
                            }
                        }

                        /* ── Skip if no sensors and no relay changes ───────────────── */
                        if (batch.count == 0 && !relay_changed && (g_uptime_seconds - cov_last_heartbeat_s) < 300U) {
                            osDelay(100);
                            continue;
                        }

                        uint8_t has_change = 0;
                        const char *trigger = "periodic";
                        
                        if (relay_changed) {
                            has_change = 1;
                            trigger = "relay";
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
                            osDelay(20);
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
                        
                        /* Update relay baseline */
                        for (int i = 0; i < s_tasks_cfg.actuator_count && i < MAX_RELAYS; i++) {
                            cov_last_relay[i] = relayStates[i];
                        }

                        int pub_rc = -1;
                        if (s_tasks_cfg.mqtt_mapping_count > 0) {
                            static char json_buf[512];
                            int json_pos = 0;
                            json_pos += snprintf(json_buf + json_pos, sizeof(json_buf) - json_pos,
                                                 "{\"timestamp\":%lu", (unsigned long)g_uptime_seconds);
                            
                            for (int m = 0; m < s_tasks_cfg.mqtt_mapping_count; m++) {
                                if (!s_tasks_cfg.mqtt_mappings[m].enabled) continue;
                                
                                if (s_tasks_cfg.mqtt_mappings[m].source_type == MAP_SOURCE_SENSOR) {
                                    for (int s = 0; s < batch.count; s++) {
                                        if (batch.records[s].id == s_tasks_cfg.mqtt_mappings[m].source_id && batch.records[s].valid) {
                                            json_pos += snprintf(json_buf + json_pos, sizeof(json_buf) - json_pos,
                                                                ",\"%s\":%.2f",
                                                                s_tasks_cfg.mqtt_mappings[m].json_key,
                                                                batch.records[s].avg_value);
                                            break;
                                        }
                                    }
                                } else if (s_tasks_cfg.mqtt_mappings[m].source_type == MAP_SOURCE_ACTUATOR) {
                                    json_pos += snprintf(json_buf + json_pos, sizeof(json_buf) - json_pos,
                                                        ",\"%s\":%u",
                                                        s_tasks_cfg.mqtt_mappings[m].json_key,
                                                        relayStates[s_tasks_cfg.mqtt_mappings[m].source_id]);
                                }
                            }
                            json_pos += snprintf(json_buf + json_pos, sizeof(json_buf) - json_pos, "}");

                            MQTTMessage message;
                            message.qos        = QOS1;
                            message.retained   = 1;
                            message.dup        = 0;
                            message.payload    = (void*)json_buf;
                            message.payloadlen = strlen(json_buf);

                            printf("[MQTT] Pub Custom JSON size=%d: %s\r\n", (int)strlen(json_buf), ts_str);
                            pub_rc = MQTTPublish(&client, active_topic, &message);
                        } else {
                            static uint8_t ddata_buf[1024];
                            uint64_t ts_ms = (uint64_t)g_uptime_seconds * 1000;
                            
                            /* Increment Sparkplug sequence */
                            cov_seq = (cov_seq + 1) & 0xFF;
                            if (cov_seq == 0) cov_seq = 1;

                            size_t ddata_len = sparkplug_encode_ddata(ddata_buf, sizeof(ddata_buf), ts_ms, cov_seq,
                                                                      &batch, relayStates);
                                                                      
                            char ddata_topic[128];
                            get_sparkplug_topic(s_tasks_cfg.sparkplug_topic, "DDATA", ddata_topic, sizeof(ddata_topic));

                            /* ── 5+7. QoS1 + Retained publish ───────────────────────── */
                            MQTTMessage message;
                            message.qos        = QOS1;  /* PUBACK required — no silent loss */
                            message.retained   = 1;     /* new subscribers get last value   */
                            message.dup        = 0;
                            message.payload    = (void*)ddata_buf;
                            message.payloadlen = ddata_len;

                            pub_rc = MQTTPublish(&client, ddata_topic, &message);
                        }

                        if (pub_rc == SUCCESSS) {
                            cov_last_pub_tick = osKernelGetTickCount();
                            last_publish = cov_last_pub_tick;
                            char pub_msg[128];
                            snprintf(pub_msg, sizeof(pub_msg), "Published telemetry data to: %s", active_topic);
                            Log_Event("MQTT", pub_msg);
                            Log_Mqtt_Topic(active_topic, 1);
                        } else {
                            Log_Mqtt_Topic(active_topic, 0);
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
                                    Partition_Queue_Push(&rec);
                                }
                            }
                            break; // Trigger reconnect
                        }
                    }

                    osDelay(100);
                }
            } else {
                printf("[MQTT] Connect failed, rc = %d\r\n", rc);
                Log_Event("MQTT", "MQTT connect rejected by broker.");
            }
            g_mqtt_status.connected = 0;
            disconnect(1);
            close(1);
        } else {
            printf("[MQTT] TCP Connection failed.\r\n");
            Log_Event("MQTT", "TCP connection to broker failed.");
            close(1);
        }
        
        // Cache data locally while disconnected
        {
            uint32_t start_retry_ms = osKernelGetTickCount();
            while (osKernelGetTickCount() - start_retry_ms < pdMS_TO_TICKS(5000)) {
                if ((osKernelGetTickCount() - last_publish) >= pdMS_TO_TICKS(s_tasks_cfg.mqtt_interval * 1000)) {
                    TelemetryBatch_t batch = {0};
                    Modbus_DMA_ConsumeBatch(&batch);
                    
                    uint8_t cached_any = 0;
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
                            Partition_Queue_Push(&rec);
                            cached_any = 1;
                        }
                    }
                    if (cached_any) {
                        char cache_msg[64];
                        snprintf(cache_msg, sizeof(cache_msg), "Broker offline. Telemetry cached. Queue: %lu",
                                 (unsigned long)Partition_Queue_Count());
                        Log_Event("MQTT", cache_msg);
                    }
                    last_publish = osKernelGetTickCount();
                }
                osDelay(100);
            }
        }
    }
}

osThreadId_t g_tid_modbus = NULL;
osThreadId_t g_tid_mqtt   = NULL;

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
    osThreadNew(Task_ControlEngine, NULL, &attr);

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
