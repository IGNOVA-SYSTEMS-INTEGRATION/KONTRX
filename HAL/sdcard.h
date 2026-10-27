#ifndef SDCARD_H
#define SDCARD_H

#include <stdint.h>
#include <stddef.h>
#include "flash_partition.h" // For OfflineRecord_t

typedef struct {
    uint8_t  mounted;
    uint8_t  critical_locked; /* 1 if SD card disconnected/tampered and awaiting admin unlock */
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

/* Critical Security Lockdown for SD Card Disconnect / Tamper */
uint8_t SDCard_IsCriticalLocked(void);
void    SDCard_SetCriticalLock(uint8_t locked);
uint8_t SDCard_UnlockWithPassword(const char *pwd, const char *admin_pwd, const char *master_pwd);
uint8_t SDCard_IsOverrideActive(void);
void    SDCard_ClearOverride(void);

/* Physical Hardware Detection & Emergency Freeze */
uint8_t SDCard_Hardware_Check_Present(void);
uint8_t SDCard_Hardware_Poll_Lockout(void);
void    SDCard_Emergency_Freeze(void);
uint8_t SDCard_GetLastProbeResult(void);

/* Get live SD Card metrics */
void SDCard_GetStatus(SDCard_Status_t *status);

/* Offline Telemetry Queue on SD Card */
void SDCard_Queue_Push(const OfflineRecord_t *rec);
uint8_t SDCard_Queue_Pop(OfflineRecord_t *rec);
uint32_t SDCard_Queue_Count(void);
uint8_t SDCard_Queue_PeekAt(uint32_t index, OfflineRecord_t *rec);
void SDCard_Queue_Discard(uint32_t n);
void SDCard_Queue_Clear(void);

/* System-Wide Event Logger on SD Card */
void SDCard_Log_Append(uint32_t timestamp, uint8_t cat_id, const char *msg);
int SDCard_Log_FormatJSON(char *buf, int max_len, uint32_t max_items, const char *cat_filter, const char *search_kw);
int SDCard_Log_FormatJSON_Paged(char *buf, int max_len, uint32_t offset, uint32_t limit, const char *cat_filter, const char *search_kw);
void SDCard_Log_Clear(void);

/* SD Card Interactive Directory & File Explorer */
int SDCard_List_Dir(const char *path, char *out_json, int max_len);
int SDCard_Read_File(const char *path, char *out_json, int max_len);
int SDCard_Read_File_Paged(const char *path, uint32_t offset, uint32_t limit, char *out_json, int max_len);
uint32_t SDCard_Stream_Download(uint8_t sn, const char *path, uint8_t (*send_fn)(uint8_t sn, const uint8_t *data, uint32_t total));
uint32_t SDCard_Stream_DownloadSize(const char *path);

/* Boot Audit & Rules Export Helpers */
int SDCard_FormatBootAudit(char *buf, int max_len);
int Rules_ExportJSON_ForPath(const char *path, char *out_buf, size_t max_len);

#endif // SDCARD_H
