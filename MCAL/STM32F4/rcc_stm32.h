#ifndef RCC_STM32_H
#define RCC_STM32_H

#include <stdint.h>

/**
 * @brief  Configures the system clock to 168 MHz (maximum for STM32F407).
 *         Attempts HSE (8 MHz external crystal) first. If HSE does not start
 *         within a timeout window, falls back seamlessly to HSI (16 MHz RC).
 *         Configures AHB (/1 = 168MHz), APB1 (/4 = 42MHz), APB2 (/2 = 84MHz),
 *         and Flash latency to 5 Wait States with prefetch & caches enabled.
 */
void RCC_SystemClock_168MHz_Init(void);

uint32_t RCC_GetSysClockFreq(void);
uint32_t RCC_GetHCLKFreq(void);
uint32_t RCC_GetPCLK1Freq(void);
uint32_t RCC_GetPCLK2Freq(void);
uint32_t RCC_GetTimerPCLK1Freq(void);
uint32_t RCC_GetTimerPCLK2Freq(void);
uint8_t  RCC_IsHSEUsed(void);

#endif // RCC_STM32_H
