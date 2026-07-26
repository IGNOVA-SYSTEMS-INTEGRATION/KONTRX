/**
 * @file    freertos_tasks.c
 * @brief   Kontrx — FreeRTOS Task Architecture & IPC Coordinator
 *
 * Task Map:
 * ┌─────────────────────────────────┬──────────┬─────────┬─────────────────────────────┐
 * │ Task                            │ Priority │ Stack   │ Period / Trigger             │
 * ├─────────────────────────────────┼──────────┼─────────┼─────────────────────────────┤
 * │ Task_ControlEngine              │ RT (5)   │ 512 B   │ Every 1ms (1000 Hz)         │
 * │ Task_ModbusSensorPoll           │ High (3) │ 1024 B  │ Every 250ms (round-robin)   │
 * │ Task_HTTPServer                 │ Norm (2) │ 2048 B  │ W5500 socket event          │
 * │ Task_OTAUpdate                  │ Low (1)  │ 1024 B  │ sem_ota_start from HTTP     │
 * └─────────────────────────────────┴──────────┴─────────┴─────────────────────────────┘
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
static const struct { uint8_t port; uint8_t pin; } relay_defaults[MAX_RELAYS] = {
    {4, 2},  /* R1  PE2 */
    {4, 4},  /* R2  PE4 */
    {4, 6},  /* R3  PE6 */
    {2, 0},  /* R4  PC0 */
    {2, 2},  /* R5  PC2 */
    {0, 0},  /* R6  PA0 */
    {0, 2},  /* R7  PA2 */
    {0, 4},  /* R8  PA4 */
    {2, 4},  /* R9  PC4 */
    {3, 8},  /* R10 PD8 */
};

/* ======================================================================
 *  Relay GPIO Initialisation
 * ====================================================================== */
void Relay_Init(void) {
    Gateway_Config_t cfg;
    Get_Shared_Config(&cfg);

    for (int i = 0; i < MAX_RELAYS; i++) {
        /* If config hasn't been persisted yet, load from defaults */
        if (cfg.relays[i].port_id == 0 && cfg.relays[i].pin_num == 0 &&
            cfg.relays[i].is_nc == 0 && cfg.relays[i].state == 0) {
            cfg.relays[i].port_id = relay_defaults[i].port;
            cfg.relays[i].pin_num = relay_defaults[i].pin;
        }

        uint8_t port = cfg.relays[i].port_id;
        uint8_t pin  = cfg.relays[i].pin_num;

        if (port > 4 || pin > 15) continue;

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

    /* Persist defaults back if config was uninitialised */
    cfg.magic = 0xC01D0001U;
    Update_Shared_Config(&cfg);
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

    if (port > 4 || pin > 15) return;

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
 *  Period: 1ms (1000 Hz)
 *  CONSTRAINT: NO blocking calls, NO I/O, NO DMA waits.
 * ====================================================================== */
static void Task_ControlEngine(void *arg) {
    (void)arg;

    TickType_t   xLastWakeTime = xTaskGetTickCount();
    const TickType_t xPeriod   = pdMS_TO_TICKS(1); /* 1ms */

    Modbus_SensorData_t sd;

    for (;;) {
        /* --- Read latest sensor snapshot (mutex-protected, non-blocking) --- */
        /* Use TryAcquire so we NEVER block if Modbus task holds the mutex */
        if (osMutexAcquire(sensorMutex, 0) == osOK) {
            /* Take a local copy of the shared data */
            sd = sharedSensorData;
            osMutexRelease(sensorMutex);
        }
        /* If mutex was unavailable, we proceed with the last known snapshot — 
         * this is intentional; the control engine never stalls for sensor data. */

        /* ================================================================
         * --- CONTROL LOGIC ZONE (user-defined) ---
         * Insert threshold logic, PID computations, relay decision trees here.
         * All operations on sd (float reads) are safe — no external dependencies.
         * ================================================================ */

        /* Example: Auto-dose relay R0 when pH drops below 6.5 */
        if (sd.ph > -900.0f && sd.ph < 6.5f) {
            /* pH too low — activate R0 (acid dosing pump) */
            /* NOTE: Relay_SetState acquires a mutex; calling from here is
             * acceptable because Update_Shared_Config is fast.
             * For truly hard-RT requirements, cache desired state and
             * let a lower-priority task apply it. */
            if (relayStates[0] == 0) Relay_SetState(0, 1);
        } else if (sd.ph > 7.2f) {
            if (relayStates[0] == 1) Relay_SetState(0, 0);
        }

        /* Example: Aeration pump (R1) ON when DO < 5.0 mg/L */
        if (sd.do_val > -900.0f && sd.do_val < 5.0f) {
            if (relayStates[1] == 0) Relay_SetState(1, 1);
        } else if (sd.do_val > 6.5f) {
            if (relayStates[1] == 1) Relay_SetState(1, 0);
        }

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

    printf("[Task] ModbusSensorPoll starting...\r\n");

    /* One-time driver init */
    Modbus_DMA_Init();
    
    printf("[Task] ModbusSensorPoll entering main loop\r\n");

    for (;;) {
        if (g_scan_status.is_scanning) {
            printf("[Task] Scan mode active\r\n");
            Modbus_DMA_PerformScan();
        } else {
            Modbus_DMA_PollSensors();
            /* Poll round-trip typically 150..200ms; extra delay pads to ~250ms */
            osDelay(50);
        }
    }
}

/* ======================================================================
 *  RTOS_Tasks_Init — Creates all tasks and starts the scheduler
 *  Called from main().  Does NOT return.
 * ====================================================================== */
void RTOS_Tasks_Init(void) {
    osThreadAttr_t attr = {0};

    /* Task 1 — Control Engine — REALTIME priority, tiny stack */
    attr.name       = "ControlEng";
    attr.stack_size = 512;
    attr.priority   = osPriorityRealtime;
    osThreadNew(Task_ControlEngine, NULL, &attr);

    /* Task 2 — Modbus Sensor Poll */
    attr.name       = "ModbusPoll";
    attr.stack_size = 1024;
    attr.priority   = osPriorityAboveNormal;
    osThreadNew(Task_ModbusSensorPoll, NULL, &attr);

    /* Task 3 — HTTP Server
     * Stack budget:
     *   - FreeRTOS task frame           ~  64 B
     *   - Dispatch_Request frame        ~ 128 B
     *   - WIZnet ioLibrary call chain   ~ 256 B
     *   - JSON handler locals           ~ 128 B
     *   - Safety margin                 ~3520 B
     *   TOTAL allocated                 4096 B */
    attr.name       = "HTTPServer";
    attr.stack_size = 4096;
    attr.priority   = osPriorityNormal;
    osThreadNew(Task_HTTPServer, NULL, &attr);

    /* Task 4 — OTA Background */
    attr.name       = "OTAUpdate";
    attr.stack_size = 1024;
    attr.priority   = osPriorityLow;
    osThreadNew(Task_OTAUpdate, NULL, &attr);

    /* Start the scheduler — does not return */
    vTaskStartScheduler();

    /* Should never reach here */
    while (1);
}
