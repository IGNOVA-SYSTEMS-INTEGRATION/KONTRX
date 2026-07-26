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

#endif /* FREERTOS_TASKS_H */
