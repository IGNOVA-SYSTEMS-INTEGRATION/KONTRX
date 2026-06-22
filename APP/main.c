#include "gpio_stm32.h"
#include "led.h"
#include "uart_stm32.h"
#include <stdint.h>
#include <stdio.h>

// Override _write so standard printf() uses our Debug USART (USART1)
int _write(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) {
        UART_Debug_SendByte((uint8_t)ptr[i]);
    }
    return len;
}

// Simple software delay (Calibrated for 16MHz HSI default clock)
void Delay_ms(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 3200; j++) {
            __asm__("nop");
        }
    }
}

int main(void) {
    // Hardware Initialization
    GPIO_Init_USART1_Pins();    // Debug USART (PA9, PA10)
    UART_Debug_Init();          // Init USART1
    LED_Init();                 // Heartbeat LED (PA6)
    
    printf("\r\n=== Kontrx STM32 Native Base System ===\r\n");
    printf("Starting heartbeat LED...\r\n");
    
    while (1) {
        LED_Toggle();
        Delay_ms(500);
    }
}
