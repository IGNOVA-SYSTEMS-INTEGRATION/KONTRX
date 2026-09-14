#include "uart_stm32.h"
#include "stm32f407_regs.h"
#include "rcc_stm32.h"

static uint32_t UART_ComputeBRR(uint32_t pclk, uint32_t baud) {
    uint32_t div_x16 = (2U * pclk + baud) / (2U * baud);
    uint32_t mantissa = div_x16 / 16U;
    uint32_t fraction = div_x16 % 16U;
    return (mantissa << 4) | (fraction & 0xFU);
}

void UART_Init(void) {
    // Enable Clock for USART3
    RCC->APB1ENR |= (1 << 18);

    // Disable USART3
    USART3->CR1 &= ~(1U << 13); // UE = 0

    // Set Baud Rate to 9600 using exact APB1 clock (42 MHz @ 168 MHz SYSCLK -> BRR = 0x1117)
    USART3->BRR = UART_ComputeBRR(RCC_GetPCLK1Freq(), 9600);

    // Set Data bits to 8, Parity none, 1 Stop bit
    USART3->CR1 &= ~(1U << 12); // M = 0 (8 bits)
    USART3->CR1 &= ~(1U << 10); // PCE = 0 (No parity)
    USART3->CR2 &= ~(3U << 12); // STOP = 00 (1 Stop bit)

    // Enable Transmitter and Receiver
    USART3->CR1 |= (1U << 3); // TE = 1
    USART3->CR1 |= (1U << 2); // RE = 1

    // Enable USART3
    USART3->CR1 |= (1U << 13); // UE = 1
}

void UART_SendByte(uint8_t data) {
    // Wait until Transmit Data Register is empty (TXE = 1)
    while (!(USART3->SR & (1U << 7)));
    USART3->DR = data;
}

void UART_WaitTransmissionComplete(void) {
    // Wait until Transmission Complete (TC = 1)
    while (!(USART3->SR & (1U << 6)));
}

uint8_t UART_ReceiveByte(void) {
    while (!(USART3->SR & (1U << 5)));
    return (uint8_t)(USART3->DR & 0xFF);
}

// USART6 Debug Implementation  (PC6 = TX, PC7 = RX, AF8)
void UART_Debug_Init(void) {
    // 1. Enable GPIOC clock
    RCC->AHB1ENR |= (1U << 2);

    // 2. Configure PC6 (TX) and PC7 (RX) as Alternate Function
    GPIOC->MODER &= ~((3U << (6 * 2)) | (3U << (7 * 2)));
    GPIOC->MODER |=  ((2U << (6 * 2)) | (2U << (7 * 2)));  // AF mode

    // Push-Pull, High Speed
    GPIOC->OTYPER  &= ~((1U << 6) | (1U << 7));
    GPIOC->OSPEEDR &= ~((3U << (6*2)) | (3U << (7*2)));
    GPIOC->OSPEEDR |=  ((2U << (6*2)) | (2U << (7*2)));

    // Pull-up on RX (PC7)
    GPIOC->PUPDR &= ~((3U << (6*2)) | (3U << (7*2)));
    GPIOC->PUPDR |=  (1U << (7*2));  // Pull-up on PC7 (RX)

    // 3. Set AF8 (USART6) on PC6 and PC7
    //    AFRL controls pins 0-7  → AFR[0]
    //    PC6 → AFRL[27:24],  PC7 → AFRL[31:28]
    GPIOC->AFR[0] &= ~((0xFU << (6 * 4)) | (0xFU << (7 * 4)));
    GPIOC->AFR[0] |=  ((8U  << (6 * 4)) | (8U  << (7 * 4)));  // AF8

    // 4. Enable USART6 clock on APB2
    RCC->APB2ENR |= (1U << 5);

    // 5. Configure USART6
    USART6->CR1 &= ~(1U << 13);   // Disable while configuring

    // BRR for 9600 baud using exact APB2 clock (84 MHz @ 168 MHz SYSCLK -> BRR = 0x222E)
    USART6->BRR = UART_ComputeBRR(RCC_GetPCLK2Freq(), 9600);

    USART6->CR1 &= ~(1U << 12);   // 8 data bits
    USART6->CR1 &= ~(1U << 10);   // No parity
    USART6->CR2 &= ~(3U << 12);   // 1 stop bit

    USART6->CR1 |= (1U << 3);     // TE — Transmitter Enable
    USART6->CR1 |= (1U << 2);     // RE — Receiver Enable

    USART6->CR1 |= (1U << 13);    // UE — USART Enable
}

void UART_Debug_SendByte(uint8_t data) {
    while (!(USART6->SR & (1U << 7)));  // Wait TXE
    USART6->DR = data;
    while (!(USART6->SR & (1U << 6)));  // Wait TC
}

uint8_t UART_Debug_ReceiveByte(void) {
    while (!(USART6->SR & (1U << 5)));  // Wait RXNE
    return (uint8_t)(USART6->DR & 0xFF);
}

// Wrapper functions for Modbus Function Pointers
void UART_Modbus_SendByte(uint8_t data) {
    UART_SendByte(data);
}

void UART_Modbus_WaitTransmissionComplete(void) {
    UART_WaitTransmissionComplete();
}

uint8_t UART_Modbus_ReceiveByte(void) {
    return UART_ReceiveByte();
}

uint8_t UART_Modbus_ReceiveByte_Timeout(uint8_t* data, uint32_t timeout) {
    uint32_t start = timeout;
    while (!(USART3->SR & (1U << 5))) {
        // Clear Overrun Error (ORE), Noise Error (NE), or Framing Error (FE) if set
        if (USART3->SR & ((1U << 3) | (1U << 2) | (1U << 1))) {
            volatile uint32_t dummy = USART3->SR;
            dummy = USART3->DR;
            (void)dummy;
        }
        if (--start == 0) return 0; // Timeout
    }
    *data = (uint8_t)(USART3->DR & 0xFF);
    return 1; // Success
}
