/**
 * @file    rng_stm32.c
 * @brief   STM32F407 hardware RNG (RM0090 section 24).
 */
#include "rng_stm32.h"
#include "stm32f407_regs.h"

#define RNG_BASE_ADDR   0x50060800UL
#define RNG_CR   (*(volatile uint32_t *)(RNG_BASE_ADDR + 0x00U))
#define RNG_SR   (*(volatile uint32_t *)(RNG_BASE_ADDR + 0x04U))
#define RNG_DR   (*(volatile uint32_t *)(RNG_BASE_ADDR + 0x08U))

#define RCC_AHB2ENR_RNGEN  (1U << 6)
#define RNG_CR_RNGEN       (1U << 2)
#define RNG_SR_DRDY        (1U << 0)
#define RNG_SR_CECS        (1U << 1)
#define RNG_SR_SECS        (1U << 2)
#define RNG_SR_SEIS        (1U << 6)

void RNG_Init(void) {
    RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;
    (void)RCC->AHB2ENR;                 /* let the clock enable settle */
    RNG_CR |= RNG_CR_RNGEN;
}

uint8_t RNG_Read32(uint32_t *out) {
    if (!out) return 0;
    for (uint32_t tries = 0; tries < 4; tries++) {
        uint32_t timeout = 100000U;     /* one word takes ~40 PLL48 cycles */
        while (!(RNG_SR & (RNG_SR_DRDY | RNG_SR_CECS | RNG_SR_SECS)) && --timeout) { }
        uint32_t sr = RNG_SR;
        if (sr & RNG_SR_SECS) {
            /* Seed error: clear SEIS and restart the generator (RM0090 24.3.2). */
            RNG_SR &= ~RNG_SR_SEIS;
            RNG_CR &= ~RNG_CR_RNGEN;
            RNG_CR |= RNG_CR_RNGEN;
            continue;
        }
        if ((sr & RNG_SR_CECS) || !(sr & RNG_SR_DRDY)) return 0;
        uint32_t v = RNG_DR;
        if (v == 0U || v == 0xFFFFFFFFU) continue;  /* reject stuck output */
        *out = v;
        return 1;
    }
    return 0;
}
