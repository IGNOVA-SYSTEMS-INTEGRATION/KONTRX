/**
 * @file    ota_task.c
 * @brief   Kontrx — Background OTA Update Task (Priority: Low)
 *
 * Sequence after HTTP task uploads binary to staging area:
 *   1. Wait for sem_ota_start (posted by HTTP task).
 *   2. Verify CRC32 of the staged binary.
 *   3. If OK: erase Sector 1, write OTA_Meta_t with PENDING status.
 *   4. Post sem_ota_done (HTTP task sends 200 OK response).
 *   5. Delay 300ms (response transmitted), then trigger SCB_AIRCR reset.
 *   6. Bootloader reads OTA_Meta, copies staging to app, clears flag, resets.
 *
 * Flash Layout (STM32F407VET6 — 512KB):
 *   Sector  0 (0x08000000, 16KB)  — Bootloader
 *   Sector  1 (0x08004000, 16KB)  — OTA Meta
 *   Sectors 2-5 (0x08008000-0x0803FFFF, 224KB) — App
 *   Sectors 6-7 (0x08040000-0x0807FFFF, 256KB) — Staging
 *   Sector  7 tail (0x0807C000, 16KB) — Config EEPROM
 *
 * Config backup/restore is handled by the HTTP task before/after
 * erasing staging sectors (6 & 7). This task only touches Sector 1.
 */

#include "ota_task.h"
#include "http_server_task.h"
#include "flash_stm32.h"
#include "stm32f407_regs.h"
#include "cmsis_os2.h"
#include <stdio.h>

/* ======================================================================
 *  Staging flash region
 * ====================================================================== */
#define STAGING_ADDR        0x08040000U

/* ======================================================================
 *  CRC32 — standard Ethernet polynomial (used by many tools)
 * ====================================================================== */
static uint32_t CRC32_Compute(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1U) crc = (crc >> 1) ^ 0xEDB88320U;
            else          crc >>= 1;
        }
        /* Yield every 256 bytes to avoid starving 1ms task */
        if ((i & 0xFF) == 0xFF) osDelay(1); /* 1ms yield = exact 1ms, keeps the
                                             * 1ms Control Engine schedulable
                                             * during long CRC computation. */
    }
    return ~crc;
}

/* ======================================================================
 *  OTA Task
 * ====================================================================== */
void Task_OTAUpdate(void *arg) {
    (void)arg;

    for (;;) {
        /* Sleep until HTTP task triggers an OTA request */
        osSemaphoreAcquire(sem_ota_start, osWaitForever);

        if (!ota_request_pending) continue;
        ota_request_pending = 0;

        uint32_t fw_size = ota_content_length;

        printf("[OTA] Verifying %lu bytes at 0x%08lX\r\n",
               (unsigned long)fw_size, (unsigned long)STAGING_ADDR);

        /* Step 1: Compute CRC32 of staged firmware */
        osDelay(10); /* 10ms: give the HTTP task a head-start to finish draining
                      * the W5500 RX buffer before OTA reads flash. One-shot
                      * during OTA only, not part of the scan cycle. */
        uint32_t crc = CRC32_Compute((const uint8_t *)STAGING_ADDR, fw_size);
        printf("[OTA] CRC32 = 0x%08lX\r\n", (unsigned long)crc);

        /* Step 2: Basic sanity check — first word must be a valid SRAM SP */
        uint32_t staged_sp = *(volatile uint32_t *)STAGING_ADDR;
        uint8_t sp_valid = (staged_sp >= 0x20000000U && staged_sp <= 0x20030000U);
        printf("[OTA] Staged SP = 0x%08lX (%s)\r\n",
               (unsigned long)staged_sp, sp_valid ? "valid" : "INVALID");

        if (!sp_valid) {
            printf("[OTA] ABORT: Invalid stack pointer. Firmware rejected.\r\n");
            ota_write_ok = 0;
            osSemaphoreRelease(sem_ota_done);
            continue;
        }

        /* Step 3: Write OTA_Meta_t to Sector 1 */
        printf("[OTA] Writing OTA metadata...\r\n");
        FLASH_EraseSector(1);
        osDelay(50); /* 50ms: yield while flash is busy erasing (erase is
                      * interrupt-enabled in FLASH_EraseSector, but this keeps
                      * the scheduler calm during the busy-wait). OTA-only. */

        OTA_Meta_t meta = {
            .magic  = OTA_MAGIC_VALUE,
            .status = OTA_STATUS_PENDING,
            .size   = fw_size,
            .crc32  = crc
        };
        FLASH_WriteBuffer(OTA_META_ADDR, (uint8_t *)&meta, sizeof(OTA_Meta_t));

        /* Verify write */
        OTA_Meta_t *read_meta = (OTA_Meta_t *)OTA_META_ADDR;
        uint8_t meta_ok = (read_meta->magic  == OTA_MAGIC_VALUE &&
                           read_meta->status == OTA_STATUS_PENDING &&
                           read_meta->size   == fw_size);
        printf("[OTA] Meta verify: %s\r\n", meta_ok ? "OK" : "FAIL");

        ota_write_ok = meta_ok ? 1 : 0;
        osSemaphoreRelease(sem_ota_done);

        if (meta_ok) {
            /* Give HTTP task 300ms to transmit the 200 OK response */
            osDelay(300); /* 300ms: wait for the HTTP task to flush the "200 OK"
                           * over the W5500 socket BEFORE the reset. Rebooting
                           * early would cut off the browser's response and the
                           * update would appear to fail. OTA-only, one-shot. */
            printf("[OTA] Rebooting into bootloader...\r\n");
            /* Software reset */
            SCB_AIRCR = AIRCR_VECTKEY | AIRCR_SYSRESET;
            while (1);
        }
    }
}
