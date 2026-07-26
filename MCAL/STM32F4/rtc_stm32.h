#ifndef RTC_STM32_H
#define RTC_STM32_H

#include <stdint.h>

void RTC_Init(void);
void RTC_GetTime(uint8_t *hours, uint8_t *minutes, uint8_t *seconds);
uint32_t RTC_GetUptimeSeconds(void);
void RTC_GetTimeString(char *buf, uint32_t buflen);

#endif /* RTC_STM32_H */
