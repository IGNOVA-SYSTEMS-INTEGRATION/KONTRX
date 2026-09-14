/**
 * @file    exti_stm32.c
 * @brief   STM32F407 Real-Time EXTI Limit Switch Interrupt Driver
 */

#include "exti_stm32.h"
#include "stm32f407_regs.h"

#define NVIC_ISER0   (*(volatile uint32_t *)0xE000E100U)
#define NVIC_IPR     ((volatile uint8_t *)0xE000E400U)

static LimitSwitch_Callback_t s_limit_cb = 0;

void EXTI_LimitSwitches_Init(void) {
    // 1. Enable GPIOE clock & SYSCFG clock
    RCC->AHB1ENR |= (1U << 4);  // GPIOE
    RCC->APB2ENR |= (1U << 14); // SYSCFG

    // 2. Configure PE0, PE1, PE3, PE7 as Input with Pull-Up
    // Mode = 00 (Input)
    GPIOE->MODER &= ~((3U << (0 * 2)) | (3U << (1 * 2)) | (3U << (3 * 2)) | (3U << (7 * 2)));
    // PUPDR = 01 (Pull-up)
    GPIOE->PUPDR &= ~((3U << (0 * 2)) | (3U << (1 * 2)) | (3U << (3 * 2)) | (3U << (7 * 2)));
    GPIOE->PUPDR |=  ((1U << (0 * 2)) | (1U << (1 * 2)) | (1U << (3 * 2)) | (1U << (7 * 2)));

    // 3. Connect EXTI lines to Port E (value 0b0100 = 4)
    // EXTICR[0]: Line 0 [3:0], Line 1 [7:4], Line 3 [15:12]
    SYSCFG->EXTICR[0] &= ~((0xFU << 0) | (0xFU << 4) | (0xFU << 12));
    SYSCFG->EXTICR[0] |=  ((4U << 0)   | (4U << 4)   | (4U << 12));

    // EXTICR[1]: Line 7 [15:12]
    SYSCFG->EXTICR[1] &= ~(0xFU << 12);
    SYSCFG->EXTICR[1] |=  (4U << 12);

    // 4. Configure Triggers: Both Falling (switch hit) and Rising (switch released)
    EXTI->FTSR |= (1U << 0) | (1U << 1) | (1U << 3) | (1U << 7);
    EXTI->RTSR |= (1U << 0) | (1U << 1) | (1U << 3) | (1U << 7);

    // 5. Unmask Interrupts in IMR
    EXTI->IMR |= (1U << 0) | (1U << 1) | (1U << 3) | (1U << 7);

    // 6. Set NVIC Priorities to 5 (highest allowed for FreeRTOS syscalls)
    NVIC_IPR[6]  = (5U << 4); // EXTI0 IRQ
    NVIC_IPR[7]  = (5U << 4); // EXTI1 IRQ
    NVIC_IPR[9]  = (5U << 4); // EXTI3 IRQ
    NVIC_IPR[23] = (5U << 4); // EXTI9_5 IRQ

    // 7. Enable NVIC Interrupts
    NVIC_ISER0 |= (1U << 6) | (1U << 7) | (1U << 9) | (1U << 23);
}

void EXTI_RegisterLimitSwitchCallback(LimitSwitch_Callback_t cb) {
    s_limit_cb = cb;
}

uint8_t EXTI_GetLimitSwitchState(uint8_t channel) {
    // Active low (switch to GND pulls down pin)
    uint32_t idr = GPIOE->IDR;
    switch (channel) {
        case 0: return !(idr & (1U << 0)); // PE0
        case 1: return !(idr & (1U << 1)); // PE1
        case 2: return !(idr & (1U << 3)); // PE3
        case 3: return !(idr & (1U << 7)); // PE7
        default: return 0;
    }
}

// Override EXTI ISR handlers
void EXTI0_IRQHandler(void) {
    EXTI->PR = (1U << 0);
    if (s_limit_cb) s_limit_cb(0);
}

void EXTI1_IRQHandler(void) {
    EXTI->PR = (1U << 1);
    if (s_limit_cb) s_limit_cb(1);
}

void EXTI3_IRQHandler(void) {
    EXTI->PR = (1U << 3);
    if (s_limit_cb) s_limit_cb(2);
}

void EXTI9_5_IRQHandler(void) {
    if (EXTI->PR & (1U << 7)) {
        EXTI->PR = (1U << 7);
        if (s_limit_cb) s_limit_cb(3);
    }
}