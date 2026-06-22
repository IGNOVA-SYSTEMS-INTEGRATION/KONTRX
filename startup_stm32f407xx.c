#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

extern int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void SysTick_Handler(void);
void SVC_Handler(void);
void PendSV_Handler(void);

void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector")))
uint32_t *vector_table[] = {
    (uint32_t *)&_estack,
    (uint32_t *)Reset_Handler,
    (uint32_t *)NMI_Handler,
    (uint32_t *)HardFault_Handler,
    0, 0, 0, 0, 0, 0, 0,
    (uint32_t *)SVC_Handler, // SVC_Handler
    (uint32_t *)Default_Handler, // DebugMon_Handler
    0,
    (uint32_t *)PendSV_Handler, // PendSV_Handler
    (uint32_t *)SysTick_Handler,
};

void Reset_Handler(void) {
    uint32_t *src = &_sidata;
    uint32_t *dest = &_sdata;

    // Enable FPU (CP10 and CP11) to prevent UsageFault on float operations
    uint32_t *CPACR = (uint32_t *)0xE000ED88;
    *CPACR |= (0xF << 20);

    // Copy .data section from flash to RAM
    while (dest < &_edata) {
        *dest++ = *src++;
    }

    // Zero fill .bss section
    dest = &_sbss;
    while (dest < &_ebss) {
        *dest++ = 0;
    }

    // Call main
    main();

    // Loop forever
    while (1);
}

void Default_Handler(void) {
    while (1);
}
