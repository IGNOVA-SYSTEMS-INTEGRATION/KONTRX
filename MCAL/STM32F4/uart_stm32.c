#include "uart_stm32.h"
#include "stm32f407_regs.h"

void UART_Init(void) {
    // Enable Clock for USART3
    RCC->APB1ENR |= (1 << 18);

    // Disable USART3
    USART3->CR1 &= ~(1U << 13); // UE = 0

    // Set Baud Rate to 9600 (Assuming 16 MHz APB1 Clock)
    // BRR = 104.1875 -> Mantissa = 104 (0x68), Fraction = 3 (0x03)
    // BRR = 0x0683
    USART3->BRR = 0x0683;

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

// USART1 Debug Implementation
void UART_Debug_Init(void) {
    // Enable Clock for USART1 (APB2 is typically 16MHz default if no PLL)
    RCC->APB2ENR |= (1 << 4);

    USART1->CR1 &= ~(1U << 13);

    // 9600 baud -> 0x0683
    USART1->BRR = 0x0683;

    USART1->CR1 &= ~(1U << 12);
    USART1->CR1 &= ~(1U << 10);
    USART1->CR2 &= ~(3U << 12);

    USART1->CR1 |= (1U << 3); // TE
    USART1->CR1 |= (1U << 2); // RE

    USART1->CR1 |= (1U << 13); // UE
}

void UART_Debug_SendByte(uint8_t data) {
    while (!(USART1->SR & (1U << 7)));
    USART1->DR = data;
    while (!(USART1->SR & (1U << 6)));
}

uint8_t UART_Debug_ReceiveByte(void) {
    while (!(USART1->SR & (1U << 5)));
    return (uint8_t)(USART1->DR & 0xFF);
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
