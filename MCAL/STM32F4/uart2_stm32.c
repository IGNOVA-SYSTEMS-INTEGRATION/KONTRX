/**
 * @file    uart2_stm32.c
 * @brief   STM32F407 USART2 Driver for Secondary RS485 Bus
 */

#include "uart2_stm32.h"
#include "stm32f407_regs.h"
#include "rcc_stm32.h"

void UART2_Init(uint32_t baud) {
    // 1. Enable GPIOD and USART2 clocks
    RCC->AHB1ENR |= (1U << 3);  // GPIOD
    RCC->APB1ENR |= (1U << 17); // USART2

    // 2. Configure PD5 (TX) and PD6 (RX) as Alternate Function AF7
    GPIOD->MODER &= ~((3U << (5 * 2)) | (3U << (6 * 2)));
    GPIOD->MODER |=  ((2U << (5 * 2)) | (2U << (6 * 2)));
    GPIOD->OSPEEDR |= ((2U << (5 * 2)) | (2U << (6 * 2)));
    GPIOD->PUPDR   |= (1U << (6 * 2)); // Pull-up RX

    GPIOD->AFR[0] &= ~((0xFU << (5 * 4)) | (0xFU << (6 * 4)));
    GPIOD->AFR[0] |=  ((7U    << (5 * 4)) | (7U    << (6 * 4))); // AF7

    // 3. Configure PD4 (DE) and PD7 (RE#) as General Purpose Outputs
    GPIOD->MODER &= ~((3U << (4 * 2)) | (3U << (7 * 2)));
    GPIOD->MODER |=  ((1U << (4 * 2)) | (1U << (7 * 2)));
    GPIOD->OSPEEDR |= ((2U << (4 * 2)) | (2U << (7 * 2)));
    GPIOD->BSRR = (1U << (4 + 16U)) | (1U << (7 + 16U)); // DE=0, RE=0 (Receive mode)

    // 4. Configure Baud Rate on APB1
    uint32_t pclk1 = RCC_GetPCLK1Freq();
    uint32_t div_x16 = (2U * pclk1 + baud) / (2U * baud);
    USART2->BRR = ((div_x16 / 16U) << 4) | (div_x16 % 16U);

    // 8 data bits, 1 stop bit, no parity
    USART2->CR1 &= ~((1U << 12) | (1U << 10));
    USART2->CR2 &= ~(3U << 12);

    // Enable TX and RX and USART
    USART2->CR1 |= (1U << 3) | (1U << 2) | (1U << 13);
}

void UART2_SendByte(uint8_t data) {
    // Assert DE=1, RE=1 for TX
    GPIOD->BSRR = (1U << 4) | (1U << 7);
    while (!(USART2->SR & (1U << 7)));
    USART2->DR = data;
    while (!(USART2->SR & (1U << 6)));
    // Return to RX mode
    GPIOD->BSRR = (1U << (4 + 16U)) | (1U << (7 + 16U));
}

uint8_t UART2_ReceiveByte(void) {
    while (!(USART2->SR & (1U << 5)));
    return (uint8_t)(USART2->DR & 0xFF);
}

uint8_t UART2_ReceiveByte_Timeout(uint8_t* data, uint32_t timeout) {
    uint32_t start = timeout;
    while (!(USART2->SR & (1U << 5))) {
        if (--start == 0) return 0;
    }
    *data = (uint8_t)(USART2->DR & 0xFF);
    return 1;
}