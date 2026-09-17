#include "led.h"
#include "gpio_stm32.h"
#include "stm32f407_regs.h"

// LED is on PA6
#define LED_PORT GPIOA
#define LED_PIN  6

void LED_Init(void) {
    GPIO_InitOutput(LED_PORT, LED_PIN);
}

void LED_Toggle(void) {
    GPIO_TogglePin(LED_PORT, LED_PIN);
}

void LED_On(void) {
    GPIOA->BSRR = (1U << 6);
}

void LED_Off(void) {
    GPIOA->BSRR = (1U << (6 + 16));
}

__attribute__((weak)) int _write(int file, char *ptr, int len) { (void)file; (void)ptr; return len; }
__attribute__((weak)) int _read(int file, char *ptr, int len) { (void)file; (void)ptr; (void)len; return 0; }
