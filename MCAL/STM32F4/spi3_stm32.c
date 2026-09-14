/**
 * @file    spi3_stm32.c
 * @brief   STM32F407 SPI3 Driver for External DAC (4-20mA Outputs)
 */

#include "spi3_stm32.h"
#include "stm32f407_regs.h"

static const uint8_t s_cs_pins[SPI3_MAX_CS_CHIPS] = {1, 3, 5, 13};

void SPI3_Init(void) {
    // 1. Enable GPIOC and SPI3 clocks
    RCC->AHB1ENR |= (1U << 2);  // GPIOC
    RCC->APB1ENR |= (1U << 15); // SPI3

    // 2. Configure PC10 (SCK), PC11 (MISO), PC12 (MOSI) as Alternate Function AF6
    // MODER = 10 (AF mode)
    GPIOC->MODER &= ~((3U << (10 * 2)) | (3U << (11 * 2)) | (3U << (12 * 2)));
    GPIOC->MODER |=  ((2U << (10 * 2)) | (2U << (11 * 2)) | (2U << (12 * 2)));

    // High Speed
    GPIOC->OSPEEDR |= ((2U << (10 * 2)) | (2U << (11 * 2)) | (2U << (12 * 2)));

    // AFR[1] (pins 8-15): PC10=[11:8], PC11=[15:12], PC12=[19:16] -> AF6
    GPIOC->AFR[1] &= ~((0xFU << (2 * 4)) | (0xFU << (3 * 4)) | (0xFU << (4 * 4)));
    GPIOC->AFR[1] |=  ((6U << (2 * 4))   | (6U << (3 * 4))   | (6U << (4 * 4)));

    // 3. Configure Chip Select Pins PC1, PC3, PC5, PC13 as Output Push-Pull
    for (uint8_t i = 0; i < SPI3_MAX_CS_CHIPS; i++) {
        uint8_t pin = s_cs_pins[i];
        GPIOC->MODER &= ~(3U << (pin * 2));
        GPIOC->MODER |=  (1U << (pin * 2)); // Output
        GPIOC->OTYPER &= ~(1U << pin);     // Push-pull
        GPIOC->OSPEEDR |= (2U << (pin * 2)); // High speed
        GPIOC->BSRR = (1U << pin);          // Deassert (High)
    }

    // 4. Configure SPI3 CR1:
    // 16-bit frame format (DFF = 1, bit 11)
    // Master mode (MSTR = 1, bit 2)
    // Mode 0 (CPOL=0, CPHA=0)
    // Baud Rate = fPCLK1 / 4 = 10.5 MHz (BR[2:0] = 001)
    // Software Slave Management (SSM = 1, SSI = 1)
    SPI3->CR1 = (1U << 11) | // DFF = 1 (16-bit)
                (1U << 9)  | // SSM = 1
                (1U << 8)  | // SSI = 1
                (1U << 2)  | // MSTR = 1
                (1U << 3);   // BR[2:0] = 001 (fPCLK1/4 = 10.5 MHz)

    // Enable SPI3
    SPI3->CR1 |= (1U << 6); // SPE = 1
}

void SPI3_CS_Select(uint8_t chip_idx) {
    if (chip_idx < SPI3_MAX_CS_CHIPS) {
        uint8_t pin = s_cs_pins[chip_idx];
        GPIOC->BSRR = (1U << (pin + 16U)); // Assert Low
    }
}

void SPI3_CS_Deselect(uint8_t chip_idx) {
    if (chip_idx < SPI3_MAX_CS_CHIPS) {
        uint8_t pin = s_cs_pins[chip_idx];
        GPIOC->BSRR = (1U << pin); // Deassert High
    }
}

uint16_t SPI3_Write16(uint16_t data) {
    uint32_t timeout = 100000;
    while (!(SPI3->SR & (1U << 1))) { // Wait TXE
        if (--timeout == 0) return 0xFFFF;
    }

    SPI3->DR = data;

    timeout = 100000;
    while (!(SPI3->SR & (1U << 0))) { // Wait RXNE
        if (--timeout == 0) return 0xFFFF;
    }

    return (uint16_t)SPI3->DR;
}