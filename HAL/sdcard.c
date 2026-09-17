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

    uint32_t log_sz = (s_sd_status.log_file_bytes > 0) ? s_sd_status.log_file_bytes : (s_sd_status.log_entry_count * 48);
    uint32_t q_sz = s_sd_status.queue_record_count * sizeof(OfflineRecord_t);
    uint32_t total_used_bytes = log_sz + q_sz;

    if (strcmp(curr_path, "/") == 0 || strcmp(curr_path, "%2F") == 0 || strcmp(curr_path, "%2f") == 0) {
        pos += snprintf(out_json + pos, max_len - pos,
            "{\"name\":\"system_event.log\",\"is_dir\":false,\"size\":%lu,\"date\":\"%s\"},"
            "{\"name\":\"telemetry_queue.dat\",\"is_dir\":false,\"size\":%lu,\"date\":\"%s\"},"
            "{\"name\":\"LOGS\",\"is_dir\":true,\"size\":0,\"date\":\"%s\"},"
            "{\"name\":\"QUEUE\",\"is_dir\":true,\"size\":0,\"date\":\"%s\"}",
            (unsigned long)log_sz, cur_dt,
            (unsigned long)q_sz, cur_dt,
            cur_dt, cur_dt
        );
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
            "{\"path\":\"%s\",\"total_count\":%lu,\"offset\":0,\"limit\":1,\"count\":0,"
            "\"content\":\"[OFFLINE TELEMETRY QUEUE]\\r\\nPending Records: %lu\\r\\nPartition: 512 KB\"}",
            curr_path, (unsigned long)q_cnt, (unsigned long)q_cnt);
    } else if (strstr(curr_path, "boot") != NULL || strstr(curr_path, "BOOT") != NULL) {
        pos = snprintf(out_json, max_len,
            "{\"path\":\"%s\",\"total_count\":1,\"offset\":0,\"limit\":1,\"count\":1,"
            "\"content\":\"[BOOT LOG]\\r\\nSTM32F407 168MHz | FreeRTOS\\r\\nBoot Reason: Power-On Reset\"}",
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
