/**
 * @file  test_layout_store.c
 * @brief Host test: active / pending canvas layout slots (HAL/layout_store.c)
 *        on an in-memory W25Q16 with NOR flash rules (erase -> 0xFF,
 *        program can only clear bits).
 *
 * Build (from tests/):
 *   gcc -std=gnu11 -Wall -Wextra -g -Ihost_stubs -I../HAL -I../APP \
 *       test_layout_store.c ../HAL/layout_store.c -o test_layout_store && ./test_layout_store
 */
#include <stdio.h>
#include <string.h>
#include "flash_partition.h"
#include "w25q16.h"

/* ---- in-memory W25Q16 (2 MB) ---- */
static uint8_t s_flash[0x200000];

void W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len) { memcpy(buf, &s_flash[addr], len); }
void W25Q_Write(uint32_t addr, const uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) s_flash[addr + i] &= buf[i]; /* NOR: 1 -> 0 only */
}
void W25Q_EraseSector(uint32_t sector_addr) { memset(&s_flash[sector_addr & ~0xFFFU], 0xFF, 4096); }

/* Same CRC-32 as HAL/flash_partition.c */
uint32_t Compute_CRC32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (uint8_t b = 0; b < 8; b++) crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
    }
    return ~crc;
}

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } } while (0)

static char buf[LAYOUT_MAX_DATA + 1];

static void make_layout(char *out, size_t size, const char *tag) {
    /* JSON-ish blob of `size` bytes ending in tag, so copies can be told apart */
    memset(out, 'x', size);
    out[0] = '{';
    size_t tl = strlen(tag);
    memcpy(out + size - tl - 1, tag, tl);
    out[size - 1] = '}';
    out[size] = '\0';
}

static void test_slots_do_not_overlap(void) {
    CHECK(LAYOUT_PENDING_ADDR >= LAYOUT_ACTIVE_ADDR + LAYOUT_SLOT_SIZE, "pending slot after active slot");
    CHECK(LAYOUT_PENDING_ADDR + LAYOUT_SLOT_SIZE <= LAYOUT_PARTITION_ADDR + LAYOUT_PARTITION_SIZE, "pending slot inside partition");
    CHECK(LAYOUT_PARTITION_ADDR + LAYOUT_PARTITION_SIZE <= PARTITION_QUEUE_ADDR, "layout partition before telemetry queue");
}

static void test_upload_does_not_touch_active_until_commit(void) {
    memset(s_flash, 0xFF, sizeof(s_flash));
    CHECK(Partition_SaveLayout("{\"v\":\"active\"}", 14), "save active");
    CHECK(Partition_SavePendingLayout("{\"v\":\"pending\"}", 15), "save pending");

    CHECK(Partition_LoadLayout(buf, sizeof(buf)) == 14 && strcmp(buf, "{\"v\":\"active\"}") == 0,
          "active layout unchanged while rules are pending");
    CHECK(Partition_LoadPendingLayout(buf, sizeof(buf)) == 15, "pending readable");

    CHECK(Partition_CommitPendingLayout() == 1, "commit");
    CHECK(Partition_LoadLayout(buf, sizeof(buf)) == 15 && strcmp(buf, "{\"v\":\"pending\"}") == 0,
          "accepted layout is active");
}

static void test_reject_keeps_active(void) {
    memset(s_flash, 0xFF, sizeof(s_flash));
    Partition_SaveLayout("{\"v\":\"active\"}", 14);
    Partition_SavePendingLayout("{\"v\":\"rejected\"}", 16);
    Partition_ClearPendingLayout();
    CHECK(Partition_LoadPendingLayout(buf, sizeof(buf)) == 0, "pending slot empty after clear");
    CHECK(Partition_CommitPendingLayout() == 0, "nothing to commit");
    CHECK(Partition_LoadLayout(buf, sizeof(buf)) == 14, "active layout still there");
}

static void test_large_layout_copy_and_shrink(void) {
    static char big[LAYOUT_MAX_DATA + 1];
    memset(s_flash, 0xFF, sizeof(s_flash));
    make_layout(big, LAYOUT_MAX_DATA, "BIG");                 /* fills both sectors of a slot */
    CHECK(Partition_SaveLayout(big, LAYOUT_MAX_DATA), "save max-size active");
    CHECK(Partition_SavePendingLayout(big, LAYOUT_MAX_DATA), "save max-size pending");
    CHECK(Partition_CommitPendingLayout() == 1, "commit max-size (multi-chunk copy)");
    CHECK(Partition_LoadLayout(buf, sizeof(buf)) == LAYOUT_MAX_DATA && memcmp(buf, big, LAYOUT_MAX_DATA) == 0,
          "max-size layout intact after copy");

    /* A smaller layout over a bigger one: old bytes must not corrupt it */
    Partition_SavePendingLayout("{\"small\":1}", 11);
    CHECK(Partition_CommitPendingLayout() == 1, "commit small over big");
    CHECK(Partition_LoadLayout(buf, sizeof(buf)) == 11 && strcmp(buf, "{\"small\":1}") == 0, "small layout intact");
}

static void test_limits_and_corruption(void) {
    static char big[LAYOUT_MAX_DATA + 2];
    memset(s_flash, 0xFF, sizeof(s_flash));
    make_layout(big, LAYOUT_MAX_DATA + 1, "X");
    CHECK(Partition_SavePendingLayout(big, LAYOUT_MAX_DATA + 1) == 0, "oversize layout rejected");
    CHECK(Partition_SavePendingLayout("", 0) == 0, "empty layout rejected");

    Partition_SaveLayout("{\"v\":\"active\"}", 14);
    Partition_SavePendingLayout("{\"v\":\"pending\"}", 15);
    s_flash[LAYOUT_PENDING_ADDR + 10] ^= 0x01;                /* flip a data bit */
    CHECK(Partition_LoadPendingLayout(buf, sizeof(buf)) == 0, "corrupt pending detected by CRC");
    CHECK(Partition_CommitPendingLayout() == 0, "corrupt pending not committed");
    CHECK(Partition_LoadLayout(buf, sizeof(buf)) == 14 && strcmp(buf, "{\"v\":\"active\"}") == 0,
          "active layout kept when pending is corrupt");

    Partition_SaveLayout("{\"v\":\"active\"}", 14);
    CHECK(Partition_LoadLayout(buf, 10) == 0, "buffer too small refused");
}

int main(void) {
    test_slots_do_not_overlap();
    test_upload_does_not_touch_active_until_commit();
    test_reject_keeps_active();
    test_large_layout_copy_and_shrink();
    test_limits_and_corruption();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("layout_store: all checks passed\n");
    return 0;
}
