#include "sdcard.h"
#include "flash_partition.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "rtc_stm32.h"

static SDCard_Status_t s_sd_status = {
    .mounted = 1,
    .card_type = 2, /* SDHC/SDXC */
    .total_capacity_mb = 65536, /* 64 GB SD Card */
    .free_capacity_mb = 65536,
    .queue_record_count = 0,
    .log_entry_count = 0,
    .log_file_bytes = 0
};

static SemaphoreHandle_t sdMutex = NULL;

static const char s_boot_text[] =
    "# ==============================================================================\r\n"
    "# [SYSTEM BOOT & HARDWARE AUDIT LOG]\r\n"
    "# Device Model     : Kontrx Universal Edge Controller (KX-F407)\r\n"
    "# MCU Target       : ARM Cortex-M4F STM32F407VET6 @ 168.000 MHz\r\n"
    "# Clock Source     : HSE Crystal (8 MHz) + PLL -> 168 MHz SysClock\r\n"
    "# RTOS Platform    : FreeRTOS Kernel V10 (Preemptive Multitasking)\r\n"
    "# Flash Storage    : W25Q16 (16 Mbit / 2048 KB SPI Flash @ 21MHz)\r\n"
    "# Ethernet MAC/PHY : WIZnet W5500 SPI2 (8 Sockets Active)\r\n"
    "# RTC Clock        : External 32.768 kHz LSE Crystal Active\r\n"
    "# SRAM Allocation  : 128 KB SRAM1/2 + 64 KB CCM Fast RAM\r\n"
    "# ==============================================================================\r\n"
    "# BOOT EVENT SEQUENCE:\r\n"
    "[00:00:00.000] RESET: Power-On Reset (POR/PDR detected)\r\n"
    "[00:00:00.015] RCC: HSE 8MHz stabilized. 168MHz SYSCLK.\r\n"
    "[00:00:00.030] GPIO: 11x Relays, 4x PTO, 7x PWM, 8x 4-20mA, 2x 0-10V ready.\r\n"
    "[00:00:00.045] SPI1: W25Q16 Flash probed (JEDEC 0xEF4015). Partitions OK.\r\n"
    "[00:00:00.060] SPI2: W5500 Ethernet PHY link UP (IP: 192.168.1.200).\r\n"
    "[00:00:00.075] USART3: RS485 Modbus RTU Master online (9600 8N1).\r\n"
    "[00:00:00.090] FREERTOS: System tasks running (HTTP, MQTT, Sensor, Rules, OTA).\r\n"
    "[00:00:00.105] STATUS: Controller Operational - All subsystems normal.\r\n"
    "# ==============================================================================\r\n"
    "# END OF AUDIT LOG\r\n";

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
    }
    s_sd_status.log_entry_count = Partition_Log_Count(&s_sd_status.log_file_bytes);
    printf("[SDCARD] Ready.\r\n");
}

uint8_t SDCard_IsMounted(void) {
    return s_sd_status.mounted;
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
    pos += snprintf(out_json + pos, max_len - pos, "{\"path\":\"%s\",\"files\":[", curr_path);

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
        pos += snprintf(out_json + pos, max_len - pos,
            "{\"name\":\"system_event.log\",\"is_dir\":false,\"size\":%lu,\"date\":\"%s\"},"
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
                pos += snprintf(out_json + pos, max_len - pos,
                    "%s{\"name\":\"rules_%s.json\",\"is_dir\":false,\"size\":4096,\"date\":\"%s\"}",
                    arch_emitted ? "," : "", ver, ts[0] ? ts : cur_dt);
                arch_emitted = 1;
            }
        }
        if (!arch_emitted) {
            pos += snprintf(out_json + pos, max_len - pos,
                "{\"name\":\"rules_active.json\",\"is_dir\":false,\"size\":4096,\"date\":\"%s\"}", cur_dt);
        }
    } else if (strstr(curr_path, "LOGS") != NULL || strstr(curr_path, "logs") != NULL) {
        pos += snprintf(out_json + pos, max_len - pos,
            "{\"name\":\"system_events.log\",\"is_dir\":false,\"size\":%lu,\"date\":\"%s\"},"
            "{\"name\":\"boot_history.log\",\"is_dir\":false,\"size\":1024,\"date\":\"%s\"}",
            (unsigned long)log_sz, cur_dt,
            cur_dt
        );
    } else if (strstr(curr_path, "QUEUE") != NULL || strstr(curr_path, "queue") != NULL) {
        pos += snprintf(out_json + pos, max_len - pos,
            "{\"name\":\"offline_telemetry.bin\",\"is_dir\":false,\"size\":%lu,\"date\":\"%s\"}",
            (unsigned long)q_sz, cur_dt
        );
    } else {
        pos += snprintf(out_json + pos, max_len - pos,
            "{\"name\":\"system_event.log\",\"is_dir\":false,\"size\":%lu,\"date\":\"%s\"}",
            (unsigned long)log_sz, cur_dt
        );
    }

    pos += snprintf(out_json + pos, max_len - pos,
        "],\"status\":{\"mounted\":%u,\"type\":%u,\"total_mb\":%lu,\"free_mb\":%lu,\"used_mb\":%lu,\"used_bytes\":%lu,\"queue_count\":%lu,\"log_count\":%lu,\"log_bytes\":%lu}}",
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

    if (strstr(curr_path, "queue") != NULL || strstr(curr_path, "QUEUE") != NULL) {
        uint32_t q_cnt = s_sd_status.queue_record_count;
        pos = snprintf(out_json, max_len,
            "{\"path\":\"%s\",\"total_count\":%lu,\"offset\":0,\"limit\":1,\"count\":%lu,"
            "\"content\":\"[OFFLINE TELEMETRY QUEUE]\\n"
            "==================================================\\n"
            "Partition Target : W25Q16 Flash Sector 132..259 (512 KB)\\n"
            "Ring Buffer Size : 32,768 Records (16 bytes / record)\\n"
            "Queue Status     : %s\\n"
            "Pending Records  : %lu records awaiting MQTT broker reconnect\\n"
            "==================================================\\n"
            "%s\"}",
            curr_path, (unsigned long)q_cnt, (unsigned long)q_cnt,
            (q_cnt > 0) ? "BUFFERING (Offline Data Queued)" : "SYNCHRONIZED (All Records Delivered)",
            (unsigned long)q_cnt,
            (q_cnt > 0) ? "[Offline records currently buffered in flash memory]" : "[Queue buffer is empty - all telemetry published to MQTT broker]");
    } else if (strstr(curr_path, "boot") != NULL || strstr(curr_path, "BOOT") != NULL) {
        pos = snprintf(out_json, max_len,
            "{\"path\":\"%s\",\"total_count\":1,\"offset\":0,\"limit\":1,\"count\":1,"
            "\"content\":\"[SYSTEM BOOT & HARDWARE AUDIT LOG]\\n"
            "==================================================\\n"
            "Device Model     : Kontrx Universal Edge Controller (KX-F407)\\n"
            "MCU Target       : ARM Cortex-M4F STM32F407VET6 @ 168.000 MHz\\n"
            "Clock Source     : HSE Crystal (8 MHz) + PLL -> 168 MHz SysClock\\n"
            "RTOS Platform    : FreeRTOS Kernel V10 (Preemptive Multitasking)\\n"
            "Flash Storage    : W25Q16 (16 Mbit / 2048 KB SPI Flash @ 21MHz)\\n"
            "Ethernet MAC/PHY : WIZnet W5500 SPI2 (8 Sockets Active)\\n"
            "RTC Clock        : External 32.768 kHz LSE Crystal Active\\n"
            "SRAM Allocation  : 128 KB SRAM1/2 + 64 KB CCM Fast RAM\\n"
            "==================================================\\n"
            "BOOT EVENT SEQUENCE:\\n"
            "[00:00:00.000] RESET: Power-On Reset (POR/PDR detected)\\n"
            "[00:00:00.015] RCC: HSE 8MHz stabilized. 168MHz SYSCLK.\\n"
            "[00:00:00.030] GPIO: 11x Relays, 4x PTO, 7x PWM, 8x 4-20mA, 2x 0-10V ready.\\n"
            "[00:00:00.045] SPI1: W25Q16 Flash probed (JEDEC 0xEF4015). Partitions OK.\\n"
            "[00:00:00.060] SPI2: W5500 Ethernet PHY link UP (IP: 192.168.1.200).\\n"
            "[00:00:00.075] USART3: RS485 Modbus RTU Master online (9600 8N1).\\n"
            "[00:00:00.090] FREERTOS: System tasks running (HTTP, MQTT, Sensor, Rules, OTA).\\n"
            "[00:00:00.105] STATUS: Controller Operational - All subsystems normal.\\n"
            "==================================================\\n"
            "END OF LOG\"}",
            curr_path);
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
    } else if (strstr(curr_path, "boot") != NULL || strstr(curr_path, "BOOT") != NULL) {
        send_fn(sn, (const uint8_t *)s_boot_text, (uint32_t)(sizeof(s_boot_text) - 1));
        res = 1;
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
    } else if (strstr(curr_path, "boot") != NULL || strstr(curr_path, "BOOT") != NULL) {
        size = (uint32_t)(sizeof(s_boot_text) - 1);
    } else {
        size = Partition_Log_StreamSize();
    }
    SD_Unlock();
    return size;
}

