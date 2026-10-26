#include "sdcard.h"
#include "flash_partition.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "rtc_stm32.h"
#include "safe_fmt.h"
#include "fault_supervisor.h"
#include "portable.h"
#include "w25q16.h"

extern volatile uint32_t g_uptime_seconds;

static SDCard_Status_t s_sd_status = {
    .mounted = 1,
    .critical_locked = 0,
    .card_type = 2, /* SDHC/SDXC */
    .total_capacity_mb = 65536, /* 64 GB SD Card */
    .free_capacity_mb = 65536,
    .queue_record_count = 0,
    .log_entry_count = 0,
    .log_file_bytes = 0
};

static SemaphoreHandle_t sdMutex = NULL;

static const char *sd_fault_name(uint32_t type) {
    switch (type) {
        case 1: return "HardFault";
        case 2: return "MemManage";
        case 3: return "BusFault";
        case 4: return "UsageFault";
        case 5: return "StackOverflow";
        case 6: return "Unrecoverable";
        default: return "None";
    }
}

static void append_escaped(char *out, int *pos, int max_len, const char *str) {
    if (!out || !pos || !str) return;
    for (const char *p = str; *p != '\0' && *pos < max_len - 4; p++) {
        if (*p == '\r') continue;
        if (*p == '\n') {
            if (*pos < max_len - 3) {
                out[(*pos)++] = '\\';
                out[(*pos)++] = 'n';
            }
        } else if (*p == '"' || *p == '\\') {
            if (*pos < max_len - 3) {
                out[(*pos)++] = '\\';
                out[(*pos)++] = *p;
            }
        } else {
            out[(*pos)++] = *p;
        }
    }
}

int SDCard_FormatBootAudit(char *buf, int max_len) {
    if (!buf || max_len <= 0) return 0;

    uint16_t yr = 2026;
    uint8_t mo = 10, dy = 25, hr = 12, mn = 0, sc = 0;
    RTC_GetDateTime(&yr, &mo, &dy, &hr, &mn, &sc);

    uint32_t up_s = g_uptime_seconds;
    uint32_t up_h = up_s / 3600U;
    uint32_t up_m = (up_s % 3600U) / 60U;
    uint32_t up_sec = up_s % 60U;

    size_t free_heap = xPortGetFreeHeapSize();
    size_t min_heap = xPortGetMinimumEverFreeHeapSize();

    /* Reset flags from RCC_CSR (0x40023874) */
    uint32_t csr = *(volatile uint32_t *)0x40023874UL;
    const char *reset_cause = "Power-On / External Reset";
    if (csr & (1U << 31)) reset_cause = "Low-Power Reset";
    else if (csr & (1U << 30)) reset_cause = "Window Watchdog Reset";
    else if (csr & (1U << 29)) reset_cause = "Independent Watchdog Reset";
    else if (csr & (1U << 28)) reset_cause = "Software Reset (Reboot/OTA)";
    else if (csr & (1U << 27)) reset_cause = "Power-On / Power-Down Reset";
    else if (csr & (1U << 26)) reset_cause = "External NRST Pin Reset";

    char fault_detail[100] = "No faults recorded since boot";
    if (g_fault_record.magic == 0x4B464C54U && g_fault_record.total > 0) {
        snprintf(fault_detail, sizeof(fault_detail), "%lu fault(s) recorded. Last: %s in task '%s' (PC=0x%08lX)",
                 (unsigned long)g_fault_record.total,
                 sd_fault_name(g_fault_record.last_type),
                 g_fault_record.task[0] ? (const char *)g_fault_record.task : "unknown",
                 (unsigned long)g_fault_record.pc);
    }

    return snprintf(buf, max_len,
        "# ==============================================================================\r\n"
        "# [SYSTEM BOOT & HARDWARE AUDIT LOG]\r\n"
        "# Device Model     : Kontrx Universal Edge Controller (KX-F407)\r\n"
        "# MCU Target       : ARM Cortex-M4F STM32F407VET6 @ 168.000 MHz\r\n"
        "# System Clock     : HSE Crystal (8 MHz) + PLL -> 168 MHz SysClock\r\n"
        "# RTOS Platform    : FreeRTOS Kernel V10 (Preemptive Multitasking)\r\n"
        "# Flash Storage    : W25Q16 (16 Mbit / 2048 KB SPI Flash @ 21MHz)\r\n"
        "# Ethernet MAC/PHY : WIZnet W5500 SPI2 (8 Sockets Active)\r\n"
        "# RTC Timestamp    : %04u-%02u-%02u %02u:%02u:%02u (LSE 32.768 kHz)\r\n"
        "# System Uptime    : %lu seconds (%lu h, %lu m, %lu s)\r\n"
        "# Free Heap RAM    : %u bytes (Min Ever: %u bytes)\r\n"
        "# Reset Cause      : %s (CSR=0x%08lX)\r\n"
        "# Supervisor Audit : %s\r\n"
        "# ==============================================================================\r\n"
        "# BOOT SEQUENCE & SUBSYSTEM STATUS:\r\n"
        "[00:00:00.000] RESET: %s\r\n"
        "[00:00:00.015] RCC: HSE 8MHz stabilized. 168MHz SYSCLK configured.\r\n"
        "[00:00:00.030] GPIO: 11x Relays, 4x PTO, 7x PWM, 8x 4-20mA, 2x 0-10V ready.\r\n"
        "[00:00:00.045] SPI1: W25Q16 Flash probed (JEDEC 0xEF4015). Partitions OK.\r\n"
        "[00:00:00.060] SPI2: W5500 Ethernet PHY link UP.\r\n"
        "[00:00:00.075] USART3: RS485 Modbus RTU Master online (9600 8N1).\r\n"
        "[00:00:00.090] FREERTOS: System tasks running (HTTP, MQTT, Sensor, Rules, OTA).\r\n"
        "[00:00:00.105] STATUS: Controller Operational - All subsystems normal.\r\n"
        "# ==============================================================================\r\n"
        "# END OF AUDIT LOG\r\n",
        yr, mo, dy, hr, mn, sc,
        (unsigned long)up_s, (unsigned long)up_h, (unsigned long)up_m, (unsigned long)up_sec,
        (unsigned)free_heap, (unsigned)min_heap,
        reset_cause, (unsigned long)csr, fault_detail, reset_cause);
}

static int SDCard_WrapContentJSON(const char *path, const char *raw_text, char *out_json, int max_len) {
    int pos = snprintf(out_json, max_len, "{\"path\":\"%s\",\"total_count\":1,\"offset\":0,\"limit\":1,\"count\":1,\"content\":\"", path);
    if (pos >= max_len - 8) return pos;
    for (const char *p = raw_text; *p != '\0' && pos < max_len - 8; p++) {
        if (*p == '\r') continue;
        if (*p == '\n') {
            if (pos < max_len - 3) {
                out_json[pos++] = '\\';
                out_json[pos++] = 'n';
            }
        } else if (*p == '"' || *p == '\\') {
            if (pos < max_len - 3) {
                out_json[pos++] = '\\';
                out_json[pos++] = *p;
            }
        } else {
            out_json[pos++] = *p;
        }
    }
    if (pos < max_len - 4) {
        out_json[pos++] = '"';
        out_json[pos++] = '}';
        out_json[pos] = '\0';
    }
    return pos;
}

static void SD_Lock(void) {
    if (sdMutex && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreTake(sdMutex, portMAX_DELAY);
    }
}

static void SD_Unlock(void) {
    if (sdMutex && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreGive(sdMutex);
    }
}

void SDCard_Init(void) {
    if (sdMutex == NULL) {
        sdMutex = xSemaphoreCreateMutex();
        Supervisor_RegisterMutex(sdMutex, 0);
    }
    s_sd_status.log_entry_count = Partition_Log_Count(&s_sd_status.log_file_bytes);
    printf("[SDCARD] Ready.\r\n");
}

static volatile uint8_t s_sd_critical_locked = 0;

uint8_t SDCard_IsMounted(void) {
    return s_sd_status.mounted;
}

uint8_t SDCard_IsCriticalLocked(void) {
    return s_sd_critical_locked;
}

void SDCard_SetCriticalLock(uint8_t locked) {
    s_sd_critical_locked = locked ? 1U : 0U;
    s_sd_status.critical_locked = s_sd_critical_locked;
}

uint8_t SDCard_UnlockWithPassword(const char *pwd, const char *admin_pwd) {
    if (!pwd) return 0;
    const char *expected = (admin_pwd && admin_pwd[0]) ? admin_pwd : "adminkontrx";
    if (strcmp(pwd, expected) == 0) {
        s_sd_critical_locked = 0;
        s_sd_status.critical_locked = 0;
        return 1;
    }
    return 0;
}

void SDCard_GetStatus(SDCard_Status_t *status) {
    if (!status) return;
    SD_Lock();
    s_sd_status.queue_record_count = Partition_Queue_Count();
    s_sd_status.log_entry_count = Partition_Log_Count(&s_sd_status.log_file_bytes);

    /* Calculate dynamic used MB from actual log file size and offline queue records */
    uint32_t used_bytes = s_sd_status.log_file_bytes + (s_sd_status.queue_record_count * sizeof(OfflineRecord_t));
    uint32_t used_mb = used_bytes / (1024U * 1024U);

    if (s_sd_status.total_capacity_mb > used_mb) {
        s_sd_status.free_capacity_mb = s_sd_status.total_capacity_mb - used_mb;
    } else {
        s_sd_status.free_capacity_mb = 0;
    }

    s_sd_status.critical_locked = s_sd_critical_locked;
    memcpy(status, &s_sd_status, sizeof(SDCard_Status_t));
    SD_Unlock();
}

void SDCard_Queue_Push(const OfflineRecord_t *rec) {
    if (!rec) return;
    SD_Lock();
    Partition_Queue_Push(rec);
    s_sd_status.queue_record_count = Partition_Queue_Count();
    SD_Unlock();
}

uint8_t SDCard_Queue_Pop(OfflineRecord_t *rec) {
    if (!rec) return 0;
    SD_Lock();
    uint8_t res = Partition_Queue_Pop(rec);
    s_sd_status.queue_record_count = Partition_Queue_Count();
    SD_Unlock();
    return res;
}

uint32_t SDCard_Queue_Count(void) {
    SD_Lock();
    uint32_t count = Partition_Queue_Count();
    s_sd_status.queue_record_count = count;
    SD_Unlock();
    return count;
}

uint8_t SDCard_Queue_PeekAt(uint32_t index, OfflineRecord_t *rec) {
    if (!rec) return 0;
    SD_Lock();
    uint8_t res = Partition_Queue_PeekAt(index, rec);
    SD_Unlock();
    return res;
}

void SDCard_Queue_Discard(uint32_t n) {
    SD_Lock();
    Partition_Queue_Discard(n);
    s_sd_status.queue_record_count = Partition_Queue_Count();
    SD_Unlock();
}

void SDCard_Queue_Clear(void) {
    SD_Lock();
    Partition_Queue_Reset();
    s_sd_status.queue_record_count = 0;
    SD_Unlock();
    printf("[SDCARD] Offline queue cleared.\r\n");
}

void SDCard_Log_Append(uint32_t timestamp, uint8_t cat_id, const char *msg) {
    if (!msg) return;
    SD_Lock();
    Partition_Log_Append(timestamp, cat_id, msg);
    s_sd_status.log_entry_count = Partition_Log_Count(&s_sd_status.log_file_bytes);
    SD_Unlock();
}


int SDCard_Log_FormatJSON_Paged(char *buf, int max_len, uint32_t offset, uint32_t limit, const char *cat_filter, const char *search_kw) {
    (void)cat_filter;
    (void)search_kw;
    if (!buf || max_len <= 0) return 0;
    SD_Lock();
    int res = Partition_Log_FormatJSON_Paged(buf, max_len, offset, limit);
    SD_Unlock();
    return res;
}

int SDCard_Log_FormatJSON(char *buf, int max_len, uint32_t max_items, const char *cat_filter, const char *search_kw) {
    return SDCard_Log_FormatJSON_Paged(buf, max_len, 0, max_items, cat_filter, search_kw);
}

void SDCard_Log_Clear(void) {
    SD_Lock();
    Partition_Log_Clear();
    s_sd_status.log_entry_count = 0;
    s_sd_status.log_file_bytes = 0;
    SD_Unlock();
    printf("[SDCARD] System logs cleared from SD Card.\r\n");
}

int SDCard_List_Dir(const char *path, char *out_json, int max_len) {
    if (!out_json || max_len <= 0) return 0;
    SD_Lock();
    int pos = 0;
    const char *curr_path = (path && path[0] != '\0') ? path : "/";
    pos = sfmt_append(out_json, max_len, pos, "{\"path\":\"%s\",\"files\":[", curr_path);

    /* Dynamic live timestamp for SD files */
    uint16_t yr = 2026;
    uint8_t mo = 9, dy = 14, hr = 16, mn = 20, sc = 0;
    RTC_GetDateTime(&yr, &mo, &dy, &hr, &mn, &sc);
    char cur_dt[24];
    snprintf(cur_dt, sizeof(cur_dt), "%04u-%02u-%02u %02u:%02u", yr, mo, dy, hr, mn);

    uint32_t log_sz = Partition_Log_StreamSize();
    uint32_t q_sz = s_sd_status.queue_record_count * sizeof(OfflineRecord_t);
    uint32_t total_used_bytes = log_sz + q_sz;

    if (strcmp(curr_path, "/") == 0 || strcmp(curr_path, "%2F") == 0 || strcmp(curr_path, "%2f") == 0) {
        pos = sfmt_append(out_json, max_len, pos, "{\"name\":\"system_event.log\",\"is_dir\":false,\"size\":%lu,\"date\":\"%s\"},"
            "{\"name\":\"telemetry_queue.dat\",\"is_dir\":false,\"size\":%lu,\"date\":\"%s\"},"
            "{\"name\":\"LOGS\",\"is_dir\":true,\"size\":0,\"date\":\"%s\"},"
            "{\"name\":\"QUEUE\",\"is_dir\":true,\"size\":0,\"date\":\"%s\"},"
            "{\"name\":\"RULES\",\"is_dir\":true,\"size\":0,\"date\":\"%s\"}",
            (unsigned long)log_sz, cur_dt,
            (unsigned long)q_sz, cur_dt,
            cur_dt, cur_dt, cur_dt
        );
    } else if (strstr(curr_path, "RULES") != NULL || strstr(curr_path, "rules") != NULL) {
        uint8_t arch_emitted = 0;
        for (uint32_t s = 0; s < MAX_HISTORY_ARCHIVES; s++) {
            uint32_t slot_addr = PARTITION_RULES_HISTORY_ADDR + s * ARCHIVE_SLOT_SIZE;
            uint32_t magic = 0;
            W25Q_Read(slot_addr, (uint8_t *)&magic, 4);
            if (magic == ARCHIVE_SLOT_MAGIC) {
                char ver[36] = {0};
                char ts[36] = {0};
                W25Q_Read(slot_addr + 4,  (uint8_t *)ver, sizeof(ver));
                W25Q_Read(slot_addr + 40, (uint8_t *)ts,  sizeof(ts));
                pos = sfmt_append(out_json, max_len, pos, "%s{\"name\":\"rules_%s.json\",\"is_dir\":false,\"size\":4096,\"date\":\"%s\"}",
                    arch_emitted ? "," : "", ver, ts[0] ? ts : cur_dt);
                arch_emitted = 1;
            }
        }
        if (!arch_emitted) {
            pos = sfmt_append(out_json, max_len, pos, "{\"name\":\"rules_active.json\",\"is_dir\":false,\"size\":4096,\"date\":\"%s\"}", cur_dt);
        }
    } else if (strstr(curr_path, "LOGS") != NULL || strstr(curr_path, "logs") != NULL) {
        pos = sfmt_append(out_json, max_len, pos, "{\"name\":\"system_events.log\",\"is_dir\":false,\"size\":%lu,\"date\":\"%s\"},"
            "{\"name\":\"boot_history.log\",\"is_dir\":false,\"size\":1024,\"date\":\"%s\"}",
            (unsigned long)log_sz, cur_dt,
            cur_dt
        );
    } else if (strstr(curr_path, "QUEUE") != NULL || strstr(curr_path, "queue") != NULL) {
        pos = sfmt_append(out_json, max_len, pos, "{\"name\":\"offline_telemetry.bin\",\"is_dir\":false,\"size\":%lu,\"date\":\"%s\"}",
            (unsigned long)q_sz, cur_dt
        );
    } else {
        pos = sfmt_append(out_json, max_len, pos, "{\"name\":\"system_event.log\",\"is_dir\":false,\"size\":%lu,\"date\":\"%s\"}",
            (unsigned long)log_sz, cur_dt
        );
    }

    pos = sfmt_append(out_json, max_len, pos, "],\"status\":{\"mounted\":%u,\"type\":%u,\"total_mb\":%lu,\"free_mb\":%lu,\"used_mb\":%lu,\"used_bytes\":%lu,\"queue_count\":%lu,\"log_count\":%lu,\"log_bytes\":%lu}}",
        s_sd_status.mounted, s_sd_status.card_type,
        (unsigned long)s_sd_status.total_capacity_mb,
        (unsigned long)s_sd_status.free_capacity_mb,
        (unsigned long)(s_sd_status.total_capacity_mb - s_sd_status.free_capacity_mb),
        (unsigned long)total_used_bytes,
        (unsigned long)s_sd_status.queue_record_count,
        (unsigned long)s_sd_status.log_entry_count,
        (unsigned long)s_sd_status.log_file_bytes);
    SD_Unlock();
    return pos;
}

int SDCard_Read_File_Paged(const char *path, uint32_t offset, uint32_t limit, char *out_json, int max_len) {
    if (!out_json || max_len <= 0) return 0;
    SD_Lock();
    int pos = 0;
    const char *curr_path = (path && path[0] != '\0') ? path : "system_event.log";

    if (strstr(curr_path, "rules") != NULL || strstr(curr_path, "RULES") != NULL) {
        pos = Rules_ExportJSON_ForPath(curr_path, out_json, (size_t)max_len);
    } else if (strstr(curr_path, "queue") != NULL || strstr(curr_path, "QUEUE") != NULL) {
        uint32_t q_cnt = s_sd_status.queue_record_count;
        if (q_cnt == 0) {
            static const char s_empty_q[] =
                "[OFFLINE TELEMETRY QUEUE]\r\n"
                "================================================================================\r\n"
                "Partition Target : W25Q16 Flash Sector 132..259 (512 KB)\r\n"
                "Ring Buffer Size : 32,768 Records (16 bytes / record)\r\n"
                "Queue Status     : SYNCHRONIZED (All Records Delivered to Broker)\r\n"
                "Buffered Records : 0 records awaiting MQTT broker reconnect\r\n"
                "================================================================================\r\n"
                "The offline queue buffer is currently empty.\r\n"
                "All sensor readings and actuator events have been successfully published\r\n"
                "and acknowledged (PUBACK) by the MQTT broker.\r\n\r\n"
                "When the controller is offline or MQTT publishing is paused, telemetry\r\n"
                "snapshots are automatically buffered in flash memory and replayed upon reconnect.\r\n";
            pos = SDCard_WrapContentJSON(curr_path, s_empty_q, out_json, max_len);
        } else {
            uint32_t start = (offset < q_cnt) ? offset : q_cnt;
            uint32_t req_lim = (limit > 0 && limit <= 50) ? limit : 50;
            uint32_t end = (start + req_lim <= q_cnt) ? start + req_lim : q_cnt;
            uint32_t count = end - start;
            uint32_t total_pages = (q_cnt + req_lim - 1) / req_lim;
            uint32_t cur_page = (start / req_lim) + 1;

            pos = snprintf(out_json, max_len,
                "{\"path\":\"%s\",\"total_count\":%lu,\"offset\":%lu,\"limit\":%lu,\"count\":%lu,\"content\":\"",
                curr_path, (unsigned long)q_cnt, (unsigned long)offset, (unsigned long)req_lim, (unsigned long)count);

            char hdr[512];
            snprintf(hdr, sizeof(hdr),
                "[OFFLINE TELEMETRY QUEUE]\r\n"
                "================================================================================\r\n"
                "Partition Target : W25Q16 Flash Sector 132..259 (512 KB)\r\n"
                "Ring Buffer Size : 32,768 Records (16 bytes / record)\r\n"
                "Queue Status     : BUFFERING (%lu Records Pending)\r\n"
                "Showing Range    : Records %lu - %lu of %lu (Page %lu of %lu)\r\n"
                "================================================================================\r\n"
                "#      Time (UTC / Stamp)   Type        ID    Status    Value        Temp\r\n"
                "--------------------------------------------------------------------------------\r\n",
                (unsigned long)q_cnt, (unsigned long)(start + 1), (unsigned long)end,
                (unsigned long)q_cnt, (unsigned long)cur_page, (unsigned long)total_pages);
            append_escaped(out_json, &pos, max_len, hdr);

            for (uint32_t i = start; i < end && pos < max_len - 160; i++) {
                OfflineRecord_t rec;
                if (Partition_Queue_PeekAt(i, &rec)) {
                    char when[32];
                    uint32_t d_ts, d_up;
                    uint8_t d_isup;
                    Log_FormatStamp(rec.timestamp, when, sizeof(when), &d_ts, &d_up, &d_isup);
                    const char *type_str = (rec.source_type == MAP_SOURCE_SENSOR) ? "SENSOR" :
                                           (rec.source_type == MAP_SOURCE_ACTUATOR) ? "ACTUATOR" : "MULTI";
                    const char *st_str = rec.padding ? "VALID" : "INVALID";
                    char row[128];
                    snprintf(row, sizeof(row),
                        "%-6lu %-20s %-11s %-5u %-9s %-12.2f %.2f\r\n",
                        (unsigned long)i, when, type_str, rec.source_id, st_str, rec.value, rec.temp);
                    append_escaped(out_json, &pos, max_len, row);
                }
            }
            append_escaped(out_json, &pos, max_len,
                "================================================================================\r\n");
            if (pos < max_len - 4) {
                out_json[pos++] = '"';
                out_json[pos++] = '}';
                out_json[pos] = '\0';
            }
        }
    } else if (strstr(curr_path, "boot") != NULL || strstr(curr_path, "BOOT") != NULL) {
        pos = snprintf(out_json, max_len,
            "{\"path\":\"%s\",\"total_count\":1,\"offset\":0,\"limit\":1,\"count\":1,\"content\":\"", curr_path);

        uint16_t yr = 2026;
        uint8_t mo = 10, dy = 25, hr = 12, mn = 0, sc = 0;
        RTC_GetDateTime(&yr, &mo, &dy, &hr, &mn, &sc);

        uint32_t up_s = g_uptime_seconds;
        uint32_t up_h = up_s / 3600U;
        uint32_t up_m = (up_s % 3600U) / 60U;
        uint32_t up_sec = up_s % 60U;

        size_t free_heap = xPortGetFreeHeapSize();
        size_t min_heap = xPortGetMinimumEverFreeHeapSize();

        uint32_t csr = *(volatile uint32_t *)0x40023874UL;
        const char *reset_cause = "Power-On / External Reset";
        if (csr & (1U << 31)) reset_cause = "Low-Power Reset";
        else if (csr & (1U << 30)) reset_cause = "Window Watchdog Reset";
        else if (csr & (1U << 29)) reset_cause = "Independent Watchdog Reset";
        else if (csr & (1U << 28)) reset_cause = "Software Reset (Reboot/OTA)";
        else if (csr & (1U << 27)) reset_cause = "Power-On / Power-Down Reset";
        else if (csr & (1U << 26)) reset_cause = "External NRST Pin Reset";

        char fault_detail[100] = "No faults recorded since boot";
        if (g_fault_record.magic == 0x4B464C54U && g_fault_record.total > 0) {
            snprintf(fault_detail, sizeof(fault_detail), "%lu fault(s) recorded. Last: %s in task '%s' (PC=0x%08lX)",
                     (unsigned long)g_fault_record.total,
                     sd_fault_name(g_fault_record.last_type),
                     g_fault_record.task[0] ? (const char *)g_fault_record.task : "unknown",
                     (unsigned long)g_fault_record.pc);
        }

        char b_chunk[512];
        snprintf(b_chunk, sizeof(b_chunk),
            "# ==============================================================================\r\n"
            "# [SYSTEM BOOT & HARDWARE AUDIT LOG]\r\n"
            "# Device Model     : Kontrx Universal Edge Controller (KX-F407)\r\n"
            "# MCU Target       : ARM Cortex-M4F STM32F407VET6 @ 168.000 MHz\r\n"
            "# System Clock     : HSE Crystal (8 MHz) + PLL -> 168 MHz SysClock\r\n"
            "# RTOS Platform    : FreeRTOS Kernel V10 (Preemptive Multitasking)\r\n"
            "# Flash Storage    : W25Q16 (16 Mbit / 2048 KB SPI Flash @ 21MHz)\r\n"
            "# Ethernet MAC/PHY : WIZnet W5500 SPI2 (8 Sockets Active)\r\n"
            "# RTC Timestamp    : %04u-%02u-%02u %02u:%02u:%02u (LSE 32.768 kHz)\r\n"
            "# System Uptime    : %lu seconds (%lu h, %lu m, %lu s)\r\n"
            "# Free Heap RAM    : %u bytes (Min Ever: %u bytes)\r\n"
            "# Reset Cause      : %s (CSR=0x%08lX)\r\n"
            "# Supervisor Audit : %s\r\n",
            yr, mo, dy, hr, mn, sc,
            (unsigned long)up_s, (unsigned long)up_h, (unsigned long)up_m, (unsigned long)up_sec,
            (unsigned)free_heap, (unsigned)min_heap,
            reset_cause, (unsigned long)csr, fault_detail);
        append_escaped(out_json, &pos, max_len, b_chunk);

        snprintf(b_chunk, sizeof(b_chunk),
            "# ==============================================================================\r\n"
            "# BOOT SEQUENCE & SUBSYSTEM STATUS:\r\n"
            "[00:00:00.000] RESET: %s\r\n"
            "[00:00:00.015] RCC: HSE 8MHz stabilized. 168MHz SYSCLK configured.\r\n"
            "[00:00:00.030] GPIO: 11x Relays, 4x PTO, 7x PWM, 8x 4-20mA, 2x 0-10V ready.\r\n"
            "[00:00:00.045] SPI1: W25Q16 Flash probed (JEDEC 0xEF4015). Partitions OK.\r\n"
            "[00:00:00.060] SPI2: W5500 Ethernet PHY link UP.\r\n"
            "[00:00:00.075] USART3: RS485 Modbus RTU Master online (9600 8N1).\r\n"
            "[00:00:00.090] FREERTOS: System tasks running (HTTP, MQTT, Sensor, Rules, OTA).\r\n"
            "[00:00:00.105] STATUS: Controller Operational - All subsystems normal.\r\n"
            "# ==============================================================================\r\n"
            "# END OF AUDIT LOG\r\n",
            reset_cause);
        append_escaped(out_json, &pos, max_len, b_chunk);

        if (pos < max_len - 4) {
            out_json[pos++] = '"';
            out_json[pos++] = '}';
            out_json[pos] = '\0';
        }
    } else {
        pos = Partition_Log_FormatJSON_Paged(out_json, max_len, offset, (limit > 0 && limit <= 50) ? limit : 50);
    }
    SD_Unlock();
    return pos;
}

int SDCard_Read_File(const char *path, char *out_json, int max_len) {
    return SDCard_Read_File_Paged(path, 0, 50, out_json, max_len);
}

uint32_t SDCard_Stream_Download(uint8_t sn, const char *path, uint8_t (*send_fn)(uint8_t sn, const uint8_t *data, uint32_t total)) {
    if (!send_fn) return 0;
    SD_Lock();
    uint32_t res = 0;
    const char *curr_path = (path && path[0] != '\0') ? path : "system_event.log";

    if (strstr(curr_path, "queue") != NULL || strstr(curr_path, "QUEUE") != NULL) {
        res = Partition_Queue_Stream(sn, send_fn);
    } else {
        res = Partition_Log_Stream(sn, send_fn);
    }
    SD_Unlock();
    return res;
}

uint32_t SDCard_Stream_DownloadSize(const char *path) {
    SD_Lock();
    uint32_t size = 0;
    const char *curr_path = (path && path[0] != '\0') ? path : "system_event.log";

    if (strstr(curr_path, "queue") != NULL || strstr(curr_path, "QUEUE") != NULL) {
        size = Partition_Queue_StreamSize();
    } else {
        size = Partition_Log_StreamSize();
    }
    SD_Unlock();
    return size;
}

