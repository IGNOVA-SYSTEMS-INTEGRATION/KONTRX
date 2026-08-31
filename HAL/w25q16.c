#include "w25q16.h"
#include "spi_stm32.h"
#include "stm32f407_regs.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <stdio.h>

#define W25Q_CS_LOW()   (GPIOB->BSRR = (1U << (0 + 16))) // PB0 CS = 0
#define W25Q_CS_HIGH()  (GPIOB->BSRR = (1U << 0))        // PB0 CS = 1

static SemaphoreHandle_t flashMutex = NULL;

static void Flash_Lock(void) {
    if (flashMutex && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreTakeRecursive(flashMutex, portMAX_DELAY);
    }
}

static void Flash_Unlock(void) {
    if (flashMutex && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreGiveRecursive(flashMutex);
    }
}

static void W25Q_WriteEnable(void) {
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(0x06); // Write Enable CMD
    W25Q_CS_HIGH();
}

static uint8_t W25Q_ReadSR1(void) {
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(0x05); // Read Status Register 1 CMD
    uint8_t sr = SPI1_ReadWriteByte(0xFF);
    W25Q_CS_HIGH();
    return sr;
}

static void W25Q_WaitBusy(void) {
    volatile uint32_t timeout = 2000000; // ~2-3 seconds safety timeout
    while ((W25Q_ReadSR1() & 0x01) && timeout--) {
        __asm__("nop");
    }
}

void W25Q_Init(void) {
    SPI1_Init();
    W25Q_CS_HIGH();

    if (flashMutex == NULL) {
        flashMutex = xSemaphoreCreateRecursiveMutex();
    }
    
    // Check JEDEC ID to confirm wiring
    uint32_t id = W25Q_ReadID();
    printf("[W25Q16] Initializing, JEDEC ID: 0x%08X\r\n", (unsigned int)id);
    if (id == W25Q16_JEDEC_ID) {
        printf("[W25Q16] Found Winbond W25Q16 SPI Flash!\r\n");
    } else {
        printf("[W25Q16] ERROR: SPI Flash mismatch or communication error!\r\n");
    }
}

uint32_t W25Q_ReadID(void) {
    Flash_Lock();
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(0x9F); // Read JEDEC ID CMD
    uint8_t m_id = SPI1_ReadWriteByte(0xFF);
    uint8_t d_type = SPI1_ReadWriteByte(0xFF);
    uint8_t d_id = SPI1_ReadWriteByte(0xFF);
    W25Q_CS_HIGH();
    Flash_Unlock();
    
    return ((uint32_t)m_id << 16) | ((uint32_t)d_type << 8) | d_id;
}

void W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len) {
    Flash_Lock();
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(0x03); // Read Data CMD
    SPI1_ReadWriteByte((addr >> 16) & 0xFF);
    SPI1_ReadWriteByte((addr >> 8) & 0xFF);
    SPI1_ReadWriteByte(addr & 0xFF);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = SPI1_ReadWriteByte(0xFF);
    }
    W25Q_CS_HIGH();
    Flash_Unlock();
}

static void W25Q_WritePage(uint32_t addr, const uint8_t *buf, uint32_t len) {
    W25Q_WriteEnable();
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(0x02); // Page Program CMD
    SPI1_ReadWriteByte((addr >> 16) & 0xFF);
    SPI1_ReadWriteByte((addr >> 8) & 0xFF);
    SPI1_ReadWriteByte(addr & 0xFF);
    for (uint32_t i = 0; i < len; i++) {
        SPI1_ReadWriteByte(buf[i]);
    }
    W25Q_CS_HIGH();
    W25Q_WaitBusy();
}

void W25Q_Write(uint32_t addr, const uint8_t *buf, uint32_t len) {
    Flash_Lock();
    uint32_t page_offset = addr % 256;
    uint32_t space_left = 256 - page_offset;
    
    if (len <= space_left) {
        W25Q_WritePage(addr, buf, len);
    } else {
        W25Q_WritePage(addr, buf, space_left);
        addr += space_left;
        buf += space_left;
        len -= space_left;
        
        while (len >= 256) {
            W25Q_WritePage(addr, buf, 256);
            addr += 256;
            buf += 256;
            len -= 256;
        }
        
        if (len > 0) {
            W25Q_WritePage(addr, buf, len);
        }
    }
    Flash_Unlock();
}

void W25Q_EraseSector(uint32_t sector_addr) {
    Flash_Lock();
    W25Q_WriteEnable();
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(0x20); // Sector Erase (4KB) CMD
    SPI1_ReadWriteByte((sector_addr >> 16) & 0xFF);
    SPI1_ReadWriteByte((sector_addr >> 8) & 0xFF);
    SPI1_ReadWriteByte(sector_addr & 0xFF);
    W25Q_CS_HIGH();
    W25Q_WaitBusy();
    Flash_Unlock();
}

void W25Q_ChipErase(void) {
    Flash_Lock();
    W25Q_WriteEnable();
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(0x60); // Chip Erase CMD (or 0xC7)
    W25Q_CS_HIGH();
    W25Q_WaitBusy();
    Flash_Unlock();
}
