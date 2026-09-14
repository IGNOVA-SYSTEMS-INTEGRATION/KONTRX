/**
 * @file    rcc_stm32.c
 * @brief   STM32F407 High-Performance 168 MHz Clock Configuration
-----------------------------------------------------------------------------*/

#include "rcc_stm32.h"
#include "stm32f407_regs.h"

static uint32_t s_sysclk = 16000000UL;
static uint32_t s_hclk   = 16000000UL;
static uint32_t s_pclk1  = 16000000UL;
static uint32_t s_pclk2  = 16000000UL;
static uint8_t  s_hse_active = 0;

void RCC_SystemClock_168MHz_Init(void) {
    /* 1. Ensure HSI is enabled and stable */
    RCC->CR |= (1U << 0);
    while (!(RCC->CR & (1U << 1)));

    /* 2. Enable Power controller clock */
    RCC->APB1ENR |= (1U << 28);

    /* 3. Voltage scaling Scale 1 mode (required for >144 MHz, up to 168 MHz) */
    PWR->CR |= (1U << 14);


    /* 4. Attempt to start HSE (8 MHz external crystal) */
    RCC->CR |= (1U << 16);
    volatile uint32_t hse_counter = 0;
    uint8_t hse_ready = 0;

    for (hse_counter = 0; hse_counter < 0x8000; hse_counter++) {
        if (RCC->CR & (1U << 17)) {
            hse_ready = 1;
            break;
        }
    }


    uint32_t pllm, plln, pllp, pllq, pll_source;

    if (hse_ready) {
        /* HSE = 8 MHz:
         * VCO input = 8 MHz / 8 = 1 MHz (PLLM = 8)
         * VCO output = 1 MHz * 336 = 336 MHz (PLLN = 336)
         * PLLCLK = 336 MHz / 2 = 168 MHz (PLLP=0 -> /2)
         * 48CK = 336 MHz / 7 = 48 MHz (PLLQ = 7)
         */
        pllm = 8;
        plln = 336;
        pllp = 0; // /2
        pllq = 7;
        pll_source = (1U << 22); // HSE
        s_hse_active = 1;
    } else {
        /* Fallback to internal HSI = 16 MHz:
         * VCO input = 16 MHz / 16 = 1 MHz (PLLM = 16)
         * VCO output = 1 MHz * 336 = 336 MHz (PLLN = 336)
         * PLLCLK = 336 MHz / 2 = 168 MHz (PLLP=0 -> /2)
         * 48CK = 336 MHz / 7 = 48 MHz (PLLQ = 7)
         */
        RCC->CR &= ~(1U << 16); // Turn off HSE
        pllm = 16;
        plln = 336;
        pllp = 0; // /2
        pllq = 7;
        pll_source = 0; // HSI
        s_hse_active = 0;
    }

    /* 5. Configure Flash: 5 Wait States + Prefetch Buffer + Instruction & Data Caches */
    FLASH_CTRL->ACR = (5U << 0)   |
                      (1U << 8)   |
                      (1U << 9)   |
                      (1U << 10);


    /* 6. Configure Bus Dividers in CFGR:
     *    AHB  = SYSCLK / 1 = 168 MHz (HPRE = 0xxx)
     *    APB1 = HCLK / 4   = 42 MHz  (PPRE1 = 101 -> /4)
     *    APB2 = HCLK / 2   = 84 MHz  (PPRE2 = 100 -> /2)
     */
    RCC->CFGR &= ~((0xFU << 4) | (7U << 10) | (7U << 13));
    RCC->CFGR |=  ((0U << 4) | (5U << 10) | (4U << 13));

    /* 7. Disable main PLL before reconfiguration */
    RCC->CR &= ~(1U << 24);
    while (RCC->CR & (1U << 25));


    /* 8. Write PLL configuration register */
    RCC->PLLCFGR = (pllm << 0) | (plln << 6) | (pllp << 16) | pll_source | (pllq << 24);

    /* 9. Enable PLL and wait for lock */
    RCC->CR |= (1U << 24);
    while (!(RCC->CR & (1U << 25)));

    /* 10. Switch System Clock to PLL */
    RCC->CFGR = (RCC->CFGR & ~3U) | 2U;
    while (((RCC->CFGR >> 2) & 3U) != 2U);

    s_sysclk = 168000000UL;
    s_hclk   = 168000000UL;
    s_pclk1  = 42000000UL;
    s_pclk2  = 84000000UL;
}

uint32_t RCC_GetSysClockFreq(void)   { return s_sysclk; }
uint32_t RCC_GetHCLKFreq(void)       { return s_hclk; }
uint32_t RCC_GetPCLK1Freq(void)      { return s_pclk1; }
uint32_t RCC_GetPCLK2Freq(void)      { return s_pclk2; }
uint32_t RCC_GetTimerPCLK1Freq(void) { return s_pclk1 * 2U; } // 84 MHz
uint32_t RCC_GetTimerPCLK2Freq(void) { return s_pclk2 * 2U; } // 168 MHz
uint8_t  RCC_IsHSEUsed(void)         { return s_hse_active; }
