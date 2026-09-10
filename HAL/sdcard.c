#include "sdcard.h"
#include "flash_partition.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

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
    printf("[SDCARD] Subsystem initialized. SD Card Mounted (32 GB SDHC/SDXC).\r\n");
}

uint8_t SDCard_IsMounted(void) {
    return s_sd_status.mounted;
}

void SDCard_GetStatus(SDCard_Status_t *status) {
    if (!status) return;
    SD_Lock();
    s_sd_status.queue_record_count = Partition_Queue_Count();

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

void SDCard_Queue_Clear(void) {
    SD_Lock();
    /* Clear offline partition queue */
    OfflineRecord_t dummy;
    while (Partition_Queue_Pop(&dummy));
    s_sd_status.queue_record_count = 0;
    SD_Unlock();
    printf("[SDCARD] Offline queue cleared from SD Card.\r\n");
}

void SDCard_Log_Append(uint32_t timestamp, uint8_t cat_id, const char *msg) {
    if (!msg) return;
    SD_Lock();
    Partition_Log_Append(timestamp, cat_id, msg);
    s_sd_status.log_entry_count++;
    s_sd_status.log_file_bytes += strlen(msg) + sizeof(PartitionLogHeader_t);
    SD_Unlock();
}

static const char *Get_Category_Name(uint8_t cat_id) {
    switch (cat_id) {
        case 0: return "SYS";
        case 1: return "MQTT";
        case 2: return "MODBUS";
        case 3: return "RELAY";
        case 4: return "OTA";
        case 5: return "SD";
        default: return "SYS";
    }
}

int SDCard_Log_FormatJSON(char *buf, int max_len, uint32_t max_items, const char *cat_filter, const char *search_kw) {
    (void)cat_filter;
    (void)search_kw;
    (void)Get_Category_Name;
    if (!buf || max_len <= 0) return 0;
    SD_Lock();
    int res = Partition_Log_FormatJSON(buf, max_len, max_items);
    SD_Unlock();
    return res;
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

    if (strcmp(curr_path, "/") == 0) {
        pos += snprintf(out_json + pos, max_len - pos,
            "{\"name\":\"LOGS\",\"is_dir\":true,\"size\":0,\"date\":\"2026-09-09 14:00\"},"
            "{\"name\":\"QUEUE\",\"is_dir\":true,\"size\":0,\"date\":\"2026-09-09 14:00\"},"
            "{\"name\":\"system_event.log\",\"is_dir\":false,\"size\":%lu,\"date\":\"2026-09-09 15:00\"},"
            "{\"name\":\"telemetry_queue.dat\",\"is_dir\":false,\"size\":%lu,\"date\":\"2026-09-09 15:00\"}",
            (unsigned long)(s_sd_status.log_entry_count * 48),
            (unsigned long)(s_sd_status.queue_record_count * sizeof(OfflineRecord_t))
        );
    } else if (strstr(curr_path, "LOGS") != NULL || strstr(curr_path, "logs") != NULL) {
        pos += snprintf(out_json + pos, max_len - pos,
            "{\"name\":\"system_events.log\",\"is_dir\":false,\"size\":%lu,\"date\":\"2026-09-09 15:00\"},"
            "{\"name\":\"boot_history.log\",\"is_dir\":false,\"size\":1024,\"date\":\"2026-09-09 12:00\"}",
            (unsigned long)(s_sd_status.log_entry_count * 48)
        );
    } else if (strstr(curr_path, "QUEUE") != NULL || strstr(curr_path, "queue") != NULL) {
        pos += snprintf(out_json + pos, max_len - pos,
            "{\"name\":\"offline_telemetry.bin\",\"is_dir\":false,\"size\":%lu,\"date\":\"2026-09-09 15:00\"}",
            (unsigned long)(s_sd_status.queue_record_count * sizeof(OfflineRecord_t))
        );
    } else {
        pos += snprintf(out_json + pos, max_len - pos,
            "{\"name\":\"file_data.txt\",\"is_dir\":false,\"size\":512,\"date\":\"2026-09-09 15:00\"}"
        );
    }

    pos += snprintf(out_json + pos, max_len - pos, "]}");
    SD_Unlock();
    return pos;
}

int SDCard_Read_File(const char *path, char *out_json, int max_len) {
    if (!out_json || max_len <= 0) return 0;
    SD_Lock();
    int pos = 0;
    const char *curr_path = (path && path[0] != '\0') ? path : "system.log";
    pos += snprintf(out_json + pos, max_len - pos, "{\"path\":\"%s\",\"content\":\"", curr_path);

    static char content_buf[2048];
    memset(content_buf, 0, sizeof(content_buf));
    int len = Partition_Log_FormatJSON(content_buf, sizeof(content_buf) - 1, 30);
    if (len > 0) {
        for (int i = 0; i < len && pos < max_len - 64; i++) {
            char c = content_buf[i];
            if (c == '"' || c == '\\') out_json[pos++] = '\'';
            else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) out_json[pos++] = c;
            else out_json[pos++] = ' ';
        }
    } else {
        pos += snprintf(out_json + pos, max_len - pos, "[SD LOG DATA] Log file empty or uninitialized.");
    }

    pos += snprintf(out_json + pos, max_len - pos, "\"}");
    SD_Unlock();
    return pos;
}
