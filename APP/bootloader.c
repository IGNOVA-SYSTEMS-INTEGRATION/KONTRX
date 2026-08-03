#include "stm32f407_regs.h"
#include "uart_stm32.h"
#include "flash_stm32.h"
#include "gpio_stm32.h"
#include "spi_stm32.h"
#include <stdio.h>

#define APP_START_ADDR       0x08008000U
#define STAGING_START_ADDR   0x08040000U

int _write(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) UART_Debug_SendByte((uint8_t)ptr[i]);
    return len;
}

static void Delay_ms(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 3200; j++) {
            __asm__("nop");
        }
    }
}

typedef void (*pFunction)(void);

/* Raw W5500 reset via SPI — no ioLibrary dependency.
 * Sends MR_RST to reset digital core, then PHYCFGR to force PHY
 * out of reset and restart auto-negotiation. */
static void W5500_BootReset(void) {
    SPI2_Init();

    /* Phase 1: Software reset (MR_RST = 0x80 to MR register at offset 0x0000) */
    W5500_CS_Select();
    SPI2_ReadWriteByte(0x00); /* (0x04 & 0x00FF0000) >> 16 = 0x00 */
    SPI2_ReadWriteByte(0x00); /* (0x04 & 0x0000FF00) >> 8  = 0x00 */
    SPI2_ReadWriteByte(0x04); /* (0x04 & 0x000000FF)       = 0x04 */
    SPI2_ReadWriteByte(0x80); /* MR_RST */
    W5500_CS_Deselect();
    Delay_ms(50);

    /* Phase 2: PHY reset + auto-negotiation restart (PHYCFGR = 0x88) */
    /* PHYCFGR offset = 0x002E, write = 0x04 → addr = (0x002E << 8) | 0x04 = 0x002E04 */
    W5500_CS_Select();
    SPI2_ReadWriteByte(0x00); /* (0x002E04 & 0x00FF0000) >> 16 = 0x00 */
    SPI2_ReadWriteByte(0x2E); /* (0x002E04 & 0x0000FF00) >> 8  = 0x2E */
    SPI2_ReadWriteByte(0x04); /* (0x002E04 & 0x000000FF)       = 0x04 */
    SPI2_ReadWriteByte(0x88); /* RST=1, ANEN=1, ANRST=1 */
    W5500_CS_Deselect();
    Delay_ms(100);
}

static void JumpToApplication(uint32_t app_addr) {
    uint32_t jump_addr = *(__IO uint32_t *)(app_addr + 4);
    pFunction JumpToApp = (pFunction)jump_addr;

    /* Set Vector Table Offset Register */
    SCB_VTOR = app_addr;

    /* Initialize user application's Stack Pointer */
    __asm__ volatile ("msr msp, %0" : : "r" (*(__IO uint32_t *)app_addr));

    /* Jump to application */
    JumpToApp();
}

int main(void) {
    UART_Debug_Init();

    /* Keep W5500 CS high during bootloader flash operations to prevent
     * the W5500 from interpreting random SPI noise as commands. */
    GPIO_Init_W5500_Pins();
    W5500_CS_Deselect();

    printf("\r\n========================================\r\n");
    printf("  Kontrx Bootloader v1.0\r\n");
    printf("========================================\r\n");

    OTA_Meta_t *meta = (OTA_Meta_t *)OTA_META_ADDR;

    if (meta->magic == OTA_MAGIC_VALUE && meta->status == OTA_STATUS_PENDING) {
        printf("[BOOT] New firmware detected! Size: %lu bytes\r\n", (unsigned long)meta->size);

        printf("[BOOT] Erasing App Sectors 2, 3, 4, 5...\r\n");
        FLASH_EraseSector(2);
        FLASH_EraseSector(3);
        FLASH_EraseSector(4);
        FLASH_EraseSector(5);

        printf("[BOOT] Copying Firmware from Staging (0x08040000) to App (0x08008000)...\r\n");
        uint32_t words_to_copy = (meta->size + 3) / 4;
        uint32_t *src = (uint32_t *)STAGING_START_ADDR;
        uint32_t *dst = (uint32_t *)APP_START_ADDR;

        for (uint32_t i = 0; i < words_to_copy; i++) {
            FLASH_WriteWord((uint32_t)&dst[i], src[i]);
        }

        printf("[BOOT] Verifying copied firmware...\r\n");
        uint32_t mismatches = 0;
        for (uint32_t i = 0; i < words_to_copy; i++) {
            if (dst[i] != src[i]) {
                mismatches++;
                if (mismatches <= 5) {
                    printf("[BOOT ERROR] Mismatch at offset 0x%08lX: read 0x%08lX, expected 0x%08lX\r\n",
                           (unsigned long)(i * 4), (unsigned long)dst[i], (unsigned long)src[i]);
                }
            }
        }

        if (mismatches > 0) {
            printf("[BOOT ERROR] Copy failed with %lu mismatches! Halted.\r\n", (unsigned long)mismatches);
            while (1);
        }
        printf("[BOOT] Verification successful!\r\n");

        printf("[BOOT] Clearing OTA Flag...\r\n");
        FLASH_EraseSector(1);

        printf("[BOOT] Update Complete! Jumping to App...\r\n");
    } else {
        printf("[BOOT] No update pending. Jumping to App (0x08008000)...\r\n");
    }

    /* Verify application is valid (check stack pointer is in SRAM: 0x20000000 - 0x20020000) */
    uint32_t app_sp = *(__IO uint32_t *)APP_START_ADDR;
    if (app_sp >= 0x20000000U && app_sp <= 0x20020000U) {
        W5500_BootReset();
        Delay_ms(50);
        JumpToApplication(APP_START_ADDR);
    } else {
        printf("[BOOT ERROR] Invalid Application Stack Pointer: 0x%08lX! Halted.\r\n", (unsigned long)app_sp);
        while (1);
    }

    return 0;
}
