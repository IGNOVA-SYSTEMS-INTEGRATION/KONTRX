/**
 * @file    pwm_controller.c
 * @brief   Kontrx Hardware PWM Controller Abstraction
 */

#include "pwm_controller.h"
#include "timer_stm32.h"
#include "stm32f407_regs.h"

static PWM_Channel_Info_t s_pwm_channels[MAX_PWM_CHANNELS] = {
    {0, 1, 0.0f, 1000U, "PD12", "TIM4_CH1"},
    {1, 1, 0.0f, 1000U, "PD13", "TIM4_CH2"},
    {2, 1, 0.0f, 1000U, "PD14", "TIM4_CH3"},
    {3, 1, 0.0f, 1000U, "PD15", "TIM4_CH4"},
    {4, 1, 0.0f, 1000U, "PB8",  "TIM10_CH1"},
    {5, 1, 0.0f, 1000U, "PB9",  "TIM11_CH1"},
    {6, 1, 0.0f, 1000U, "PA7",  "TIM14_CH1"}
};

void PWM_Controller_Init(void) {
    // 1. Enable Clocks for GPIOD, GPIOB, GPIOA
    RCC->AHB1ENR |= (1U << 3) | (1U << 1) | (1U << 0);

    // 2. Configure PD12, PD13, PD14, PD15 as Alternate Function AF2 (TIM4)
    // MODER = 10 (AF)
    GPIOD->MODER &= ~((3U << (12*2)) | (3U << (13*2)) | (3U << (14*2)) | (3U << (15*2)));
    GPIOD->MODER |=  ((2U << (12*2)) | (2U << (13*2)) | (2U << (14*2)) | (2U << (15*2)));
    // OSPEEDR = 10 (High speed)
    GPIOD->OSPEEDR |= ((2U << (12*2)) | (2U << (13*2)) | (2U << (14*2)) | (2U << (15*2)));
    // AFR[1] = AF2 (value 2) for pins 12, 13, 14, 15
    GPIOD->AFR[1] &= ~((0xFU << (4*4)) | (0xFU << (5*4)) | (0xFU << (6*4)) | (0xFU << (7*4)));
    GPIOD->AFR[1] |=  ((2U    << (4*4)) | (2U    << (5*4)) | (2U    << (6*4)) | (2U    << (7*4)));

    // 3. Configure PB8, PB9 as Alternate Function AF3 (TIM10, TIM11)
    GPIOB->MODER &= ~((3U << (8*2)) | (3U << (9*2)));
    GPIOB->MODER |=  ((2U << (8*2)) | (2U << (9*2)));
    GPIOB->OSPEEDR |= ((2U << (8*2)) | (2U << (9*2)));
    GPIOB->AFR[1] &= ~((0xFU << (0*4)) | (0xFU << (1*4)));
    GPIOB->AFR[1] |=  ((3U    << (0*4)) | (3U    << (1*4)));

    // 4. Configure PA7 as Alternate Function AF9 (TIM14)
    GPIOA->MODER &= ~(3U << (7*2));
    GPIOA->MODER |=  (2U << (7*2));
    GPIOA->OSPEEDR |= (2U << (7*2));
    GPIOA->AFR[0] &= ~(0xFU << (7*4));
    GPIOA->AFR[0] |=  (9U    << (7*4));

    // 5. Initialize Timer PWM channels
    Timer_PWM_Init(TIM4,  TIMER_CH1, 1000U);
    Timer_PWM_Init(TIM4,  TIMER_CH2, 1000U);
    Timer_PWM_Init(TIM4,  TIMER_CH3, 1000U);
    Timer_PWM_Init(TIM4,  TIMER_CH4, 1000U);
    Timer_PWM_Init(TIM10, TIMER_CH1, 1000U);
    Timer_PWM_Init(TIM11, TIMER_CH1, 1000U);
    Timer_PWM_Init(TIM14, TIMER_CH1, 1000U);
}

void PWM_SetDuty(uint8_t channel, float duty_pct) {
    if (channel >= MAX_PWM_CHANNELS) return;
    s_pwm_channels[channel].duty_pct = duty_pct;

    switch (channel) {
        case 0: Timer_PWM_SetDuty(TIM4,  TIMER_CH1, duty_pct); break;
        case 1: Timer_PWM_SetDuty(TIM4,  TIMER_CH2, duty_pct); break;
        case 2: Timer_PWM_SetDuty(TIM4,  TIMER_CH3, duty_pct); break;
        case 3: Timer_PWM_SetDuty(TIM4,  TIMER_CH4, duty_pct); break;
        case 4: Timer_PWM_SetDuty(TIM10, TIMER_CH1, duty_pct); break;
        case 5: Timer_PWM_SetDuty(TIM11, TIMER_CH1, duty_pct); break;
        case 6: Timer_PWM_SetDuty(TIM14, TIMER_CH1, duty_pct); break;
    }
}

void PWM_SetFrequency(uint8_t channel, uint32_t freq_hz) {
    if (channel >= MAX_PWM_CHANNELS || freq_hz == 0) return;
    s_pwm_channels[channel].freq_hz = freq_hz;

    switch (channel) {
        case 0:
        case 1:
        case 2:
        case 3:
            Timer_PWM_SetFreq(TIM4, freq_hz);
            // Reapply current duties on new ARR
            Timer_PWM_SetDuty(TIM4, TIMER_CH1, s_pwm_channels[0].duty_pct);
            Timer_PWM_SetDuty(TIM4, TIMER_CH2, s_pwm_channels[1].duty_pct);
            Timer_PWM_SetDuty(TIM4, TIMER_CH3, s_pwm_channels[2].duty_pct);
            Timer_PWM_SetDuty(TIM4, TIMER_CH4, s_pwm_channels[3].duty_pct);
            break;
        case 4:
            Timer_PWM_SetFreq(TIM10, freq_hz);
            Timer_PWM_SetDuty(TIM10, TIMER_CH1, s_pwm_channels[4].duty_pct);
            break;
        case 5:
            Timer_PWM_SetFreq(TIM11, freq_hz);
            Timer_PWM_SetDuty(TIM11, TIMER_CH1, s_pwm_channels[5].duty_pct);
            break;
        case 6:
            Timer_PWM_SetFreq(TIM14, freq_hz);
            Timer_PWM_SetDuty(TIM14, TIMER_CH1, s_pwm_channels[6].duty_pct);
            break;
    }
}

const PWM_Channel_Info_t* PWM_GetChannelInfo(uint8_t channel) {
    if (channel < MAX_PWM_CHANNELS) return &s_pwm_channels[channel];
    return 0;
}

uint8_t PWM_GetChannelCount(void) {
    return MAX_PWM_CHANNELS;
}