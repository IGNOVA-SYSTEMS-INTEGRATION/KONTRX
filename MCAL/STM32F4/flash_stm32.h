#ifndef FLASH_STM32_H
#define FLASH_STM32_H

#include <stdint.h>

/* OTA Metadata Structure stored at Sector 1 (0x08004000) */
#define OTA_META_ADDR        0x08004000U
#define OTA_MAGIC_VALUE      0xDEADBEEFU
#define OTA_STATUS_PENDING   0xA5A5A5A5U
#define OTA_STATUS_SUCCESS   0x5A5A5A5AU

/* Config EEPROM — last 16KB of Sector 7 (valid on 512KB STM32F407VET6) */
#define CONFIG_EEPROM_ADDR   0x0807C000U
#define CONFIG_EEPROM_SIZE   256U

typedef struct {
    uint32_t magic;
    uint32_t status;
    uint32_t size;
    uint32_t crc32;
} OTA_Meta_t;

void FLASH_Unlock(void);
void FLASH_Lock(void);
void FLASH_EraseSector(uint8_t sector);
void FLASH_WriteWord(uint32_t address, uint32_t data);
void FLASH_WriteBuffer(uint32_t start_addr, const uint8_t *data, uint32_t len);

#endif // FLASH_STM32_H
