#include "flash_partition.h"
#include "w25q16.h"
#include "rtc_stm32.h"
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
static uint32_t s_log_entry_count = 0;
static uint32_t s_log_total_bytes = 0;

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

    // 3. Scan Log Partition across all sectors to count entries and find write address
    s_log_entry_count = 0;
    s_log_total_bytes = 0;
    log_write_addr = PARTITION_LOG_ADDR;
    
    for (uint32_t sec = 0; sec < 252; sec++) {
        uint32_t sec_addr = PARTITION_LOG_ADDR + sec * 4096;
        uint32_t marker = 0xFFFFFFFF;
        W25Q_Read(sec_addr, (uint8_t *)&marker, 4);
        if (marker == 0xFFFFFFFF) {
            if (sec == 0) {
                log_write_addr = PARTITION_LOG_ADDR;
            }
            break;
        }
        
        uint32_t off = 0;
        while (off + sizeof(PartitionLogHeader_t) <= 4096) {
            PartitionLogHeader_t hdr;
            W25Q_Read(sec_addr + off, (uint8_t *)&hdr, sizeof(PartitionLogHeader_t));
            if (hdr.timestamp == 0xFFFFFFFF || hdr.msg_len == 0 || hdr.msg_len > 60) {
                break;
            }
            s_log_entry_count++;
            s_log_total_bytes += sizeof(PartitionLogHeader_t) + hdr.msg_len;
            off += sizeof(PartitionLogHeader_t) + hdr.msg_len;
        }
        
        if (off + sizeof(PartitionLogHeader_t) + 1 <= 4096) {
            log_write_addr = sec_addr + off;
        } else {
            uint32_t next = (sec + 1) % 252;
            log_write_addr = PARTITION_LOG_ADDR + next * 4096;
        }
    }
    
    log_read_addr = PARTITION_LOG_ADDR;
    g_partition_ready = 1;
    printf("[Partition] Log: %lu entries\r\n", (unsigned long)s_log_entry_count);
}

/* ======================================================================
 *  Gateway Configuration Storage (Dual-Sector Redundancy)
 * ====================================================================== */
static uint8_t Sanitize_Config(Gateway_Config_t *cfg) {
    uint8_t changed = 0;

    if (cfg->magic != CONFIG_MAGIC_CURRENT) {
        cfg->magic = CONFIG_MAGIC_CURRENT;
        changed = 1;
    }
    if (cfg->actuator_count > MAX_RELAYS) {
        cfg->actuator_count = 0;
        changed = 1;
    }
    if (cfg->sensors.count > MAX_SENSORS) {
        cfg->sensors.count = 0;
        changed = 1;
    }
    if (cfg->mqtt_mapping_count > MAX_MQTT_MAPPINGS) {
        cfg->mqtt_mapping_count = 0;
        changed = 1;
    }
    if (cfg->mqtt_port == 0) {
        cfg->mqtt_port = 1883;
        changed = 1;
    }
    if (cfg->mqtt_interval == 0 || cfg->mqtt_interval > 86400) {
        cfg->mqtt_interval = 2;
        changed = 1;
    }
    if (cfg->mqtt_send_mode > 1) {
        cfg->mqtt_send_mode = 0;
        changed = 1;
    }
    if (cfg->mqtt_payload_shape > 2) {
        cfg->mqtt_payload_shape = 0;
        changed = 1;
    }
    if (cfg->test_mode > 1) {
        cfg->test_mode = 0;
        changed = 1;
    }
    if (cfg->mqtt_tx_enabled > 1) {
        cfg->mqtt_tx_enabled = 1;
        changed = 1;
    }
    if (cfg->mqtt_skip_offline > 1) {
        cfg->mqtt_skip_offline = 0;
        changed = 1;
    }

    #define SANITIZE_FIELD(field) do { \
        field[sizeof(field) - 1] = '\0'; \
        for (size_t _idx = 0; _idx < sizeof(field) && field[_idx] != '\0'; _idx++) { \
            uint8_t _ch = (uint8_t)field[_idx]; \
            if (_ch < 0x20 || _ch > 0x7E || _ch == '"' || _ch == '\\') { \
                field[_idx] = '\0'; \
                changed = 1; \
                break; \
            } \
        } \
    } while (0)

    SANITIZE_FIELD(cfg->mqtt_broker);
    SANITIZE_FIELD(cfg->mqtt_client_id);
    SANITIZE_FIELD(cfg->mqtt_username);
    SANITIZE_FIELD(cfg->mqtt_password);
    SANITIZE_FIELD(cfg->device_id);
    SANITIZE_FIELD(cfg->sparkplug_topic);
    SANITIZE_FIELD(cfg->pending_sparkplug_topic);
    SANITIZE_FIELD(cfg->provision_status);
    SANITIZE_FIELD(cfg->provision_message);
    SANITIZE_FIELD(cfg->admin_username);
    SANITIZE_FIELD(cfg->admin_password);

    for (int i = 0; i < MAX_RELAYS; i++) {
        SANITIZE_FIELD(cfg->actuators[i].name);
        SANITIZE_FIELD(cfg->actuators[i].port_or_ip);
        SANITIZE_FIELD(cfg->actuators[i].opc_node_id);
        if (cfg->actuators[i].type > 7) {
            cfg->actuators[i].type = 0;
            changed = 1;
        }
    }

    for (int i = 0; i < MAX_MQTT_MAPPINGS; i++) {
        SANITIZE_FIELD(cfg->mqtt_mappings[i].json_key);
    }
    #undef SANITIZE_FIELD

    /* Validate sparkplug_topic looks like a real topic */
    if (cfg->sparkplug_topic[0] != '\0') {
        int valid_topic = 1;
        for (int i = 0; cfg->sparkplug_topic[i] != '\0'; i++) {
            char c = cfg->sparkplug_topic[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '/' || c == '_' || c == '-' || c == '+' || c == '#' || c == '.' || c == ':')) {
                valid_topic = 0;
                break;
            }
        }
        if (!valid_topic) {
            strncpy(cfg->sparkplug_topic, "spBv1.0/farm/kontrx-0000001/KontrxDevice-01/DDATA", sizeof(cfg->sparkplug_topic) - 1);
            cfg->sparkplug_topic[sizeof(cfg->sparkplug_topic) - 1] = '\0';
            changed = 1;
        }
    }

    if (cfg->admin_username[0] == '\0') {
        strncpy(cfg->admin_username, "admin", sizeof(cfg->admin_username) - 1);
        changed = 1;
    }
    if (cfg->admin_password[0] == '\0') {
        strncpy(cfg->admin_password, "adminkontrx", sizeof(cfg->admin_password) - 1);
        changed = 1;
    }
    if (cfg->provision_status[0] == '\0') {
        strncpy(cfg->provision_status, "Active", sizeof(cfg->provision_status) - 1);
        changed = 1;
    }

    return changed;
}

uint8_t Partition_LoadConfig(Gateway_Config_t *cfg) {
    static Gateway_Config_t temp;
    uint8_t loaded = 0;

    // Try Primary Config (Sector 128)
    W25Q_Read(CONFIG_PRIMARY_ADDR, (uint8_t *)&temp, sizeof(Gateway_Config_t));
    uint32_t cal_crc = Compute_CRC32((const uint8_t *)&temp, offsetof(Gateway_Config_t, checksum));
    
    if (temp.magic == CONFIG_MAGIC_CURRENT && temp.checksum == cal_crc) {
        memcpy(cfg, &temp, sizeof(Gateway_Config_t));
        loaded = 1;
    } else {
        printf("[Partition] Primary config CRC/Magic mismatch! Trying backup...\r\n");
        // Try Backup Config (Sector 129)
        W25Q_Read(CONFIG_BACKUP_ADDR, (uint8_t *)&temp, sizeof(Gateway_Config_t));
        cal_crc = Compute_CRC32((const uint8_t *)&temp, offsetof(Gateway_Config_t, checksum));
        
        if (temp.magic == CONFIG_MAGIC_CURRENT && temp.checksum == cal_crc) {
            memcpy(cfg, &temp, sizeof(Gateway_Config_t));
            printf("[Partition] Restored primary config from backup sector.\r\n");
            loaded = 1;
        } else {
            printf("[Partition] Backup also failed!\r\n");
            // Check magic-only migration from primary
            W25Q_Read(CONFIG_PRIMARY_ADDR, (uint8_t *)&temp, sizeof(Gateway_Config_t));
            if (temp.magic == CONFIG_MAGIC_CURRENT) {
                printf("[Partition] CRC-skip migration from primary...\r\n");
                memcpy(cfg, &temp, sizeof(Gateway_Config_t));
                loaded = 1;
            } else {
                // Check magic-only migration from backup
                W25Q_Read(CONFIG_BACKUP_ADDR, (uint8_t *)&temp, sizeof(Gateway_Config_t));
                if (temp.magic == CONFIG_MAGIC_CURRENT) {
                    printf("[Partition] CRC-skip migration from backup...\r\n");
                    memcpy(cfg, &temp, sizeof(Gateway_Config_t));
                    loaded = 1;
                }
            }
        }
    }

    if (loaded) {
        uint8_t dirty = Sanitize_Config(cfg);
        uint32_t cur_crc = Compute_CRC32((const uint8_t *)cfg, offsetof(Gateway_Config_t, checksum));
        if (dirty || cfg->checksum != cur_crc) {
            cfg->magic = CONFIG_MAGIC_CURRENT;
            cfg->checksum = cur_crc;
            Partition_SaveConfig(cfg);
            printf("[Partition] Config sanitized and re-saved to flash partitions.\r\n");
        }
        return 1;
    }

    return 0; // Config failed to load (blank or invalid magic)
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
        snprintf(fallback_msg, sizeof(fallback_msg), "Recovered rules: %s", temp.version_id);
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

void Partition_Queue_Reset(void) {
    q_read_ptr = q_write_ptr;
    printf("[Partition] Queue reset: read_ptr=%lu, write_ptr=%lu\r\n",
           (unsigned long)q_read_ptr, (unsigned long)q_write_ptr);
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
    s_log_entry_count++;
    s_log_total_bytes += req_len;
    
    // Wrap around whole partition if needed
    if (log_write_addr >= PARTITION_LOG_ADDR + PARTITION_LOG_SIZE) {
        log_write_addr = PARTITION_LOG_ADDR;
        W25Q_EraseSector(log_write_addr);
    }
}

uint32_t Partition_Log_Count(uint32_t *out_bytes) {
    if (out_bytes) *out_bytes = s_log_total_bytes;
    return s_log_entry_count;
}

int Partition_Log_FormatJSON_Paged(char *buf, int max_len, uint32_t offset, uint32_t limit) {
    if (!buf || max_len <= 0) return 0;
    if (limit == 0) limit = 40;
    if (limit > 50) limit = 50;

    int pos = 0;
    pos += snprintf(buf + pos, max_len - pos, "{\"logs\":[");

    /* Pass 1: Count total valid log entries across active sectors */
    uint32_t total_found = 0;
    for (uint32_t sec = 0; sec < 252; sec++) {
        uint32_t sec_addr = PARTITION_LOG_ADDR + sec * 4096;
        uint32_t marker = 0xFFFFFFFF;
        W25Q_Read(sec_addr, (uint8_t *)&marker, 4);
        if (marker == 0xFFFFFFFF) break;

        uint32_t off = 0;
        while (off + sizeof(PartitionLogHeader_t) <= 4096) {
            PartitionLogHeader_t hdr;
            uint32_t entry_addr = sec_addr + off;
            W25Q_Read(entry_addr, (uint8_t *)&hdr, sizeof(PartitionLogHeader_t));
            if (hdr.timestamp == 0xFFFFFFFF || hdr.msg_len == 0 || hdr.msg_len > 60) break;
            total_found++;
            off += sizeof(PartitionLogHeader_t) + hdr.msg_len;
        }
    }

    /* Target window: entries from newest downwards.
     * Newest is index (total_found - 1).
     * With offset, requested newest index is (total_found - 1 - offset).
     * Oldest requested index is (total_found - offset - limit) or 0.
     */
    uint32_t target_addrs[50];
    uint32_t target_count = 0;

    if (total_found > offset) {
        uint32_t win_end = total_found - 1 - offset;
        uint32_t win_start = (total_found >= offset + limit) ? (total_found - offset - limit) : 0;

        /* Pass 2: Collect entry addresses that fall into [win_start, win_end] */
        uint32_t curr_idx = 0;
        for (uint32_t sec = 0; sec < 252; sec++) {
            uint32_t sec_addr = PARTITION_LOG_ADDR + sec * 4096;
            uint32_t marker = 0xFFFFFFFF;
            W25Q_Read(sec_addr, (uint8_t *)&marker, 4);
            if (marker == 0xFFFFFFFF) break;

            uint32_t off = 0;
            while (off + sizeof(PartitionLogHeader_t) <= 4096) {
                PartitionLogHeader_t hdr;
                uint32_t entry_addr = sec_addr + off;
                W25Q_Read(entry_addr, (uint8_t *)&hdr, sizeof(PartitionLogHeader_t));
                if (hdr.timestamp == 0xFFFFFFFF || hdr.msg_len == 0 || hdr.msg_len > 60) break;

                if (curr_idx >= win_start && curr_idx <= win_end) {
                    uint32_t slot = curr_idx - win_start;
                    if (slot < 50) {
                        target_addrs[slot] = entry_addr;
                        if (slot + 1 > target_count) target_count = slot + 1;
                    }
                }
                curr_idx++;
                off += sizeof(PartitionLogHeader_t) + hdr.msg_len;
                if (curr_idx > win_end) break;
            }
            if (curr_idx > win_end) break;
        }
    }

    /* Print target_addrs from newest (target_count - 1) down to 0 */
    uint16_t yr = 2026;
    uint8_t mo = 9, dy = 14;
    RTC_GetDateTime(&yr, &mo, &dy, NULL, NULL, NULL);

    static const char * const cats[] = {"SYS", "MQTT", "MODBUS", "RELAY", "OTA", "SD", "AUTH"};
    uint32_t printed = 0;

    for (int idx = (int)target_count - 1; idx >= 0; idx--) {
        uint32_t addr = target_addrs[idx];
        PartitionLogHeader_t hdr;
        W25Q_Read(addr, (uint8_t *)&hdr, sizeof(PartitionLogHeader_t));

        char msg_temp[64];
        uint32_t len = hdr.msg_len;
        if (len >= sizeof(msg_temp)) len = sizeof(msg_temp) - 1;
        W25Q_Read(addr + sizeof(PartitionLogHeader_t), (uint8_t *)msg_temp, len);
        msg_temp[len] = '\0';

        for (uint32_t k = 0; k < len; k++) {
            if ((unsigned char)msg_temp[k] < 32 || (unsigned char)msg_temp[k] > 126) {
                msg_temp[k] = ' ';
            } else if (msg_temp[k] == '"' || msg_temp[k] == '\\') {
                msg_temp[k] = '\'';
            }
        }

        uint32_t s = hdr.timestamp;
        uint32_t hrs = (s / 3600) % 24;
        uint32_t mins = (s % 3600) / 60;
        uint32_t secs = s % 60;
        const char *cat_str = (hdr.category_id <= 6) ? cats[hdr.category_id] : "SYS";

        if (printed > 0) {
            pos += snprintf(buf + pos, max_len - pos, ",");
        }

        pos += snprintf(buf + pos, max_len - pos,
                        "{\"time\":\"%04u-%02u-%02u %02lu:%02lu:%02lu\",\"uptime\":\"%02lu:%02lu:%02lu\",\"up_s\":%lu,\"cat\":\"%s\",\"msg\":\"%s\"}",
                        (unsigned int)yr, (unsigned int)mo, (unsigned int)dy,
                        (unsigned long)hrs, (unsigned long)mins, (unsigned long)secs,
                        (unsigned long)hrs, (unsigned long)mins, (unsigned long)secs,
                        (unsigned long)s,
                        cat_str, msg_temp);

        printed++;
        if (pos >= max_len - 128) break;
    }

    pos += snprintf(buf + pos, max_len - pos,
                    "],\"offset\":%lu,\"limit\":%lu,\"count\":%lu,\"total_count\":%lu}",
                    (unsigned long)offset, (unsigned long)limit,
                    (unsigned long)printed, (unsigned long)total_found);
    return pos;
}

int Partition_Log_FormatJSON(char *buf, int max_len, uint32_t max_items) {
    return Partition_Log_FormatJSON_Paged(buf, max_len, 0, (max_items > 0 && max_items <= 50) ? max_items : 40);
}

void Partition_Log_Clear(void) {
    for (uint32_t s = 0; s < 252; s++) {
        uint32_t s_addr = PARTITION_LOG_ADDR + s * 4096;
        uint32_t marker = 0xFFFFFFFF;
        W25Q_Read(s_addr, (uint8_t *)&marker, 4);
        if (marker != 0xFFFFFFFF) {
            W25Q_EraseSector(s_addr);
        } else {
            break;
        }
    }
    log_write_addr = PARTITION_LOG_ADDR;
    log_read_addr = PARTITION_LOG_ADDR;
    s_log_entry_count = 0;
    s_log_total_bytes = 0;
}

uint32_t Partition_Log_Stream(uint8_t sn, uint8_t (*send_fn)(uint8_t sn, const uint8_t *data, uint32_t total)) {
    if (!send_fn) return 0;

    static char buf[2048];
    int pos = 0;
    uint32_t total_sent = 0;

    uint16_t yr = 2026;
    uint8_t mo = 9, dy = 14, hr = 0, mn = 0, sc = 0;
    RTC_GetDateTime(&yr, &mo, &dy, &hr, &mn, &sc);

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "# ==============================================================================\r\n"
        "# Kontrx Universal Edge Gateway (Model: KX-F407)\r\n"
        "# System Event Audit Log Export\r\n"
        "# Date: %04u-%02u-%02u %02u:%02u:%02u\r\n"
        "# ==============================================================================\r\n",
        yr, mo, dy, hr, mn, sc);

    static const char * const cats[] = {"SYS", "MQTT", "MODBUS", "RELAY", "OTA", "SD", "AUTH"};

    for (uint32_t sec = 0; sec < 252; sec++) {
        uint32_t sec_addr = PARTITION_LOG_ADDR + sec * 4096;
        uint32_t marker = 0xFFFFFFFF;
        W25Q_Read(sec_addr, (uint8_t *)&marker, 4);
        if (marker == 0xFFFFFFFF) break;

        uint32_t off = 0;
        while (off + sizeof(PartitionLogHeader_t) <= 4096) {
            PartitionLogHeader_t hdr;
            uint32_t entry_addr = sec_addr + off;
            W25Q_Read(entry_addr, (uint8_t *)&hdr, sizeof(PartitionLogHeader_t));
            if (hdr.timestamp == 0xFFFFFFFF || hdr.msg_len == 0 || hdr.msg_len > 60) break;

            char msg_temp[64];
            uint32_t len = hdr.msg_len;
            if (len >= sizeof(msg_temp)) len = sizeof(msg_temp) - 1;
            W25Q_Read(entry_addr + sizeof(PartitionLogHeader_t), (uint8_t *)msg_temp, len);
            msg_temp[len] = '\0';

            for (uint32_t k = 0; k < len; k++) {
                if ((unsigned char)msg_temp[k] < 32 || (unsigned char)msg_temp[k] > 126) {
                    msg_temp[k] = ' ';
                }
            }

            uint32_t s = hdr.timestamp;
            uint32_t hrs = (s / 3600) % 24;
            uint32_t mins = (s % 3600) / 60;
            uint32_t secs = s % 60;
            const char *cat_str = (hdr.category_id <= 6) ? cats[hdr.category_id] : "SYS";

            char line_buf[128];
            int line_len = snprintf(line_buf, sizeof(line_buf),
                "%04u-%02u-%02u %02lu:%02lu:%02lu [%s] %s\r\n",
                (unsigned int)yr, (unsigned int)mo, (unsigned int)dy,
                (unsigned long)hrs, (unsigned long)mins, (unsigned long)secs,
                cat_str, msg_temp);

            if (line_len > 0) {
                if (pos + line_len >= (int)sizeof(buf)) {
                    if (!send_fn(sn, (const uint8_t *)buf, (uint32_t)pos)) {
                        return total_sent;
                    }
                    pos = 0;
                }
                memcpy(buf + pos, line_buf, line_len);
                pos += line_len;
                total_sent++;
            }

            off += sizeof(PartitionLogHeader_t) + hdr.msg_len;
        }
    }

    if (pos > 0) {
        send_fn(sn, (const uint8_t *)buf, (uint32_t)pos);
    }

    return total_sent;
}

uint32_t Partition_Log_StreamSize(void) {
    static const uint8_t cat_lens[] = {3, 4, 6, 5, 3, 2, 4};

    uint16_t yr = 2026;
    uint8_t mo = 9, dy = 14, hr = 0, mn = 0, sc = 0;
    RTC_GetDateTime(&yr, &mo, &dy, &hr, &mn, &sc);

    uint32_t total = 0;

    /* Header block size — same format as Partition_Log_Stream */
    total += (uint32_t)snprintf(NULL, 0,
        "# ==============================================================================\r\n"
        "# Kontrx Universal Edge Gateway (Model: KX-F407)\r\n"
        "# System Event Audit Log Export\r\n"
        "# Date: %04u-%02u-%02u %02u:%02u:%02u\r\n"
        "# ==============================================================================\r\n",
        yr, mo, dy, hr, mn, sc);

    /* Each log entry formatted line */
    for (uint32_t sec = 0; sec < 252; sec++) {
        uint32_t sec_addr = PARTITION_LOG_ADDR + sec * 4096;
        uint32_t marker = 0xFFFFFFFF;
        W25Q_Read(sec_addr, (uint8_t *)&marker, 4);
        if (marker == 0xFFFFFFFF) break;

        uint32_t off = 0;
        while (off + sizeof(PartitionLogHeader_t) <= 4096) {
            PartitionLogHeader_t hdr;
            W25Q_Read(sec_addr + off, (uint8_t *)&hdr, sizeof(PartitionLogHeader_t));
            if (hdr.timestamp == 0xFFFFFFFF || hdr.msg_len == 0 || hdr.msg_len > 60) break;

            uint8_t clen = (hdr.category_id <= 6) ? cat_lens[hdr.category_id] : 3;
            /* "YYYY-MM-DD HH:MM:SS [CAT] msg\r\n" = 19 + 2 + clen + 2 + msg_len + 2 */
            total += 25 + clen + hdr.msg_len;

            off += sizeof(PartitionLogHeader_t) + hdr.msg_len;
        }
    }

    return total;
}

uint32_t Partition_Queue_StreamSize(void) {
    uint32_t count = Partition_Queue_Count();

    uint16_t yr = 2026;
    uint8_t mo = 9, dy = 14, hr = 0, mn = 0, sc = 0;
    RTC_GetDateTime(&yr, &mo, &dy, &hr, &mn, &sc);

    /* Header block */
    uint32_t total = (uint32_t)snprintf(NULL, 0,
        "# ==============================================================================\r\n"
        "# Kontrx Universal Edge Gateway (Model: KX-F407)\r\n"
        "# Offline Telemetry Queue Export\r\n"
        "# Status: %s | Total Buffered Records: %lu\r\n"
        "# ==============================================================================\r\n"
        "# Index\tTimestamp(Epoch)\tType\tID\tStatus\tValue\tTemp\r\n",
        (count > 0) ? "BUFFERING" : "SYNCHRONIZED (Empty)",
        (unsigned long)count);

    if (count == 0) {
        total += (uint32_t)snprintf(NULL, 0,
            "# Offline telemetry queue is currently empty. All data published to broker.\r\n");
        return total;
    }

    /* Each record line — iterate to compute exact formatted lengths */
    uint32_t cur_ptr = q_read_ptr;
    uint32_t idx = 0;
    while (cur_ptr != q_write_ptr) {
        uint32_t addr = PARTITION_QUEUE_ADDR + cur_ptr * 16;
        OfflineRecord_t rec;
        W25Q_Read(addr, (uint8_t *)&rec, sizeof(OfflineRecord_t));

        total += (uint32_t)snprintf(NULL, 0,
            "%lu\t%lu\t%s\t%u\t0x%02X\t%.2f\t%.2f\r\n",
            (unsigned long)idx,
            (unsigned long)rec.timestamp,
            (rec.source_type == 1) ? "SENSOR" : "ACTUATOR",
            rec.source_id,
            rec.valid,
            rec.value,
            rec.temp);

        idx++;
        cur_ptr = (cur_ptr + 1) % 32768;
    }

    return total;
}

uint32_t Partition_Queue_Stream(uint8_t sn, uint8_t (*send_fn)(uint8_t sn, const uint8_t *data, uint32_t total)) {
    if (!send_fn) return 0;

    static char buf[2048];
    int pos = 0;
    uint32_t count = Partition_Queue_Count();

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "# ==============================================================================\r\n"
        "# Kontrx Universal Edge Gateway (Model: KX-F407)\r\n"
        "# Offline Telemetry Queue Export\r\n"
        "# Status: %s | Total Buffered Records: %lu\r\n"
        "# ==============================================================================\r\n"
        "# Index\tTimestamp(Epoch)\tType\tID\tStatus\tValue\tTemp\r\n",
        (count > 0) ? "BUFFERING" : "SYNCHRONIZED (Empty)",
        (unsigned long)count);

    if (count == 0) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "# Offline telemetry queue is currently empty. All data published to broker.\r\n");
        send_fn(sn, (const uint8_t *)buf, (uint32_t)pos);
        return 0;
    }

    uint32_t cur_ptr = q_read_ptr;
    uint32_t idx = 0;
    while (cur_ptr != q_write_ptr) {
        uint32_t addr = PARTITION_QUEUE_ADDR + cur_ptr * 16;
        OfflineRecord_t rec;
        W25Q_Read(addr, (uint8_t *)&rec, sizeof(OfflineRecord_t));

        char line_buf[128];
        int line_len = snprintf(line_buf, sizeof(line_buf),
            "%lu\t%lu\t%s\t%u\t0x%02X\t%.2f\t%.2f\r\n",
            (unsigned long)idx,
            (unsigned long)rec.timestamp,
            (rec.source_type == 1) ? "SENSOR" : "ACTUATOR",
            rec.source_id,
            rec.valid,
            rec.value,
            rec.temp);

        if (line_len > 0) {
            if (pos + line_len >= (int)sizeof(buf)) {
                if (!send_fn(sn, (const uint8_t *)buf, (uint32_t)pos)) {
                    return idx;
                }
                pos = 0;
            }
            memcpy(buf + pos, line_buf, line_len);
            pos += line_len;
            idx++;
        }

        cur_ptr = (cur_ptr + 1) % 32768;
    }

    if (pos > 0) {
        send_fn(sn, (const uint8_t *)buf, (uint32_t)pos);
    }
    return idx;
}

