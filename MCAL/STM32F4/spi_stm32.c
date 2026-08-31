#include "spi_stm32.h"
#include "stm32f407_regs.h"

void SPI1_Init(void) {
    // 1. Enable GPIOB Clock and SPI1 Clock
    RCC->AHB1ENR |= (1U << 1);  // GPIOB clock
    RCC->APB2ENR |= (1U << 12); // SPI1 clock

    // 2. Configure PB3 (SCK), PB4 (MISO), PB5 (MOSI) as Alternate Function AF5
    GPIOB->MODER &= ~((3U << (3 * 2)) | (3U << (4 * 2)) | (3U << (5 * 2)));
    GPIOB->MODER |=  ((2U << (3 * 2)) | (2U << (4 * 2)) | (2U << (5 * 2)));

    GPIOB->OSPEEDR &= ~((3U << (3 * 2)) | (3U << (4 * 2)) | (3U << (5 * 2)));
    GPIOB->OSPEEDR |=  ((2U << (3 * 2)) | (2U << (4 * 2)) | (2U << (5 * 2)));

    GPIOB->AFR[0] &= ~((0xFU << (3 * 4)) | (0xFU << (4 * 4)) | (0xFU << (5 * 4)));
    GPIOB->AFR[0] |=  ((5U << (3 * 4)) | (5U << (4 * 4)) | (5U << (5 * 4)));

    // 3. Configure PB0 as CS (Chip Select) Output
    GPIOB->MODER &= ~(3U << (0 * 2));
    GPIOB->MODER |=  (1U << (0 * 2)); // General purpose output
    GPIOB->OTYPER &= ~(1U << 0);      // Push-pull
    GPIOB->OSPEEDR &= ~(3U << (0 * 2));
    GPIOB->OSPEEDR |=  (2U << (0 * 2)); // High speed
    GPIOB->BSRR = (1U << 0);          // Set CS High initially

    // 4. Configure SPI1 CR1
    // Master mode, Mode 0, fPCLK2/8 = 10.5MHz, MSB first, 8-bit, SSM=1, SSI=1
    SPI1->CR1 = (1U << 9) | // SSM = 1
                (1U << 8) | // SSI = 1
                (1U << 2) | // MSTR = 1
                (2U << 3);  // BR[2:0] = 010 (fPCLK2/8)

    // Enable SPI1
    SPI1->CR1 |= (1U << 6); // SPE = 1
}

uint8_t SPI1_ReadWriteByte(uint8_t data) {
    uint32_t timeout = 100000;
    // Wait until TXE (Transmit buffer empty)
    while (!(SPI1->SR & (1U << 1))) {
        if (--timeout == 0) return 0xFF;
    }

    // Send byte
    SPI1->DR = data;

    timeout = 100000;
    // Wait until RXNE (Receive buffer not empty)
    while (!(SPI1->SR & (1U << 0))) {
        if (--timeout == 0) return 0xFF;
    }

    // Return read byte
    return (uint8_t)SPI1->DR;
}

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
    uint32_t timeout = 100000;
    // Wait until TXE (Transmit buffer empty)
    while (!(SPI2->SR & (1U << 1))) {
        if (--timeout == 0) return 0xFF;
    }

    // Send data
    SPI2->DR = data;

    timeout = 100000;
    // Wait until RXNE (Receive buffer not empty)
    while (!(SPI2->SR & (1U << 0))) {
        if (--timeout == 0) return 0xFF;
    }

    // Return received data
    return (uint8_t)SPI2->DR;
}
