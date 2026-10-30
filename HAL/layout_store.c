/**
 * @file    layout_store.c
 * @brief   Canvas layout blob (opaque JSON from the desktop configurator).
 *
 * Two 8 KB slots in the layout partition:
 *   ACTIVE  (LAYOUT_PARTITION_ADDR)          layout of the accepted rules,
 *                                            served by GET /api/rules and /api/layout
 *   PENDING (LAYOUT_PARTITION_ADDR + 8 KB)   layout of an upload / rollback that
 *                                            waits for approval
 * Accept copies PENDING into ACTIVE; reject leaves ACTIVE untouched.
 * Slot format: [magic:4] [data_len:4] [json_data:N] [crc32:4]
 */
#include "flash_partition.h"
#include "w25q16.h"
#include <stdio.h>
#include <string.h>

#define FLASH_SECTOR_SIZE 4096U
#define COPY_CHUNK        256U

/* Chunked form of Compute_CRC32 (same CRC-32, init ~0, final ~) */
static uint32_t Crc32_Update(uint32_t crc, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1U) crc = (crc >> 1) ^ 0xEDB88320U;
            else          crc >>= 1;
        }
    }
    return crc;
}

static uint8_t Layout_Save(uint32_t base, const char *json, uint32_t len) {
    if (!json || len == 0 || len > LAYOUT_MAX_DATA) {
        printf("[Layout] Save rejected: len=%lu (max %lu)\r\n",
               (unsigned long)len, (unsigned long)LAYOUT_MAX_DATA);
        return 0;
    }

    uint32_t sectors = (len + 12U + FLASH_SECTOR_SIZE - 1U) / FLASH_SECTOR_SIZE;
    for (uint32_t s = 0; s < sectors; s++) {
        W25Q_EraseSector(base + s * FLASH_SECTOR_SIZE);
    }

    uint32_t magic = LAYOUT_MAGIC;
    uint32_t crc   = Compute_CRC32((const uint8_t *)json, len);

    W25Q_Write(base,           (const uint8_t *)&magic, 4);
    W25Q_Write(base + 4,       (const uint8_t *)&len,   4);
    W25Q_Write(base + 8,       (const uint8_t *)json,   len);
    W25Q_Write(base + 8 + len, (const uint8_t *)&crc,   4);
    return 1;
}

/* Reads the slot header; returns data_len or 0 if the slot holds no layout. */
static uint32_t Layout_Header(uint32_t base) {
    uint32_t magic = 0, data_len = 0;
    W25Q_Read(base,     (uint8_t *)&magic,    4);
    W25Q_Read(base + 4, (uint8_t *)&data_len, 4);
    if (magic != LAYOUT_MAGIC || data_len == 0 || data_len > LAYOUT_MAX_DATA) return 0;
    return data_len;
}

static uint32_t Layout_Load(uint32_t base, char *buf, uint32_t buf_size) {
    uint32_t data_len = Layout_Header(base);
    if (data_len == 0) return 0;
    if (data_len >= buf_size) {
        printf("[Layout] Buffer too small: need %lu, have %lu\r\n",
               (unsigned long)data_len, (unsigned long)buf_size);
        return 0;
    }

    W25Q_Read(base + 8, (uint8_t *)buf, data_len);
    buf[data_len] = '\0';

    uint32_t stored_crc = 0;
    W25Q_Read(base + 8 + data_len, (uint8_t *)&stored_crc, 4);
    if (stored_crc != Compute_CRC32((const uint8_t *)buf, data_len)) {
        printf("[Layout] CRC mismatch — layout data corrupted.\r\n");
        return 0;
    }
    return data_len;
}

uint8_t Partition_SaveLayout(const char *json, uint32_t len) {
    uint8_t ok = Layout_Save(LAYOUT_ACTIVE_ADDR, json, len);
    if (ok) printf("[Layout] Saved %lu bytes of canvas layout to flash.\r\n", (unsigned long)len);
    return ok;
}

uint32_t Partition_LoadLayout(char *buf, uint32_t buf_size) {
    return Layout_Load(LAYOUT_ACTIVE_ADDR, buf, buf_size);
}

uint8_t Partition_SavePendingLayout(const char *json, uint32_t len) {
    uint8_t ok = Layout_Save(LAYOUT_PENDING_ADDR, json, len);
    if (ok) printf("[Layout] Stored %lu bytes of pending canvas layout.\r\n", (unsigned long)len);
    return ok;
}

uint32_t Partition_LoadPendingLayout(char *buf, uint32_t buf_size) {
    return Layout_Load(LAYOUT_PENDING_ADDR, buf, buf_size);
}

void Partition_ClearPendingLayout(void) {
    W25Q_EraseSector(LAYOUT_PENDING_ADDR); /* header gone = slot empty */
}

/* 1 if the slot at base holds data_len bytes whose CRC matches (chunked read). */
static uint8_t Layout_Verify(uint32_t base, uint32_t data_len) {
    uint8_t chunk[COPY_CHUNK];
    uint32_t crc_calc = 0xFFFFFFFFU;
    for (uint32_t off = 0; off < data_len; off += COPY_CHUNK) {
        uint32_t n = (data_len - off) < COPY_CHUNK ? (data_len - off) : COPY_CHUNK;
        W25Q_Read(base + 8 + off, chunk, n);
        crc_calc = Crc32_Update(crc_calc, chunk, n);
    }
    uint32_t stored_crc = 0;
    W25Q_Read(base + 8 + data_len, (uint8_t *)&stored_crc, 4);
    return (uint8_t)(~crc_calc == stored_crc);
}

/* Copies the PENDING slot over the ACTIVE slot in small chunks (no large
 * RAM buffer). The pending slot is verified first, so a corrupt pending
 * layout leaves the active one untouched. Returns 1 on success; 0 if the
 * pending slot is empty/corrupt, or the copy does not verify (ACTIVE is
 * then cleared rather than left half-written). */
uint8_t Partition_CommitPendingLayout(void) {
    uint32_t data_len = Layout_Header(LAYOUT_PENDING_ADDR);
    if (data_len == 0) return 0;
    if (!Layout_Verify(LAYOUT_PENDING_ADDR, data_len)) {
        printf("[Layout] Pending layout corrupted — active layout kept.\r\n");
        return 0;
    }

    uint32_t total = data_len + 12U;
    uint32_t sectors = (total + FLASH_SECTOR_SIZE - 1U) / FLASH_SECTOR_SIZE;
    for (uint32_t s = 0; s < sectors; s++) {
        W25Q_EraseSector(LAYOUT_ACTIVE_ADDR + s * FLASH_SECTOR_SIZE);
    }

    uint8_t chunk[COPY_CHUNK];
    for (uint32_t off = 0; off < total; off += COPY_CHUNK) {
        uint32_t n = (total - off) < COPY_CHUNK ? (total - off) : COPY_CHUNK;
        W25Q_Read(LAYOUT_PENDING_ADDR + off, chunk, n);
        W25Q_Write(LAYOUT_ACTIVE_ADDR + off, chunk, n);
    }

    /* Read back the copy */
    if (Layout_Header(LAYOUT_ACTIVE_ADDR) != data_len || !Layout_Verify(LAYOUT_ACTIVE_ADDR, data_len)) {
        printf("[Layout] Commit verify failed — active layout cleared.\r\n");
        W25Q_EraseSector(LAYOUT_ACTIVE_ADDR);
        return 0;
    }
    printf("[Layout] Pending layout (%lu bytes) is now active.\r\n", (unsigned long)data_len);
    return 1;
}
