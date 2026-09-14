#ifndef SPI3_STM32_H
#define SPI3_STM32_H

#include <stdint.h>

#define SPI3_MAX_CS_CHIPS   4U

/**
 * @brief Initialize SPI3 Master interface on PC10 (SCK), PC11 (MISO), PC12 (MOSI)
 *        and Chip Select pins PC1, PC3, PC5, PC13 for external DACs.
 */
void SPI3_Init(void);

/**
 * @brief Select/Deselect Chip Select pin for target DAC chip (0..3)
 */
void SPI3_CS_Select(uint8_t chip_idx);
void SPI3_CS_Deselect(uint8_t chip_idx);

/**
 * @brief Send 16-bit word over SPI3 (used by MCP4922/AD5624 DACs)
 */
uint16_t SPI3_Write16(uint16_t data);

#endif // SPI3_STM32_H