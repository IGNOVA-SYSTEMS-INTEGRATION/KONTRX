#ifndef FREERTOS_TASKS_H
#define FREERTOS_TASKS_H

/**
 * @file    freertos_tasks.h
 * @brief   Kontrx — RTOS Task Initialisation & Relay GPIO Driver
 *
 * Task priorities (higher number = higher priority in FreeRTOS):
 *   5  osPriorityRealtime  — Control Engine  (1ms periodic)
 *   3  osPriorityAboveNormal — Modbus Sensor Polling
 *   2  osPriorityNormal    — HTTP Server
 *   1  osPriorityLow       — OTA Background
 */

#include "cmsis_os2.h"
#include "semphr.h"
#include <stdint.h>

/**
 * @brief  Create all application tasks and launch the FreeRTOS scheduler.
 *         Call this from main() after peripheral initialisation.
 *         Does not return.
 */
void RTOS_Tasks_Init(void);

/**
 * @brief  GPIO-level relay driver.
 *         Reads Gateway_Config_t to determine port/pin and applies NC inversion.
 * @param  idx   0..MAX_RELAYS-1
 * @param  state 1 = activate (ON), 0 = deactivate (OFF)
 */
void Relay_SetState(uint8_t idx, uint8_t state);

/**
 * @brief  Initialise relay GPIO pins from current Gateway_Config_t.
 *         Call once after Modbus_DMA_Init().
 */
void Relay_Init(void);

/** @brief Global uptime counter, incremented by vApplicationTickHook */
extern volatile uint32_t g_uptime_seconds;

/** @brief CPU usage 0-100%, updated every second by vApplicationIdleHook */
extern volatile uint8_t g_cpu_usage_pct;

/** @brief Initialise DWT cycle counter for CPU usage measurement. Call before RTOS_Tasks_Init(). */
void KontrxDWT_Init(void);

#define MQTT_LOG_MAX 5

typedef struct {
    char topic[128];
    uint8_t success;
    uint32_t timestamp; /* uptime seconds when sent */
} MqttLogEntry_t;

typedef struct {
    uint8_t connected;
    char active_topic[128];
    MqttLogEntry_t log[MQTT_LOG_MAX];
    uint8_t log_count;
} MqttStatus_t;

#define LOG_BUFFER_SIZE 16384  // 16 KB circular byte buffer

typedef struct {
    uint32_t timestamp;
    uint8_t category_id;  // 0:SYS, 1:MQTT, 2:MODBUS, 3:RELAY, 4:OTA, 0xFF:Padding/Wrap
    uint8_t msg_len;
} LogHeader_t;

extern uint8_t g_log_ring[LOG_BUFFER_SIZE];
extern volatile uint32_t g_log_head;
extern volatile uint32_t g_log_tail;
extern volatile uint32_t g_total_logs_written;
extern volatile uint32_t g_sys_log_count;
extern volatile uint8_t g_sys_log_full;

void Log_Event(const char *category, const char *message);

#include "flash_partition.h"

extern volatile MqttStatus_t g_mqtt_status;

extern osThreadId_t g_tid_modbus;
extern osThreadId_t g_tid_mqtt;

/* W5500 SPI bus mutex — protects concurrent socket access from HTTP + MQTT tasks.
 * Declared as SemaphoreHandle_t so it can be created before osKernelStart(). */
extern SemaphoreHandle_t spiMutex;

extern RuleConfig_t activeRules;
extern RuleConfig_t pendingRules;
extern volatile uint8_t hasPendingRules;
extern osMutexId_t rulesMutex;
extern volatile uint32_t rulesTestTicks;
extern volatile uint8_t rulesTesting;

/**
 * @brief Self-healing monitor for W5500 hardware registers.
 *        Automatically recovers static IP and socket configuration if W5500 registers clear (0.0.0.0).
 */
void Ensure_W5500_Network_Alive(void);

#endif /* FREERTOS_TASKS_H */
