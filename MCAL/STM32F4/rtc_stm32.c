/**
 * @file    rtc_stm32.c
 * @brief   STM32F407 Hardware RTC (Real-Time Clock) Driver
 */

#include "rtc_stm32.h"
#include "stm32f407_regs.h"
#include <stdio.h>

#define PWR_BASE_ADDR     (APB1PERIPH_BASE + 0x7000U)
#define RTC_BASE_ADDR     (APB1PERIPH_BASE + 0x2800U)

typedef struct {
    volatile uint32_t CR;
    volatile uint32_t CSR;
} PWR_TypeDef;

typedef struct {
    volatile uint32_t TR;       /* 0x00 Time register */
    volatile uint32_t DR;       /* 0x04 Date register */
    volatile uint32_t CR;       /* 0x08 Control register */
    volatile uint32_t ISR;      /* 0x0C Initialization and Status register */
    volatile uint32_t PRER;     /* 0x10 Prescaler register */
    volatile uint32_t WUTR;     /* 0x14 Wakeup timer register */
    volatile uint32_t CALIBR;   /* 0x18 Calibration register */
    volatile uint32_t ALRMAR;   /* 0x1C Alarm A register */
    volatile uint32_t ALRMBR;   /* 0x20 Alarm B register */
    volatile uint32_t WPR;      /* 0x24 Write protection register */
} RTC_TypeDef;

#define PWR   ((PWR_TypeDef *)PWR_BASE_ADDR)
#define RTC   ((RTC_TypeDef *)RTC_BASE_ADDR)

#define RCC_BDCR   (*((volatile uint32_t *)(RCC_BASE + 0x70U)))
#define RCC_CSR    (*((volatile uint32_t *)(RCC_BASE + 0x74U)))

void RTC_Init(void) {
    /* 1. Enable PWR clock in APB1ENR (bit 28) */
    RCC->APB1ENR |= (1U << 28);

    /* 2. Allow access to Backup Domain (DBP bit in PWR_CR) */
    PWR->CR |= (1U << 8);

    /* Check if RTC is already running */
    if ((RCC_BDCR & (1U << 15)) && (RTC->ISR & (1U << 4))) {
        return; /* Already initialized and running */
    }

    /* 3. Try LSE (32.768 kHz) */
    RCC_BDCR |= (1U << 0); /* LSEON */

    uint32_t timeout = 200000U;
    while (!(RCC_BDCR & (1U << 1)) && --timeout);

    if (RCC_BDCR & (1U << 1)) {
        /* LSE ready -> Select LSE */
        RCC_BDCR &= ~(3U << 8);
        RCC_BDCR |=  (1U << 8);
        RCC_BDCR |=  (1U << 15); /* RTCEN */

        /* Unlock RTC */
        RTC->WPR = 0xCA;
        RTC->WPR = 0x53;

        /* Enter Init mode */
        RTC->ISR |= (1U << 7);
        timeout = 100000U;
        while (!(RTC->ISR & (1U << 6)) && --timeout);

        /* Asynch 127, Synch 255 for 32.768kHz -> 1 Hz */
        RTC->PRER = (127U << 16) | 255U;
        RTC->TR   = 0x00000000U; /* 00:00:00 */
        RTC->DR   = (26U << 16) | (7U << 8) | 25U;

        /* Exit Init mode */
        RTC->ISR &= ~(1U << 7);
        RTC->WPR = 0xFF;
    } else {
        /* LSE timed out -> Fallback to LSI (~32kHz internal RC) */
        RCC_BDCR &= ~(1U << 0);

        RCC_CSR |= (1U << 0); /* LSION */
        timeout = 100000U;
        while (!(RCC_CSR & (1U << 1)) && --timeout);

        /* Select LSI */
        RCC_BDCR &= ~(3U << 8);
        RCC_BDCR |=  (2U << 8);
        RCC_BDCR |=  (1U << 15); /* RTCEN */

        /* Unlock RTC */
        RTC->WPR = 0xCA;
        RTC->WPR = 0x53;

        /* Enter Init mode */
        RTC->ISR |= (1U << 7);
        timeout = 100000U;
        while (!(RTC->ISR & (1U << 6)) && --timeout);

        /* Asynch 127, Synch 249 for ~32kHz -> 1 Hz */
        RTC->PRER = (127U << 16) | 249U;
        RTC->TR   = 0x00000000U; /* 00:00:00 */
        RTC->DR   = (26U << 16) | (7U << 8) | 25U;

        /* Exit Init mode */
        RTC->ISR &= ~(1U << 7);
        RTC->WPR = 0xFF;
    }
}

void RTC_GetTime(uint8_t *hours, uint8_t *minutes, uint8_t *seconds) {
    uint32_t tr = RTC->TR;
    uint32_t dr = RTC->DR; /* Reading DR unlocks shadow registers */
    (void)dr;

    uint8_t s = (uint8_t)(((tr >> 4) & 0x07) * 10 + (tr & 0x0F));
    uint8_t m = (uint8_t)(((tr >> 12) & 0x07) * 10 + ((tr >> 8) & 0x0F));
    uint8_t h = (uint8_t)(((tr >> 20) & 0x03) * 10 + ((tr >> 16) & 0x0F));

    if (hours)   *hours   = h;
    if (minutes) *minutes = m;
    if (seconds) *seconds = s;
}

uint32_t RTC_GetUptimeSeconds(void) {
    uint8_t h, m, s;
    RTC_GetTime(&h, &m, &s);
    return (uint32_t)h * 3600U + (uint32_t)m * 60U + (uint32_t)s;
}

void RTC_GetTimeString(char *buf, uint32_t buflen) {
    uint8_t h, m, s;
    RTC_GetTime(&h, &m, &s);
    snprintf(buf, buflen, "%02u:%02u:%02u", h, m, s);
}
