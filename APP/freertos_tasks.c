/**
 * @file    freertos_tasks.c
 * @brief   Kontrx — FreeRTOS Task Architecture & IPC Coordinator
 *
 * Task Map:
 * ┌─────────────────────────────────┬──────────┬─────────┬─────────────────────────────┐
 * │ Task                            │ Priority │ Stack   │ Period / Trigger             │
 * ├─────────────────────────────────┼──────────┼─────────┼─────────────────────────────┤
 * │ Task_ControlEngine              │ RT (5)   │ 512 B   │ Every 1ms (1000 Hz) ← ONLY   │
 * │ Task_ModbusSensorPoll           │ High (3) │ 1024 B  │ Every ~500ms (poll cycle)    │
 * │ Task_HTTPServer                 │ Norm (2) │ 2048 B  │ W5500 socket event          │
 * │ Task_OTAUpdate                  │ Low (1)  │ 1024 B  │ sem_ota_start from HTTP     │
 * └─────────────────────────────────┴──────────┴─────────┴─────────────────────────────┘
 *
 * NOTE: ONLY Task_ControlEngine runs at exactly 1ms. The Modbus poll task runs
 * every ~500ms (+ N x ~70ms per sensor cycle at 9600 baud) — see modbus_dma.c.
 * The 1ms Control Engine is never blocked by it: priority 5 > 3, and the engine
 * uses non-blocking osMutexAcquire(..., 0) + vTaskDelayUntil.
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
#include "stm32f407_regs.h"
#include "gpio_stm32.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

/* ======================================================================
 *  Global uptime counter
 * ====================================================================== */
volatile uint32_t g_uptime_seconds = 0;

/* FreeRTOS tick hook — increments uptime every second */
static volatile uint32_t g_tick_counter = 0;

void vApplicationTickHook(void) {
    g_tick_counter++;
    if (g_tick_counter >= (uint32_t)configTICK_RATE_HZ) {
        g_tick_counter = 0;
        g_uptime_seconds++;
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
 *  Round-robins all 5 sensor IDs every ~250ms.
 * ====================================================================== */
static void Task_ModbusSensorPoll(void *arg) {
    (void)arg;
    osDelay(2000); // 2s boot delay: let the W5500 finish link negotiation and
                   // sensors power up before the first Modbus poll. One-time only.
    
    printf("[Task] ModbusSensorPoll starting...\r\n");

    /* One-time driver init */
    Modbus_DMA_Init();
        osDelay(1000); // 1s post-init: settle UART/MAX485 before first read.

    printf("[Task] ModbusSensorPoll entering main loop\r\n");

    for (;;) {
        if (g_scan_status.is_scanning) {
            printf("[Task] Scan mode active\r\n");
            Modbus_DMA_PerformScan();
            osDelay(2000); // 2s pause between full network scans (247 probes).
                           // One-shot user action, not the realtime path.
        } else {
            Modbus_DMA_PollSensors();
            osDelay(500); /* 500ms gap between complete poll cycles. This sets
                           * the sensor refresh rate. Each cycle itself already
                           * takes N x ~70ms (transaction+gap), so 1ms is
                           * physically impossible at 9600 baud. The 1ms Control
                           * Engine is NOT delayed by this: it has higher
                           * priority (5 vs 3) and only reads the snapshot. */
        }
    }
}

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
    osThreadNew(Task_ModbusSensorPoll, NULL, &attr);

    /* Task 3 — HTTP Server */
    attr.name       = "HTTPServer";
    attr.stack_size = 4096;
    attr.priority   = osPriorityNormal;
    osThreadNew(Task_HTTPServer, NULL, &attr);

    /* Task 4 — OTA Background */
    attr.name       = "OTAUpdate";
    attr.stack_size = 2048; // زدناها من 1024 إلى 2048
    attr.priority   = osPriorityLow;
    osThreadNew(Task_OTAUpdate, NULL, &attr);

    osKernelStart();
}
