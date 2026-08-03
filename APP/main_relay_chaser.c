/**
 * @file    main_relay_chaser.c
 * @brief   Relay Chaser / Knight-Rider Effect
 *
 * Relay sequence (in order):
 *   [0] PE2   [1] PE4   [2] PE6
 *   [3] PC0   [4] PC2
 *   [5] PA0   [6] PA2   [7] PA4
 *   [8] PC4
 *
 * Behaviour:
 *   Forward  : Turn ON  each relay one-by-one (PE2 → PC4)
 *   Backward : Turn OFF each relay one-by-one (PC4 → PE2)
 *   Repeat forever.
 *
 * Board : STM32F407VET6 (Kontrx)
 */

#include "gpio_stm32.h"
#include "uart_stm32.h"
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Debug printf redirect → USART1                                     */
/* ------------------------------------------------------------------ */
int _write(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) {
        UART_Debug_SendByte((uint8_t)ptr[i]);
    }
    return len;
}

/* ------------------------------------------------------------------ */
/*  Relay descriptor                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    GPIO_TypeDef *port;
    uint8_t       pin;
    const char   *name;
} Relay_t;

/* ------------------------------------------------------------------ */
/*  Relay table  — order defines the chaser sequence                   */
/* ------------------------------------------------------------------ */
static const Relay_t RELAYS[] = {
    { GPIOE, 2, "PE2" },   /* relay 0 */
    { GPIOE, 4, "PE4" },   /* relay 1 */
    { GPIOE, 6, "PE6" },   /* relay 2 */
    { GPIOC, 0, "PC0" },   /* relay 3 */
    { GPIOC, 2, "PC2" },   /* relay 4 */
    { GPIOA, 0, "PA0" },   /* relay 5 */
    { GPIOA, 2, "PA2" },   /* relay 6 */
    { GPIOA, 4, "PA4" },   /* relay 7 */
    { GPIOC, 4, "PC4" },   /* relay 8 */
};

#define NUM_RELAYS  (sizeof(RELAYS) / sizeof(RELAYS[0]))

/* Delay between each relay step (milliseconds) */
#define STEP_DELAY_MS   30U

/* ------------------------------------------------------------------ */
/*  Helpers to init clocks for all ports used                          */
/* ------------------------------------------------------------------ */
static void Relay_EnableClocks(void) {
    /* GPIOA – bit 0 */
    RCC->AHB1ENR |= (1U << 0);
    /* GPIOC – bit 2 */
    RCC->AHB1ENR |= (1U << 2);
    /* GPIOE – bit 4 */
    RCC->AHB1ENR |= (1U << 4);
}

/* ------------------------------------------------------------------ */
/*  Generic GPIO output init (full ports supported)                    */
/* ------------------------------------------------------------------ */
static void Relay_InitPin(GPIO_TypeDef *port, uint8_t pin) {
    /* MODER: 01 = General Purpose Output */
    port->MODER &= ~(3U << (pin * 2));
    port->MODER |=  (1U << (pin * 2));

    /* OTYPER: 0 = Push-Pull */
    port->OTYPER &= ~(1U << pin);

    /* OSPEEDR: 10 = High speed */
    port->OSPEEDR &= ~(3U << (pin * 2));
    port->OSPEEDR |=  (2U << (pin * 2));

    /* PUPDR: 00 = No pull */
    port->PUPDR &= ~(3U << (pin * 2));
}

/* ------------------------------------------------------------------ */
/*  Relay ON / OFF primitives                                           */
/* ------------------------------------------------------------------ */
static inline void Relay_Set(const Relay_t *r, uint8_t state) {
    if (state) {
        r->port->BSRR = (1U << r->pin);          /* Set   HIGH → relay ON  */
    } else {
        r->port->BSRR = (1U << (r->pin + 16));   /* Reset LOW  → relay OFF */
    }
}

/* ================================================================== */
/*  ACCURATE DELAY — SysTick (1 ms per tick)                          */
/* ================================================================== */

/*
 * Change SYSCLOCK_HZ to match your actual CPU clock:
 *   16000000UL  → 16 MHz  (HSI default, no PLL)
 *   84000000UL  → 84 MHz  (common PLL config)
 *  168000000UL  → 168 MHz (max for STM32F407)
 */
#define SYSCLOCK_HZ   16000000UL

/* Volatile tick counter — incremented by SysTick_Handler every 1 ms */
static volatile uint32_t g_tick_ms = 0;

/* SysTick ISR — overrides the weak stub in startup file */
void SysTick_Handler(void) {
    g_tick_ms++;
}

/* Init SysTick for 1 ms interrupts */
static void SysTick_Init(void) {
    /* Reload value = (clock / 1000) - 1  →  fires every 1 ms */
    SysTick->LOAD = (SYSCLOCK_HZ / 1000U) - 1U;

    /* Clear current value */
    SysTick->VAL  = 0U;

    /*
     * CTRL bits:
     *   [2] CLKSOURCE = 1  → use processor clock (AHB)
     *   [1] TICKINT   = 1  → generate exception on reaching 0
     *   [0] ENABLE    = 1  → counter enabled
     */
    SysTick->CTRL = (1U << 2) | (1U << 1) | (1U << 0);
}

/* Accurate blocking delay — uses SysTick 1 ms ticks */
static void Delay_ms(uint32_t ms) {
    uint32_t start = g_tick_ms;
    while ((g_tick_ms - start) < ms) {
        /* Wait — SysTick ISR updates g_tick_ms every 1 ms */
        __asm__("nop");
    }
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */
int main(void) {

    /* 1. Init SysTick FIRST — gives us accurate timing immediately */
    SysTick_Init();

    /* 2. Init Debug UART */
    GPIO_Init_USART1_Pins();
    UART_Debug_Init();

    /* 3. Enable relay port clocks */
    Relay_EnableClocks();

    /* 4. Configure all relay pins as outputs, start LOW (relay OFF) */
    for (uint8_t i = 0; i < NUM_RELAYS; i++) {
        Relay_InitPin(RELAYS[i].port, RELAYS[i].pin);
        Relay_Set(&RELAYS[i], 0);   /* OFF at startup */
    }

    printf("\r\n=== Relay Chaser Started ===\r\n");
    printf("Clock: %lu Hz | Step delay: %u ms\r\n",
           (unsigned long)SYSCLOCK_HZ, (unsigned)STEP_DELAY_MS);
    printf("Sequence: PE2->PE4->PE6->PC0->PC2->PA0->PA2->PA4->PC4\r\n\r\n");

    uint32_t cycle = 0;

    while (1) {
        cycle++;
        printf("--- Cycle %lu ---\r\n", (unsigned long)cycle);

        /* ---------------------------------------------------------- */
        /*  FORWARD: Turn ON relays one-by-one  (index 0 → N-1)       */
        /* ---------------------------------------------------------- */
        printf("  [FWD] ON : ");
        for (uint8_t i = 0; i < NUM_RELAYS; i++) {
            Relay_Set(&RELAYS[i], 1);
            printf("%s ", RELAYS[i].name);
            Delay_ms(STEP_DELAY_MS);
        }
        printf("\r\n");

        /* Brief pause — all ON */
        Delay_ms(STEP_DELAY_MS);

        /* ---------------------------------------------------------- */
        /*  BACKWARD: Turn OFF relays one-by-one  (index N-1 → 0)     */
        /* ---------------------------------------------------------- */
        printf("  [BWD] OFF: ");
        for (int8_t i = (int8_t)(NUM_RELAYS - 1); i >= 0; i--) {
            Relay_Set(&RELAYS[i], 0);
            printf("%s ", RELAYS[i].name);
            Delay_ms(STEP_DELAY_MS);
        }
        printf("\r\n");

        /* Brief pause — all OFF */
        Delay_ms(STEP_DELAY_MS);
    }

    return 0;  /* never reached */
}
