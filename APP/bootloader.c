#include "stm32f407_regs.h"
#include "uart_stm32.h"
#include "flash_stm32.h"
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

        printf("[BOOT] Update Complete! Resetting...\r\n");
        Delay_ms(100);
        SCB_AIRCR = AIRCR_VECTKEY | AIRCR_SYSRESET;
    } else {
        printf("[BOOT] No update pending. Jumping to App (0x08008000)...\r\n");
    }

    /* Verify application is valid (check stack pointer is in SRAM: 0x20000000 - 0x20020000) */
    uint32_t app_sp = *(__IO uint32_t *)APP_START_ADDR;
    if (app_sp >= 0x20000000U && app_sp <= 0x20020000U) {
        Delay_ms(50);
        JumpToApplication(APP_START_ADDR);
    } else {
        printf("[BOOT ERROR] Invalid Application Stack Pointer: 0x%08lX! Halted.\r\n", (unsigned long)app_sp);
        while (1);
    }

    return 0;
}
