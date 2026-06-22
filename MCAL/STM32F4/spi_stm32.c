#include "spi_stm32.h"
#include "stm32f407_regs.h"

void SPI2_Init(void) {
    // 1. Enable SPI2 clock
    RCC->APB1ENR |= (1U << 14);

    // 2. Configure SPI2
    // Master mode, baud rate = fPCLK / 2 = 8MHz, CPOL = 0, CPHA = 0 (Mode 0)
    // 8-bit data frame, MSB first, Software slave management (SSM=1, SSI=1)
    SPI2->CR1 = (1U << 9) | // SSM = 1
                (1U << 8) | // SSI = 1
                (1U << 2) | // MSTR = 1
                (0U << 3);  // BR[2:0] = 000 (fPCLK/2)

    // Enable SPI2
    SPI2->CR1 |= (1U << 6); // SPE = 1
}

uint8_t SPI2_ReadWriteByte(uint8_t data) {
    // Wait until TXE (Transmit buffer empty)
    while (!(SPI2->SR & (1U << 1)));

    // Send data
    SPI2->DR = data;

    // Wait until RXNE (Receive buffer not empty)
    while (!(SPI2->SR & (1U << 0)));

    // Return received data
    return (uint8_t)SPI2->DR;
}
