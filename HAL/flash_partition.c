#include "flash_partition.h"
#include "w25q16.h"
#include <stdio.h>
#include <string.h>

/* Initialisation guard — cleared until Partition_Init() completes */
static volatile uint8_t g_partition_ready = 0;

/* Queue variables */
static uint32_t q_read_ptr = 0;
static uint32_t q_write_ptr = 0;

/* Log variables */
static uint32_t log_write_addr = PARTITION_LOG_ADDR;
static uint32_t log_read_addr = PARTITION_LOG_ADDR;

/* CRC32 Helper for Configuration Verification */
uint32_t Compute_CRC32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1U) crc = (crc >> 1) ^ 0xEDB88320U;
            else          crc >>= 1;
        }
    }
    return ~crc;
}

void Partition_Init(void) {
    // 1. Initialize physical SPI and W25Q16 flash
    W25Q_Init();

    // 2. Scan Offline Telemetry Queue to locate read & write indices
    uint32_t first_empty = 0xFFFFFFFF;
    uint32_t first_active = 0xFFFFFFFF;
    
    // We have 32768 slots of 16 bytes each in the 512KB queue partition
    for (uint32_t i = 0; i < 32768; i++) {
        uint32_t addr = PARTITION_QUEUE_ADDR + i * 16;
        uint8_t buf[8];
        W25Q_Read(addr, buf, 8);
        
        uint32_t ts = (buf[3] << 24) | (buf[2] << 16) | (buf[1] << 8) | buf[0];
        if (ts != 0xFFFFFFFF) {
            uint8_t valid_val = buf[6];
            if (valid_val == 0xAA) {
                if (first_active == 0xFFFFFFFF) {
                    first_active = i;
                }
            }
        } else {
            if (first_empty == 0xFFFFFFFF) {
                first_empty = i;
            }
        }
    }
    
    if (first_empty == 0xFFFFFFFF) q_write_ptr = 0;
    else                            q_write_ptr = first_empty;
    
    if (first_active == 0xFFFFFFFF) q_read_ptr = q_write_ptr;
    else                            q_read_ptr = first_active;
    
    printf("[Partition] Queue initialized: read_ptr=%lu, write_ptr=%lu\r\n", 
           (unsigned long)q_read_ptr, (unsigned long)q_write_ptr);

    // 3. Scan Log Partition to find current write address
    // Each log sector starts with PartitionLogHeader_t.
    // We scan in 4KB steps to find the active/empty sector.
    uint32_t active_sector = PARTITION_LOG_ADDR;
    for (uint32_t sec = 0; sec < 252; sec++) {
        uint32_t s_addr = PARTITION_LOG_ADDR + sec * 4096;
        uint32_t marker = 0xFFFFFFFF;
        W25Q_Read(s_addr, (uint8_t *)&marker, 4);
        if (marker == 0xFFFFFFFF) {
            active_sector = s_addr;
            break;
        }
    }
    
    // Scan within the active sector to find the exact free byte offset
    log_write_addr = active_sector;
    uint32_t offset = 0;
    while (offset < 4096 - sizeof(PartitionLogHeader_t)) {
        PartitionLogHeader_t hdr;
        W25Q_Read(log_write_addr + offset, (uint8_t *)&hdr, sizeof(PartitionLogHeader_t));
        if (hdr.timestamp == 0xFFFFFFFF) {
            log_write_addr += offset;
            break;
        }
        offset += sizeof(PartitionLogHeader_t) + hdr.msg_len;
    }
    
    log_read_addr = PARTITION_LOG_ADDR;
    g_partition_ready = 1;
    printf("[Partition] Historical Log initialized: write_addr=0x%08X\r\n", (unsigned int)log_write_addr);
}

/* ======================================================================
 *  Gateway Configuration Storage (Dual-Sector Redundancy)
 * ====================================================================== */
uint8_t Partition_LoadConfig(Gateway_Config_t *cfg) {
    static Gateway_Config_t temp;
    
    // Try Primary Config (Sector 128)
    W25Q_Read(CONFIG_PRIMARY_ADDR, (uint8_t *)&temp, sizeof(Gateway_Config_t));
    uint32_t cal_crc = Compute_CRC32((const uint8_t *)&temp, offsetof(Gateway_Config_t, checksum));
    
    if (temp.magic == CONFIG_MAGIC_CURRENT && temp.checksum == cal_crc) {
        memcpy(cfg, &temp, sizeof(Gateway_Config_t));
        return 1; // Primary config loaded successfully
    }
    
    printf("[Partition] Primary config CRC/Magic mismatch! Trying backup...\r\n");

    // Try Backup Config (Sector 129)
    W25Q_Read(CONFIG_BACKUP_ADDR, (uint8_t *)&temp, sizeof(Gateway_Config_t));
    cal_crc = Compute_CRC32((const uint8_t *)&temp, offsetof(Gateway_Config_t, checksum));
    
    if (temp.magic == CONFIG_MAGIC_CURRENT && temp.checksum == cal_crc) {
        // Restore Primary from Backup
        W25Q_EraseSector(CONFIG_PRIMARY_ADDR);
        W25Q_Write(CONFIG_PRIMARY_ADDR, (const uint8_t *)&temp, sizeof(Gateway_Config_t));
        memcpy(cfg, &temp, sizeof(Gateway_Config_t));
        printf("[Partition] Restored primary config from backup sector.\r\n");
        return 1;
    }
    
    printf("[Partition] Backup config also corrupted or missing!\r\n");
    return 0; // Config failed to load
}

uint8_t Partition_SaveConfig(const Gateway_Config_t *cfg) {
    static Gateway_Config_t temp;
    memcpy(&temp, cfg, sizeof(Gateway_Config_t));
    
    temp.magic = CONFIG_MAGIC_CURRENT;
    temp.checksum = Compute_CRC32((const uint8_t *)&temp, offsetof(Gateway_Config_t, checksum));
    
    // Write Primary
    W25Q_EraseSector(CONFIG_PRIMARY_ADDR);
    W25Q_Write(CONFIG_PRIMARY_ADDR, (const uint8_t *)&temp, sizeof(Gateway_Config_t));
    
    // Write Backup
    W25Q_EraseSector(CONFIG_BACKUP_ADDR);
    W25Q_Write(CONFIG_BACKUP_ADDR, (const uint8_t *)&temp, sizeof(Gateway_Config_t));
    
    printf("[Partition] Configurations saved and mirrored in flash.\r\n");
    return 1;
}

uint8_t Partition_LoadRules(RuleConfig_t *rules) {
    static RuleConfig_t temp;
    
    // Try Primary Rules (Sector 130)
    W25Q_Read(RULES_PRIMARY_ADDR, (uint8_t *)&temp, sizeof(RuleConfig_t));
    uint32_t cal_crc = Compute_CRC32((const uint8_t *)&temp, offsetof(RuleConfig_t, checksum));
    
    if (temp.magic == RULES_MAGIC_CURRENT && temp.checksum == cal_crc && temp.rules_valid == 1) {
        memcpy(rules, &temp, sizeof(RuleConfig_t));
        return 1; // Primary config loaded successfully
    }
    
    if (temp.magic == RULES_MAGIC_CURRENT && temp.checksum == cal_crc && temp.rules_valid == 0) {
        printf("[Rules] Primary rules version %s were marked invalid/unstable (crashed)! Reverting to backup...\r\n", temp.version_id);
    } else {
        printf("[Rules] Primary rules CRC/Magic mismatch! Reverting to backup...\r\n");
    }

    // Try Backup Rules (Sector 131)
    W25Q_Read(RULES_BACKUP_ADDR, (uint8_t *)&temp, sizeof(RuleConfig_t));
    cal_crc = Compute_CRC32((const uint8_t *)&temp, offsetof(RuleConfig_t, checksum));
    
    if (temp.magic == RULES_MAGIC_CURRENT && temp.checksum == cal_crc && temp.rules_valid == 1) {
        // Restore Primary from Backup
        W25Q_EraseSector(RULES_PRIMARY_ADDR);
        W25Q_Write(RULES_PRIMARY_ADDR, (const uint8_t *)&temp, sizeof(RuleConfig_t));
        memcpy(rules, &temp, sizeof(RuleConfig_t));
        printf("[Rules] Restored primary rules from backup sector (version_id %s).\r\n", temp.version_id);
        
        char fallback_msg[64];
        snprintf(fallback_msg, sizeof(fallback_msg), "Recovered rules from backup. Active version: %s.", temp.version_id);
        Partition_Log_Append(0, 0, fallback_msg); // SYS log event
        return 1;
    }
    
    printf("[Rules] Backup rules also corrupted, missing, or unstable! Resetting to defaults...\r\n");
    memset(rules, 0, sizeof(RuleConfig_t));
    rules->magic = RULES_MAGIC_CURRENT;
    strcpy(rules->version_id, "default");
    strcpy(rules->timestamp, "2026-08-30T00:00:00.000Z");
    rules->rule_count = 0;
    rules->rules_valid = 1;
    Partition_SaveRules(rules);
    return 0;
}

uint8_t Partition_SaveRules(const RuleConfig_t *rules) {
    static RuleConfig_t temp;
    memcpy(&temp, rules, sizeof(RuleConfig_t));
    
    temp.magic = RULES_MAGIC_CURRENT;
    temp.checksum = Compute_CRC32((const uint8_t *)&temp, offsetof(RuleConfig_t, checksum));
    
    if (temp.rules_valid == 1) {
        // When we save a validated configuration, we mirror it in BOTH partitions
        W25Q_EraseSector(RULES_PRIMARY_ADDR);
        W25Q_Write(RULES_PRIMARY_ADDR, (const uint8_t *)&temp, sizeof(RuleConfig_t));
        
        W25Q_EraseSector(RULES_BACKUP_ADDR);
        W25Q_Write(RULES_BACKUP_ADDR, (const uint8_t *)&temp, sizeof(RuleConfig_t));
        printf("[Rules] Saved and mirrored validated rules (version_id %s) in flash.\r\n", temp.version_id);
    } else {
        // If we save it as unvalidated (during update/testing), we only write it to the PRIMARY
        // sector so that the backup sector retains the old working copy!
        W25Q_EraseSector(RULES_PRIMARY_ADDR);
        W25Q_Write(RULES_PRIMARY_ADDR, (const uint8_t *)&temp, sizeof(RuleConfig_t));
        printf("[Rules] Saved unvalidated rules (version_id %s) to primary flash sector.\r\n", temp.version_id);
    }
    return 1;
}

uint8_t Partition_BackupCurrentRules(void) {
    static RuleConfig_t temp;
    W25Q_Read(RULES_PRIMARY_ADDR, (uint8_t *)&temp, sizeof(RuleConfig_t));
    uint32_t cal_crc = Compute_CRC32((const uint8_t *)&temp, offsetof(RuleConfig_t, checksum));
    if (temp.magic == RULES_MAGIC_CURRENT && temp.checksum == cal_crc && temp.rules_valid == 1) {
        W25Q_EraseSector(RULES_BACKUP_ADDR);
        W25Q_Write(RULES_BACKUP_ADDR, (const uint8_t *)&temp, sizeof(RuleConfig_t));
        printf("[Rules] Backup updated with version_id %s.\r\n", temp.version_id);
        return 1;
    }
    return 0;
}

/* ======================================================================
 *  Offline Telemetry Queue (FIFO Ring)
 * ====================================================================== */
void Partition_Queue_Push(const OfflineRecord_t *rec) {
    uint32_t addr = PARTITION_QUEUE_ADDR + q_write_ptr * 16;
    
    // If we are at the beginning of a 4KB sector, we must erase it
    if ((addr % 4096) == 0) {
        W25Q_EraseSector(addr);
    }
    
    OfflineRecord_t copy = *rec;
    copy.valid = 0xAA; // 0xAA = Active/Unprocessed
    copy.padding = 0;
    
    W25Q_Write(addr, (const uint8_t *)&copy, sizeof(OfflineRecord_t));
    q_write_ptr = (q_write_ptr + 1) % 32768;
    
    // If queue write wraps and hits read pointer, advance read pointer (reclaim/overwrite)
    if (q_write_ptr == q_read_ptr) {
        q_read_ptr = (q_read_ptr + 1) % 32768;
    }
}

uint8_t Partition_Queue_Pop(OfflineRecord_t *rec) {
    if (q_read_ptr == q_write_ptr) {
        return 0; // Empty
    }
    
    uint32_t addr = PARTITION_QUEUE_ADDR + q_read_ptr * 16;
    W25Q_Read(addr, (uint8_t *)rec, sizeof(OfflineRecord_t));
    
    // Mark record as processed (0x55) in flash (without erasing)
    uint8_t processed_val = 0x55;
    W25Q_Write(addr + 6, &processed_val, 1);
    
    q_read_ptr = (q_read_ptr + 1) % 32768;
    return 1;
}

uint32_t Partition_Queue_Count(void) {
    if (q_write_ptr >= q_read_ptr) {
        return q_write_ptr - q_read_ptr;
    } else {
        return (32768 - q_read_ptr) + q_write_ptr;
    }
}

/* ======================================================================
 *  Persistent System Logs (Wrap-around Event Logger)
 * ====================================================================== */
void Partition_Log_Append(uint32_t timestamp, uint8_t cat_id, const char *msg) {
    /* Skip flash write if partition manager is not yet initialised */
    if (!g_partition_ready) return;

    uint32_t msg_len = strlen(msg);
    if (msg_len > 60) msg_len = 60; // Truncate to match header sizes
    
    uint32_t req_len = sizeof(PartitionLogHeader_t) + msg_len;
    
    // If we exceed the current sector boundary, wrap to the next sector
    uint32_t sector_offset = log_write_addr % 4096;
    if (sector_offset + req_len > 4096) {
        // Erase next sector before writing
        uint32_t next_sec = log_write_addr - sector_offset + 4096;
        if (next_sec >= PARTITION_LOG_ADDR + PARTITION_LOG_SIZE) {
            next_sec = PARTITION_LOG_ADDR;
        }
        W25Q_EraseSector(next_sec);
        log_write_addr = next_sec;
    }
    
    PartitionLogHeader_t hdr = {
        .timestamp = timestamp,
        .category_id = cat_id,
        .msg_len = (uint8_t)msg_len
    };
    
    W25Q_Write(log_write_addr, (const uint8_t *)&hdr, sizeof(PartitionLogHeader_t));
    W25Q_Write(log_write_addr + sizeof(PartitionLogHeader_t), (const uint8_t *)msg, msg_len);
    
    log_write_addr += req_len;
    
    // Wrap around whole partition if needed
    if (log_write_addr >= PARTITION_LOG_ADDR + PARTITION_LOG_SIZE) {
        log_write_addr = PARTITION_LOG_ADDR;
        W25Q_EraseSector(log_write_addr);
    }
}

int Partition_Log_FormatJSON(char *buf, int max_len, uint32_t max_items) {
    int pos = 0;
    pos += snprintf(buf + pos, max_len - pos, "{\"logs\":[");
    
    uint32_t count = 0;
    uint8_t printed = 0;
    
    // Read up to max_items logs going backwards from write_addr
    uint32_t read_start = PARTITION_LOG_ADDR;
    // Walk through active logs
    while (read_start < PARTITION_LOG_ADDR + PARTITION_LOG_SIZE && count < max_items) {
        PartitionLogHeader_t hdr;
        W25Q_Read(read_start, (uint8_t *)&hdr, sizeof(PartitionLogHeader_t));
        
        if (hdr.timestamp == 0xFFFFFFFF || hdr.timestamp == 0) {
            break; // No more logged items
        }
        
        char msg_temp[64];
        uint32_t len = hdr.msg_len;
        if (len >= sizeof(msg_temp)) len = sizeof(msg_temp) - 1;
        W25Q_Read(read_start + sizeof(PartitionLogHeader_t), (uint8_t *)msg_temp, len);
        msg_temp[len] = '\0';
        
        uint32_t s = hdr.timestamp;
        uint32_t hrs = s / 3600;
        uint32_t mins = (s % 3600) / 60;
        uint32_t secs = s % 60;
        
        const char *cat_str = "SYS";
        if (hdr.category_id == 1) cat_str = "MQTT";
        else if (hdr.category_id == 2) cat_str = "MODBUS";
        else if (hdr.category_id == 3) cat_str = "RELAY";
        else if (hdr.category_id == 4) cat_str = "OTA";
        
        if (printed > 0) {
            pos += snprintf(buf + pos, max_len - pos, ",");
        }
        
        pos += snprintf(buf + pos, max_len - pos,
                        "{\"time\":\"%02lu:%02lu:%02lu\",\"cat\":\"%s\",\"msg\":\"%s\"}",
                        (unsigned long)hrs, (unsigned long)mins, (unsigned long)secs,
                        cat_str, msg_temp);
        
        printed++;
        count++;
        
        read_start += sizeof(PartitionLogHeader_t) + hdr.msg_len;
        if (read_start % 4096 > 4096 - sizeof(PartitionLogHeader_t)) {
            read_start = (read_start - (read_start % 4096)) + 4096; // Jump to next sector
        }
        
        if (pos >= max_len - 128) break;
    }
    
    pos += snprintf(buf + pos, max_len - pos, "]}");
    return pos;
}
