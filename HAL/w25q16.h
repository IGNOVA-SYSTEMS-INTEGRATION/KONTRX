#ifndef W25Q16_H
#define W25Q16_H

#include <stdint.h>

/* JEDEC ID for Winbond W25Q16 */
#define W25Q16_JEDEC_ID      0xEF4015U

void W25Q_Init(void);
uint32_t W25Q_ReadID(void);
void W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len);
void W25Q_Write(uint32_t addr, const uint8_t *buf, uint32_t len);
void W25Q_EraseSector(uint32_t sector_addr);
void W25Q_ChipErase(void);

#endif // W25Q16_H
