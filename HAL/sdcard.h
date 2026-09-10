#ifndef SDCARD_H
#define SDCARD_H

#include <stdint.h>
#include <stddef.h>
#include "flash_partition.h" // For OfflineRecord_t

typedef struct {
    uint8_t  mounted;
    uint8_t  card_type; /* 0=None, 1=SDSC, 2=SDHC/SDXC */
    uint32_t total_capacity_mb;
    uint32_t free_capacity_mb;
    uint32_t queue_record_count;
    uint32_t log_entry_count;
    uint32_t log_file_bytes;
} SDCard_Status_t;

/* Initialize SD Card storage subsystem */
void SDCard_Init(void);

/* Check if SD Card is mounted & operational */
uint8_t SDCard_IsMounted(void);

/* Get live SD Card metrics */
void SDCard_GetStatus(SDCard_Status_t *status);

/* Offline Telemetry Queue on SD Card */
void SDCard_Queue_Push(const OfflineRecord_t *rec);
uint8_t SDCard_Queue_Pop(OfflineRecord_t *rec);
uint32_t SDCard_Queue_Count(void);
void SDCard_Queue_Clear(void);

/* System-Wide Event Logger on SD Card */
void SDCard_Log_Append(uint32_t timestamp, uint8_t cat_id, const char *msg);
int SDCard_Log_FormatJSON(char *buf, int max_len, uint32_t max_items, const char *cat_filter, const char *search_kw);
void SDCard_Log_Clear(void);

/* SD Card Interactive Directory & File Explorer */
int SDCard_List_Dir(const char *path, char *out_json, int max_len);
int SDCard_Read_File(const char *path, char *out_json, int max_len);

#endif // SDCARD_H
