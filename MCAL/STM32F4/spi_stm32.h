#ifndef SPI_STM32_H
#define SPI_STM32_H

#include <stdint.h>

void SPI2_Init(void);
uint8_t SPI2_ReadWriteByte(uint8_t data);

#endif // SPI_STM32_H
