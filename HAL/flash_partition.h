#ifndef FLASH_PARTITION_H
#define FLASH_PARTITION_H

#include <stdint.h>
#include "modbus_dma.h" // For Gateway_Config_t

/* W25Q16 Layout Addresses */
#define PARTITION_WEB_ADDR       0x00000000U
#define PARTITION_WEB_SIZE       0x00080000U // 512 KB (Sectors 0 to 127)

#define PARTITION_CONFIG_ADDR    0x00080000U
#define PARTITION_CONFIG_SIZE    0x00004000U // 16 KB (Sectors 128 to 131)
#define CONFIG_PRIMARY_ADDR      PARTITION_CONFIG_ADDR
#define CONFIG_BACKUP_ADDR       (PARTITION_CONFIG_ADDR + 0x1000U) // Sector 129
#define RULES_PRIMARY_ADDR       (PARTITION_CONFIG_ADDR + 0x2000U) // Sector 130 (0x00082000U)
#define RULES_BACKUP_ADDR        (PARTITION_CONFIG_ADDR + 0x3000U) // Sector 131 (0x00083000U)
#define MAX_RULES                16
#define RULES_MAGIC_CURRENT      0xC01D7778U // Increment magic due to structural refactoring

typedef struct {
    char     rule_id[36];
    char     input_id[36];
    char     operator[4];
    float    threshold;
    char     output_id[36];
    char     action[8];
    uint8_t  active;
    uint8_t  padding[3]; // Align to 4-byte boundary
} Rule_t;

typedef struct {
    uint32_t magic;
    char     version_id[36];
    char     timestamp[36];
    uint32_t rule_count;
    Rule_t   rules[MAX_RULES];
    uint8_t  rules_valid;       // 1 = Validated & Stable, 0 = Under test
    uint8_t  bypass_validation; // 1 = Bypass sensor online validation, 0 = Strict online checking
    uint8_t  padding[2];
    uint32_t checksum;
} RuleConfig_t;

#define PARTITION_QUEUE_ADDR     0x00084000U
#define PARTITION_QUEUE_SIZE     0x00080000U // 512 KB (Sectors 132 to 259)

#define PARTITION_LOG_ADDR       0x00104000U
#define PARTITION_LOG_SIZE       0x000FC000U // 1008 KB (Sectors 260 to 511)

/* Structure of a single telemetry record queued during broker disconnect */
typedef struct {
    uint32_t timestamp;      // RTC Epoch time
    uint8_t  source_type;    // MAP_SOURCE_SENSOR or MAP_SOURCE_ACTUATOR
    uint8_t  source_id;      // Sensor/Actuator ID
    uint8_t  valid;
    uint8_t  padding;
    float    value;
    float    temp;
} OfflineRecord_t;

/* Log header for persisted event logs on W25Q16 */
typedef struct {
    uint32_t timestamp;
    uint8_t  category_id;    // 0=SYS, 1=MQTT, 2=MODBUS, 3=RELAY, 4=OTA
    uint8_t  msg_len;
} PartitionLogHeader_t;

/* APIs */
void Partition_Init(void);
uint32_t Compute_CRC32(const uint8_t *data, uint32_t len);

// Configuration Storage (Dual-Sector Redundancy)
uint8_t Partition_LoadConfig(Gateway_Config_t *cfg);
uint8_t Partition_SaveConfig(const Gateway_Config_t *cfg);
uint8_t Partition_LoadRules(RuleConfig_t *rules);
uint8_t Partition_SaveRules(const RuleConfig_t *rules);
uint8_t Partition_BackupCurrentRules(void);

// Offline Telemetry Queue (FIFO Ring)
void Partition_Queue_Push(const OfflineRecord_t *rec);
uint8_t Partition_Queue_Pop(OfflineRecord_t *rec);
uint32_t Partition_Queue_Count(void);

// Persistent System Audit Logs (Wrap-around Logger)
void Partition_Log_Append(uint32_t timestamp, uint8_t cat_id, const char *msg);
// Read logs into JSON buffer for API output (max_items limit)
int Partition_Log_FormatJSON(char *buf, int max_len, uint32_t max_items);

#endif // FLASH_PARTITION_H
