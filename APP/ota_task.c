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
#include "FreeRTOS.h"
#include "task.h"
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
        if ((i & 0x3FF) == 0) { // every 1024 bytes
            g_ota_debug.staged_sp = i;
        }
        crc ^= (uint32_t)data[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1U) crc = (crc >> 1) ^ 0xEDB88320U;
            else          crc >>= 1;
        }
    }
    return ~crc;
}

/* ======================================================================
 *  OTA Task
 * ====================================================================== */
void Task_OTAUpdate(void *arg) {
    (void)arg;

    for (;;) {
        g_ota_debug.step = 1; // Sleeping / waiting
        /* Sleep until HTTP task triggers an OTA request */
        osSemaphoreAcquire(sem_ota_start, osWaitForever);

        g_ota_debug.step = 2; // Acquired semaphore
        if (!ota_request_pending) continue;
        ota_request_pending = 0;

        uint32_t fw_size = ota_content_length;
        g_ota_debug.fw_size = fw_size;
        g_ota_debug.step = 3; // Copied fw_size

        /* Suspend Modbus and MQTT tasks during validation to prevent CPU starvation and W5500 SPI contention */
        extern osThreadId_t g_tid_modbus;
        extern osThreadId_t g_tid_mqtt;
        if (g_tid_modbus) vTaskSuspend((TaskHandle_t)g_tid_modbus);
        if (g_tid_mqtt)   vTaskSuspend((TaskHandle_t)g_tid_mqtt);

        /* Step 1: Compute CRC32 of staged firmware */
        osDelay(10);
        g_ota_debug.step = 4; // Start CRC computation
        uint32_t crc = CRC32_Compute((const uint8_t *)STAGING_ADDR, fw_size);
        g_ota_debug.computed_crc = crc;
        g_ota_debug.step = 5; // Finished CRC computation

        /* Step 2: Basic sanity check — first word must be a valid SRAM SP */
        uint32_t staged_sp = *(volatile uint32_t *)STAGING_ADDR;
        g_ota_debug.staged_sp = staged_sp;
        uint8_t sp_valid = (staged_sp >= 0x20000000U && staged_sp <= 0x20030000U);
        g_ota_debug.sp_valid = sp_valid;
        g_ota_debug.step = 6; // Checked SP

        if (!sp_valid) {
            g_ota_debug.step = 7; // SP check failed
            ota_write_ok = 0;
            g_ota_debug.write_ok = 0;
            osSemaphoreRelease(sem_ota_done);
            /* Resume suspended tasks since we are aborting */
            if (g_tid_modbus) vTaskResume((TaskHandle_t)g_tid_modbus);
            if (g_tid_mqtt)   vTaskResume((TaskHandle_t)g_tid_mqtt);
            continue;
        }

        /* Step 3: Write OTA_Meta_t to Sector 1 */
        g_ota_debug.step = 8; // Erasing Sector 1
        FLASH_EraseSector(1);
        osDelay(50);
        g_ota_debug.step = 9; // Erased Sector 1

        OTA_Meta_t meta = {
            .magic  = OTA_MAGIC_VALUE,
            .status = OTA_STATUS_PENDING,
            .size   = fw_size,
            .crc32  = crc
        };
        g_ota_debug.step = 10; // Writing Metadata to Sector 1
        FLASH_WriteBuffer(OTA_META_ADDR, (uint8_t *)&meta, sizeof(OTA_Meta_t));
        g_ota_debug.step = 11; // Written Metadata to Sector 1

        /* Verify write */
        OTA_Meta_t *read_meta = (OTA_Meta_t *)OTA_META_ADDR;
        uint8_t meta_ok = (read_meta->magic  == OTA_MAGIC_VALUE &&
                           read_meta->status == OTA_STATUS_PENDING &&
                           read_meta->size   == fw_size);
        g_ota_debug.meta_magic  = read_meta->magic;
        g_ota_debug.meta_status = read_meta->status;
        g_ota_debug.meta_size   = read_meta->size;
        g_ota_debug.meta_crc32  = read_meta->crc32;
        g_ota_debug.meta_ok     = meta_ok;
        g_ota_debug.step = 12; // Verified Metadata

        ota_write_ok = meta_ok ? 1 : 0;
        g_ota_debug.write_ok = ota_write_ok;
        g_ota_debug.step = 13; // Releasing sem_ota_done
        osSemaphoreRelease(sem_ota_done);

        if (meta_ok) {
            g_ota_debug.step = 14; // Rebooting
            /* Give HTTP task 300ms to transmit the 200 OK response */
            osDelay(300); /* 300ms: wait for the HTTP task to flush the "200 OK"
                           * over the W5500 socket BEFORE the reset. Rebooting
                           * early would cut off the browser's response and the
                           * update would appear to fail. OTA-only, one-shot. */
            /* Software reset */
            SCB_AIRCR = AIRCR_VECTKEY | AIRCR_SYSRESET;
            while (1);
        } else {
            /* Resume suspended tasks since validation failed */
            if (g_tid_modbus) vTaskResume((TaskHandle_t)g_tid_modbus);
            if (g_tid_mqtt)   vTaskResume((TaskHandle_t)g_tid_mqtt);
        }
    }
}
