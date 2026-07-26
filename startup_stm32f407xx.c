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

/* Core exceptions */
void NMI_Handler(void)          __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)     __attribute__((weak, alias("Default_Handler")));

/* STM32F407 Peripheral Interrupts */
void WWDG_IRQHandler(void)                   __attribute__((weak, alias("Default_Handler")));
void PVD_IRQHandler(void)                    __attribute__((weak, alias("Default_Handler")));
void TAMP_STAMP_IRQHandler(void)             __attribute__((weak, alias("Default_Handler")));
void RTC_WKUP_IRQHandler(void)               __attribute__((weak, alias("Default_Handler")));
void FLASH_IRQHandler(void)                  __attribute__((weak, alias("Default_Handler")));
void RCC_IRQHandler(void)                    __attribute__((weak, alias("Default_Handler")));
void EXTI0_IRQHandler(void)                  __attribute__((weak, alias("Default_Handler")));
void EXTI1_IRQHandler(void)                  __attribute__((weak, alias("Default_Handler")));
void EXTI2_IRQHandler(void)                  __attribute__((weak, alias("Default_Handler")));
void EXTI3_IRQHandler(void)                  __attribute__((weak, alias("Default_Handler")));
void EXTI4_IRQHandler(void)                  __attribute__((weak, alias("Default_Handler")));
void DMA1_Stream0_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA1_Stream1_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA1_Stream2_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA1_Stream3_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA1_Stream4_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA1_Stream5_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA1_Stream6_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void ADC_IRQHandler(void)                    __attribute__((weak, alias("Default_Handler")));
void CAN1_TX_IRQHandler(void)                __attribute__((weak, alias("Default_Handler")));
void CAN1_RX0_IRQHandler(void)               __attribute__((weak, alias("Default_Handler")));
void CAN1_RX1_IRQHandler(void)               __attribute__((weak, alias("Default_Handler")));
void CAN1_SCE_IRQHandler(void)               __attribute__((weak, alias("Default_Handler")));
void EXTI9_5_IRQHandler(void)                __attribute__((weak, alias("Default_Handler")));
void TIM1_BRK_TIM9_IRQHandler(void)          __attribute__((weak, alias("Default_Handler")));
void TIM1_UP_TIM10_IRQHandler(void)          __attribute__((weak, alias("Default_Handler")));
void TIM1_TRG_COM_TIM11_IRQHandler(void)     __attribute__((weak, alias("Default_Handler")));
void TIM1_CC_IRQHandler(void)                __attribute__((weak, alias("Default_Handler")));
void TIM2_IRQHandler(void)                   __attribute__((weak, alias("Default_Handler")));
void TIM3_IRQHandler(void)                   __attribute__((weak, alias("Default_Handler")));
void TIM4_IRQHandler(void)                   __attribute__((weak, alias("Default_Handler")));
void I2C1_EV_IRQHandler(void)                __attribute__((weak, alias("Default_Handler")));
void I2C1_ER_IRQHandler(void)                __attribute__((weak, alias("Default_Handler")));
void I2C2_EV_IRQHandler(void)                __attribute__((weak, alias("Default_Handler")));
void I2C2_ER_IRQHandler(void)                __attribute__((weak, alias("Default_Handler")));
void SPI1_IRQHandler(void)                   __attribute__((weak, alias("Default_Handler")));
void SPI2_IRQHandler(void)                   __attribute__((weak, alias("Default_Handler")));
void USART1_IRQHandler(void)                 __attribute__((weak, alias("Default_Handler")));
void USART2_IRQHandler(void)                 __attribute__((weak, alias("Default_Handler")));
void USART3_IRQHandler(void)                 __attribute__((weak, alias("Default_Handler")));
void EXTI15_10_IRQHandler(void)              __attribute__((weak, alias("Default_Handler")));
void RTC_Alarm_IRQHandler(void)              __attribute__((weak, alias("Default_Handler")));
void OTG_FS_WKUP_IRQHandler(void)            __attribute__((weak, alias("Default_Handler")));
void TIM8_BRK_TIM12_IRQHandler(void)         __attribute__((weak, alias("Default_Handler")));
void TIM8_UP_TIM13_IRQHandler(void)          __attribute__((weak, alias("Default_Handler")));
void TIM8_TRG_COM_TIM14_IRQHandler(void)     __attribute__((weak, alias("Default_Handler")));
void TIM8_CC_IRQHandler(void)                __attribute__((weak, alias("Default_Handler")));
void DMA1_Stream7_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void FSMC_IRQHandler(void)                   __attribute__((weak, alias("Default_Handler")));
void SDIO_IRQHandler(void)                   __attribute__((weak, alias("Default_Handler")));
void TIM5_IRQHandler(void)                   __attribute__((weak, alias("Default_Handler")));
void SPI3_IRQHandler(void)                   __attribute__((weak, alias("Default_Handler")));
void UART4_IRQHandler(void)                  __attribute__((weak, alias("Default_Handler")));
void UART5_IRQHandler(void)                  __attribute__((weak, alias("Default_Handler")));
void TIM6_DAC_IRQHandler(void)               __attribute__((weak, alias("Default_Handler")));
void TIM7_IRQHandler(void)                   __attribute__((weak, alias("Default_Handler")));
void DMA2_Stream0_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA2_Stream1_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA2_Stream2_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA2_Stream3_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA2_Stream4_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void ETH_IRQHandler(void)                    __attribute__((weak, alias("Default_Handler")));
void ETH_WKUP_IRQHandler(void)               __attribute__((weak, alias("Default_Handler")));
void CAN2_TX_IRQHandler(void)                __attribute__((weak, alias("Default_Handler")));
void CAN2_RX0_IRQHandler(void)               __attribute__((weak, alias("Default_Handler")));
void CAN2_RX1_IRQHandler(void)               __attribute__((weak, alias("Default_Handler")));
void CAN2_SCE_IRQHandler(void)               __attribute__((weak, alias("Default_Handler")));
void OTG_FS_IRQHandler(void)                 __attribute__((weak, alias("Default_Handler")));
void DMA2_Stream5_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA2_Stream6_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void DMA2_Stream7_IRQHandler(void)           __attribute__((weak, alias("Default_Handler")));
void USART6_IRQHandler(void)                 __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector")))
uint32_t *vector_table[] = {
    (uint32_t *)&_estack,
    (uint32_t *)Reset_Handler,
    (uint32_t *)NMI_Handler,
    (uint32_t *)HardFault_Handler,
    (uint32_t *)MemManage_Handler,
    (uint32_t *)BusFault_Handler,
    (uint32_t *)UsageFault_Handler,
    0, 0, 0, 0,
    (uint32_t *)SVC_Handler,
    (uint32_t *)DebugMon_Handler,
    0,
    (uint32_t *)PendSV_Handler,
    (uint32_t *)SysTick_Handler,
    
    /* External Interrupts */
    (uint32_t *)WWDG_IRQHandler,
    (uint32_t *)PVD_IRQHandler,
    (uint32_t *)TAMP_STAMP_IRQHandler,
    (uint32_t *)RTC_WKUP_IRQHandler,
    (uint32_t *)FLASH_IRQHandler,
    (uint32_t *)RCC_IRQHandler,
    (uint32_t *)EXTI0_IRQHandler,
    (uint32_t *)EXTI1_IRQHandler,
    (uint32_t *)EXTI2_IRQHandler,
    (uint32_t *)EXTI3_IRQHandler,
    (uint32_t *)EXTI4_IRQHandler,
    (uint32_t *)DMA1_Stream0_IRQHandler,
    (uint32_t *)DMA1_Stream1_IRQHandler,
    (uint32_t *)DMA1_Stream2_IRQHandler,
    (uint32_t *)DMA1_Stream3_IRQHandler,
    (uint32_t *)DMA1_Stream4_IRQHandler,
    (uint32_t *)DMA1_Stream5_IRQHandler,
    (uint32_t *)DMA1_Stream6_IRQHandler,
    (uint32_t *)ADC_IRQHandler,
    (uint32_t *)CAN1_TX_IRQHandler,
    (uint32_t *)CAN1_RX0_IRQHandler,
    (uint32_t *)CAN1_RX1_IRQHandler,
    (uint32_t *)CAN1_SCE_IRQHandler,
    (uint32_t *)EXTI9_5_IRQHandler,
    (uint32_t *)TIM1_BRK_TIM9_IRQHandler,
    (uint32_t *)TIM1_UP_TIM10_IRQHandler,
    (uint32_t *)TIM1_TRG_COM_TIM11_IRQHandler,
    (uint32_t *)TIM1_CC_IRQHandler,
    (uint32_t *)TIM2_IRQHandler,
    (uint32_t *)TIM3_IRQHandler,
    (uint32_t *)TIM4_IRQHandler,
    (uint32_t *)I2C1_EV_IRQHandler,
    (uint32_t *)I2C1_ER_IRQHandler,
    (uint32_t *)I2C2_EV_IRQHandler,
    (uint32_t *)I2C2_ER_IRQHandler,
    (uint32_t *)SPI1_IRQHandler,
    (uint32_t *)SPI2_IRQHandler,
    (uint32_t *)USART1_IRQHandler,
    (uint32_t *)USART2_IRQHandler,
    (uint32_t *)USART3_IRQHandler,
    (uint32_t *)EXTI15_10_IRQHandler,
    (uint32_t *)RTC_Alarm_IRQHandler,
    (uint32_t *)OTG_FS_WKUP_IRQHandler,
    (uint32_t *)TIM8_BRK_TIM12_IRQHandler,
    (uint32_t *)TIM8_UP_TIM13_IRQHandler,
    (uint32_t *)TIM8_TRG_COM_TIM14_IRQHandler,
    (uint32_t *)TIM8_CC_IRQHandler,
    (uint32_t *)DMA1_Stream7_IRQHandler,
    (uint32_t *)FSMC_IRQHandler,
    (uint32_t *)SDIO_IRQHandler,
    (uint32_t *)TIM5_IRQHandler,
    (uint32_t *)SPI3_IRQHandler,
    (uint32_t *)UART4_IRQHandler,
    (uint32_t *)UART5_IRQHandler,
    (uint32_t *)TIM6_DAC_IRQHandler,
    (uint32_t *)TIM7_IRQHandler,
    (uint32_t *)DMA2_Stream0_IRQHandler,
    (uint32_t *)DMA2_Stream1_IRQHandler,
    (uint32_t *)DMA2_Stream2_IRQHandler,
    (uint32_t *)DMA2_Stream3_IRQHandler,
    (uint32_t *)DMA2_Stream4_IRQHandler,
    (uint32_t *)ETH_IRQHandler,
    (uint32_t *)ETH_WKUP_IRQHandler,
    (uint32_t *)CAN2_TX_IRQHandler,
    (uint32_t *)CAN2_RX0_IRQHandler,
    (uint32_t *)CAN2_RX1_IRQHandler,
    (uint32_t *)CAN2_SCE_IRQHandler,
    (uint32_t *)OTG_FS_IRQHandler,
    (uint32_t *)DMA2_Stream5_IRQHandler,
    (uint32_t *)DMA2_Stream6_IRQHandler,
    (uint32_t *)DMA2_Stream7_IRQHandler,
    (uint32_t *)USART6_IRQHandler,
};

void Reset_Handler(void) {
    uint32_t *src = &_sidata;
    uint32_t *dest = &_sdata;

    // Set VTOR to point to current vector table
    uint32_t *VTOR = (uint32_t *)0xE000ED08;
    *VTOR = (uint32_t)vector_table;

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

/* Stubs for handlers previously provided by FreeRTOS */
void SysTick_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)      __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)   __attribute__((weak, alias("Default_Handler")));
