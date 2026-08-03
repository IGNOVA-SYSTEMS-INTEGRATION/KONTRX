#include "gpio_stm32.h"
#include "uart_stm32.h"
#include <stdint.h>

/* Simple software delay (Calibrated for 16MHz HSI default clock) */
static void Delay_ms(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 3200; j++) {
            __asm__("nop");
        }
    }
}

/* Custom simple string print to USART6 */
static void UART_PrintStr(const char *str) {
    while (*str) {
        UART_Debug_SendByte((uint8_t)*str);
        str++;
    }
}

int main(void) {
    /* 1. Initialize Debug UART (USART6 PC6/PC7 @ 9600 baud) */
    UART_Debug_Init();

    /* 2. Initialize PE2 as digital output */
    GPIO_InitOutput(GPIOE, 2);

    UART_PrintStr("\r\n========================================\r\n");
    UART_PrintStr("  Lightweight PE2 Blink (bblink22) Active\r\n");
    UART_PrintStr("  No stdio/SysTick dependencies\r\n");
    UART_PrintStr("========================================\r\n");

    uint32_t count = 0;

    while (1) {
        // Toggle PE2
        GPIO_TogglePin(GPIOE, 2);
        count++;

        // Print toggle event
        UART_PrintStr("PE2 Toggled!\r\n");

        // Delay for 2 seconds (2000 ms)
        Delay_ms(2000);
    }

    return 0; // Never reached
}
