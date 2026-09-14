/**
 * @file    pto_motion.c
 * @brief   Kontrx Multi-Axis PTO (Pulse Train Output) Motion Controller
 */

#include "pto_motion.h"
#include "timer_stm32.h"
#include "exti_stm32.h"
#include "stm32f407_regs.h"
#include <stdlib.h>

static PTO_Channel_Status_t s_pto[MAX_PTO_CHANNELS] = {
    {0, 0, 0, 1000U, 0, 0, 1, LMT_STATE_CLEAR, "PE9", "PE8", "PE0", "TIM1_CH1"},
    {1, 0, 0, 1000U, 0, 0, 1, LMT_STATE_CLEAR, "PE5", "PE3", "PE1", "TIM9_CH1"},
    {2, 0, 0, 1000U, 0, 0, 1, LMT_STATE_CLEAR, "PC8", "PC9", "PE7", "TIM3_CH3"},
    {3, 0, 0, 1000U, 0, 0, 1, LMT_STATE_CLEAR, "PA3", "PA5", "PB1", "TIM2_CH4"}
};

static void SetDirPin(uint8_t ch, uint8_t dir) {
    s_pto[ch].direction = dir;
    switch (ch) {
        case 0: // PE8
            if (dir) GPIOE->BSRR = (1U << 8);
            else     GPIOE->BSRR = (1U << (8 + 16U));
            break;
        case 1: // PE3
            if (dir) GPIOE->BSRR = (1U << 3);
            else     GPIOE->BSRR = (1U << (3 + 16U));
            break;
        case 2: // PC9
            if (dir) GPIOC->BSRR = (1U << 9);
            else     GPIOC->BSRR = (1U << (9 + 16U));
            break;
        case 3: // PA5
            if (dir) GPIOA->BSRR = (1U << 5);
            else     GPIOA->BSRR = (1U << (5 + 16U));
            break;
    }
}

static void LimitSwitch_Handler(uint8_t ch) {
    if (ch >= MAX_PTO_CHANNELS) return;
    uint8_t state = EXTI_GetLimitSwitchState(ch);
    if (state) {
        // Limit Switch triggered -> Emergency stop!
        PTO_EmergencyStop(ch);
        s_pto[ch].lmt_state = s_pto[ch].direction ? LMT_STATE_HIT_FWD : LMT_STATE_HIT_REV;
    } else {
        s_pto[ch].lmt_state = LMT_STATE_CLEAR;
    }
}

void PTO_Motion_Init(void) {
    // 1. Enable Clocks for GPIOA, GPIOB, GPIOC, GPIOE
    RCC->AHB1ENR |= (1U << 0) | (1U << 1) | (1U << 2) | (1U << 4);

    // 2. Configure Direction Pins as General Purpose Output Push-Pull
    // PE8 (CH1)
    GPIOE->MODER &= ~(3U << (8 * 2));
    GPIOE->MODER |=  (1U << (8 * 2));
    // PE3 (CH2)
    GPIOE->MODER &= ~(3U << (3 * 2));
    GPIOE->MODER |=  (1U << (3 * 2));
    // PC9 (CH3)
    GPIOC->MODER &= ~(3U << (9 * 2));
    GPIOC->MODER |=  (1U << (9 * 2));
    // PA5 (CH4)
    GPIOA->MODER &= ~(3U << (5 * 2));
    GPIOA->MODER |=  (1U << (5 * 2));

    // 3. Configure Pulse Pins as Alternate Function
    // PE9 -> AF1 (TIM1_CH1)
    GPIOE->MODER &= ~(3U << (9 * 2));
    GPIOE->MODER |=  (2U << (9 * 2));
    GPIOE->OSPEEDR |= (2U << (9 * 2));
    GPIOE->AFR[1] &= ~(0xFU << (1 * 4));
    GPIOE->AFR[1] |=  (1U    << (1 * 4));

    // PE5 -> AF3 (TIM9_CH1)
    GPIOE->MODER &= ~(3U << (5 * 2));
    GPIOE->MODER |=  (2U << (5 * 2));
    GPIOE->OSPEEDR |= (2U << (5 * 2));
    GPIOE->AFR[0] &= ~(0xFU << (5 * 4));
    GPIOE->AFR[0] |=  (3U    << (5 * 4));

    // PC8 -> AF2 (TIM3_CH3)
    GPIOC->MODER &= ~(3U << (8 * 2));
    GPIOC->MODER |=  (2U << (8 * 2));
    GPIOC->OSPEEDR |= (2U << (8 * 2));
    GPIOC->AFR[1] &= ~(0xFU << (0 * 4));
    GPIOC->AFR[1] |=  (2U    << (0 * 4));

    // PA3 -> AF1 (TIM2_CH4)
    GPIOA->MODER &= ~(3U << (3 * 2));
    GPIOA->MODER |=  (2U << (3 * 2));
    GPIOA->OSPEEDR |= (2U << (3 * 2));
    GPIOA->AFR[0] &= ~(0xFU << (3 * 4));
    GPIOA->AFR[0] |=  (1U    << (3 * 4));

    // 4. Initialize Hardware Timers
    Timer_PTO_Init(TIM1, TIMER_CH1);
    Timer_PTO_Init(TIM9, TIMER_CH1);
    Timer_PTO_Init(TIM3, TIMER_CH3);
    Timer_PTO_Init(TIM2, TIMER_CH4);

    // 5. Initialize EXTI Limit Switches & Register ISR callback
    EXTI_LimitSwitches_Init();
    EXTI_RegisterLimitSwitchCallback(LimitSwitch_Handler);
}

void PTO_MoveRelative(uint8_t ch, int32_t steps, uint32_t speed_pps) {
    if (ch >= MAX_PTO_CHANNELS || steps == 0 || speed_pps == 0) return;

    uint8_t dir = (steps > 0) ? 1 : 0;

    // Check limit switches: don't allow motion into a triggered limit!
    if (dir && s_pto[ch].lmt_state == LMT_STATE_HIT_FWD) return;
    if (!dir && s_pto[ch].lmt_state == LMT_STATE_HIT_REV) return;

    uint32_t count = (uint32_t)abs(steps);
    SetDirPin(ch, dir);

    s_pto[ch].speed_pps = speed_pps;
    s_pto[ch].target = s_pto[ch].position + steps;
    s_pto[ch].steps_remaining = count;
    s_pto[ch].moving = 1;

    TIM_TypeDef *tim = 0;
    switch (ch) {
        case 0: tim = TIM1; break;
        case 1: tim = TIM9; break;
        case 2: tim = TIM3; break;
        case 3: tim = TIM2; break;
    }

    if (tim) {
        Timer_PTO_SetFrequency(tim, speed_pps);
        Timer_PTO_Start(tim);
    }
}

void PTO_MoveAbsolute(uint8_t ch, int32_t target_pos, uint32_t speed_pps) {
    if (ch >= MAX_PTO_CHANNELS) return;
    int32_t delta = target_pos - s_pto[ch].position;
    PTO_MoveRelative(ch, delta, speed_pps);
}

void PTO_Stop(uint8_t ch) {
    if (ch >= MAX_PTO_CHANNELS) return;
    s_pto[ch].moving = 0;
    s_pto[ch].steps_remaining = 0;

    switch (ch) {
        case 0: Timer_PTO_Stop(TIM1); break;
        case 1: Timer_PTO_Stop(TIM9); break;
        case 2: Timer_PTO_Stop(TIM3); break;
        case 3: Timer_PTO_Stop(TIM2); break;
    }
}

void PTO_EmergencyStop(uint8_t ch) {
    PTO_Stop(ch);
}

void PTO_SetHome(uint8_t ch) {
    if (ch >= MAX_PTO_CHANNELS) return;
    PTO_Stop(ch);
    s_pto[ch].position = 0;
    s_pto[ch].target = 0;
    s_pto[ch].lmt_state = LMT_STATE_CLEAR;
}

const PTO_Channel_Status_t* PTO_GetStatus(uint8_t ch) {
    if (ch < MAX_PTO_CHANNELS) return &s_pto[ch];
    return 0;
}

uint8_t PTO_GetChannelCount(void) {
    return MAX_PTO_CHANNELS;
}

// Step pulse countdown ISR handlers
static void PTO_StepTick(uint8_t ch) {
    if (s_pto[ch].moving && s_pto[ch].steps_remaining > 0) {
        s_pto[ch].steps_remaining--;
        if (s_pto[ch].direction) s_pto[ch].position++;
        else s_pto[ch].position--;

        if (s_pto[ch].steps_remaining == 0) {
            PTO_Stop(ch);
        }
    }
}

void TIM1_UP_TIM10_IRQHandler(void) {
    if (TIM1->SR & (1U << 0)) {
        TIM1->SR &= ~(1U << 0);
        PTO_StepTick(0);
    }
}

void TIM1_BRK_TIM9_IRQHandler(void) {
    if (TIM9->SR & (1U << 0)) {
        TIM9->SR &= ~(1U << 0);
        PTO_StepTick(1);
    }
}

void TIM3_IRQHandler(void) {
    if (TIM3->SR & (1U << 0)) {
        TIM3->SR &= ~(1U << 0);
        PTO_StepTick(2);
    }
}

void TIM2_IRQHandler(void) {
    if (TIM2->SR & (1U << 0)) {
        TIM2->SR &= ~(1U << 0);
        PTO_StepTick(3);
    }
}