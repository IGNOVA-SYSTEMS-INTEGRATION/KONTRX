#ifndef TIMER_STM32_H
#define TIMER_STM32_H

#include <stdint.h>
#include "stm32f407_regs.h"

#define TIMER_CH1   1U
#define TIMER_CH2   2U
#define TIMER_CH3   3U
#define TIMER_CH4   4U

void Timer_PWM_Init(TIM_TypeDef *tim, uint8_t channel, uint32_t freq_hz);
void Timer_PWM_SetDuty(TIM_TypeDef *tim, uint8_t channel, float duty_pct);
void Timer_PWM_SetFreq(TIM_TypeDef *tim, uint32_t freq_hz);
void Timer_PWM_Start(TIM_TypeDef *tim, uint8_t channel);
void Timer_PWM_Stop(TIM_TypeDef *tim, uint8_t channel);

void Timer_PTO_Init(TIM_TypeDef *tim, uint8_t channel);
void Timer_PTO_SetFrequency(TIM_TypeDef *tim, uint32_t pps);
void Timer_PTO_Start(TIM_TypeDef *tim);
void Timer_PTO_Stop(TIM_TypeDef *tim);

#endif // TIMER_STM32_H