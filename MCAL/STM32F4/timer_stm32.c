/**
 * @file    timer_stm32.c
 * @brief   STM32F407 Hardware Timer Driver (PWM & PTO Pulse Train Output)
 */

#include "timer_stm32.h"
#include "rcc_stm32.h"

static uint32_t GetTimerClock(TIM_TypeDef *tim) {
    if (tim == TIM1 || tim == TIM9 || tim == TIM10 || tim == TIM11) {
        return RCC_GetTimerPCLK2Freq(); // 168 MHz
    } else {
        return RCC_GetTimerPCLK1Freq(); // 84 MHz
    }
}

static void EnableTimerClock(TIM_TypeDef *tim) {
    if (tim == TIM1)  RCC->APB2ENR |= (1U << 0);
    else if (tim == TIM2)  RCC->APB1ENR |= (1U << 0);
    else if (tim == TIM3)  RCC->APB1ENR |= (1U << 1);
    else if (tim == TIM4)  RCC->APB1ENR |= (1U << 2);
    else if (tim == TIM9)  RCC->APB2ENR |= (1U << 16);
    else if (tim == TIM10) RCC->APB2ENR |= (1U << 17);
    else if (tim == TIM11) RCC->APB2ENR |= (1U << 18);
    else if (tim == TIM14) RCC->APB1ENR |= (1U << 8);
}

void Timer_PWM_Init(TIM_TypeDef *tim, uint8_t channel, uint32_t freq_hz) {
    if (!tim || channel < 1 || channel > 4 || freq_hz == 0) return;

    EnableTimerClock(tim);

    // Calculate Prescaler & ARR for ~1 MHz internal counter frequency
    uint32_t tim_clk = GetTimerClock(tim);
    uint32_t psc = (tim_clk / 1000000U) - 1U;
    uint32_t arr = (1000000U / freq_hz) - 1U;
    if (arr < 1) arr = 1;

    tim->CR1 &= ~(1U << 0); // Disable timer during config
    tim->PSC = psc;
    tim->ARR = arr;
    tim->CR1 |= (1U << 7);  // ARPE: Auto-reload preload enable

    // Configure Channel PWM Mode 1 (OCxM = 110) + Preload enable (OCxPE = 1)
    if (channel == TIMER_CH1) {
        tim->CCMR1 &= ~0xFFU;
        tim->CCMR1 |= (6U << 4) | (1U << 3); // PWM mode 1 + Preload
        tim->CCR1 = 0;
        tim->CCER |= (1U << 0); // CC1E: Output Enable
    } else if (channel == TIMER_CH2) {
        tim->CCMR1 &= ~(0xFFU << 8);
        tim->CCMR1 |= ((6U << 4) | (1U << 3)) << 8;
        tim->CCR2 = 0;
        tim->CCER |= (1U << 4); // CC2E
    } else if (channel == TIMER_CH3) {
        tim->CCMR2 &= ~0xFFU;
        tim->CCMR2 |= (6U << 4) | (1U << 3);
        tim->CCR3 = 0;
        tim->CCER |= (1U << 8); // CC3E
    } else if (channel == TIMER_CH4) {
        tim->CCMR2 &= ~(0xFFU << 8);
        tim->CCMR2 |= ((6U << 4) | (1U << 3)) << 8;
        tim->CCR4 = 0;
        tim->CCER |= (1U << 12); // CC4E
    }

    if (tim == TIM1) {
        tim->BDTR |= (1U << 15); // MOE: Main Output Enable for TIM1
    }

    tim->EGR = (1U << 0); // UG: Re-initialize counter & registers
    tim->CR1 |= (1U << 0); // CEN: Enable counter
}

void Timer_PWM_SetDuty(TIM_TypeDef *tim, uint8_t channel, float duty_pct) {
    if (!tim || channel < 1 || channel > 4) return;
    if (duty_pct < 0.0f) duty_pct = 0.0f;
    if (duty_pct > 100.0f) duty_pct = 100.0f;

    uint32_t arr = tim->ARR;
    uint32_t ccr = (uint32_t)((float)arr * (duty_pct / 100.0f) + 0.5f);

    if (channel == TIMER_CH1) tim->CCR1 = ccr;
    else if (channel == TIMER_CH2) tim->CCR2 = ccr;
    else if (channel == TIMER_CH3) tim->CCR3 = ccr;
    else if (channel == TIMER_CH4) tim->CCR4 = ccr;
}

void Timer_PWM_SetFreq(TIM_TypeDef *tim, uint32_t freq_hz) {
    if (!tim || freq_hz == 0) return;
    uint32_t arr = (1000000U / freq_hz) - 1U;
    if (arr < 1) arr = 1;
    tim->ARR = arr;
}

void Timer_PWM_Start(TIM_TypeDef *tim, uint8_t channel) {
    if (!tim) return;
    if (channel == TIMER_CH1) tim->CCER |= (1U << 0);
    else if (channel == TIMER_CH2) tim->CCER |= (1U << 4);
    else if (channel == TIMER_CH3) tim->CCER |= (1U << 8);
    else if (channel == TIMER_CH4) tim->CCER |= (1U << 12);
    tim->CR1 |= (1U << 0);
}

void Timer_PWM_Stop(TIM_TypeDef *tim, uint8_t channel) {
    if (!tim) return;
    if (channel == TIMER_CH1) tim->CCER &= ~(1U << 0);
    else if (channel == TIMER_CH2) tim->CCER &= ~(1U << 4);
    else if (channel == TIMER_CH3) tim->CCER &= ~(1U << 8);
    else if (channel == TIMER_CH4) tim->CCER &= ~(1U << 12);
}

void Timer_PTO_Init(TIM_TypeDef *tim, uint8_t channel) {
    if (!tim) return;
    EnableTimerClock(tim);

    uint32_t tim_clk = GetTimerClock(tim);
    uint32_t psc = (tim_clk / 1000000U) - 1U; // 1 MHz timebase (1 us resolution)

    tim->CR1 &= ~(1U << 0);
    tim->PSC = psc;
    tim->ARR = 1000U - 1U; // Default 1000 Hz = 1000 pps
    tim->CR1 |= (1U << 7);

    // PWM Mode 1 with 50% duty cycle for square wave pulse
    if (channel == TIMER_CH1) {
        tim->CCMR1 &= ~0xFFU;
        tim->CCMR1 |= (6U << 4) | (1U << 3);
        tim->CCR1 = tim->ARR / 2U;
        tim->CCER |= (1U << 0);
    } else if (channel == TIMER_CH2) {
        tim->CCMR1 &= ~(0xFFU << 8);
        tim->CCMR1 |= ((6U << 4) | (1U << 3)) << 8;
        tim->CCR2 = tim->ARR / 2U;
        tim->CCER |= (1U << 4);
    } else if (channel == TIMER_CH3) {
        tim->CCMR2 &= ~0xFFU;
        tim->CCMR2 |= (6U << 4) | (1U << 3);
        tim->CCR3 = tim->ARR / 2U;
        tim->CCER |= (1U << 8);
    } else if (channel == TIMER_CH4) {
        tim->CCMR2 &= ~(0xFFU << 8);
        tim->CCMR2 |= ((6U << 4) | (1U << 3)) << 8;
        tim->CCR4 = tim->ARR / 2U;
        tim->CCER |= (1U << 12);
    }

    if (tim == TIM1) {
        tim->BDTR |= (1U << 15); // MOE
    }

    // Enable Update Interrupt to count steps
    tim->DIER |= (1U << 0); // UIE: Update interrupt enable
    tim->EGR = (1U << 0);
}

void Timer_PTO_SetFrequency(TIM_TypeDef *tim, uint32_t pps) {
    if (!tim || pps == 0) return;
    if (pps > 100000U) pps = 100000U; // Max 100 kHz pulse rate

    uint32_t arr = (1000000U / pps) - 1U;
    if (arr < 1) arr = 1;
    tim->ARR = arr;
    tim->CCR1 = arr / 2U;
    tim->CCR2 = arr / 2U;
    tim->CCR3 = arr / 2U;
    tim->CCR4 = arr / 2U;
}

void Timer_PTO_Start(TIM_TypeDef *tim) {
    if (!tim) return;
    tim->EGR = (1U << 0); // Reset counter
    tim->CR1 |= (1U << 0); // CEN
}

void Timer_PTO_Stop(TIM_TypeDef *tim) {
    if (!tim) return;
    tim->CR1 &= ~(1U << 0); // CEN = 0
}