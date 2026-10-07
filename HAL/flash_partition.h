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
#define MAX_COND_NODES           16
#define MAX_SEQUENCES            4
#define MAX_SEQ_STEPS            6
#define RULES_MAGIC_CURRENT      0xC01D7792U // Bump: Condition tree & sequences added

typedef enum {
    COND_TYPE_COMPARE = 0,
    COND_TYPE_HYSTERESIS,
    COND_TYPE_RANGE,
    COND_TYPE_TIMER_ON,
    COND_TYPE_PULSE_TIMER,
    COND_TYPE_SR_LATCH,
    COND_TYPE_AND,
    COND_TYPE_OR,
    COND_TYPE_NOT
} RuleConditionType_t;

typedef enum {
    COND_NODE_COMPARE = 0,
    COND_NODE_HYSTERESIS,
    COND_NODE_RANGE,
    COND_NODE_TIMER_ON,
    COND_NODE_PULSE_TIMER,
    COND_NODE_SR_LATCH,
    COND_NODE_AND,
    COND_NODE_OR,
    COND_NODE_NOT,
    COND_NODE_SENSOR_STATE
} CondNodeType_t;

typedef struct {
    uint8_t  type;             // CondNodeType_t
    uint8_t  op;               // 0:none, 1:'>', 2:'<', 3:'==', 4:'!=', 5:'>=', 6:'<='
    uint8_t  compare_mode;     // 0: constant, 1: sensor-to-sensor
    uint8_t  range_mode;       // 0: inside, 1: outside
    uint8_t  sr_priority;      // 0: reset dominant, 1: set dominant
    char     input_id[16];     // Primary sensor ID (numeric or name)
    char     input_b_id[16];   // Secondary sensor ID
    float    threshold;        // Constant threshold / Range Min / Hyst High
    float    threshold_b;      // Range Max / Hyst Low
    uint32_t delay_ms;         // TON delay or TP pulse duration in ms
    int8_t   children[4];      // Indices in cond_nodes[] (-1 for none)
    uint8_t  child_count;      // Number of child nodes (0..4)
    uint8_t  padding[3];
} CondNode_t;

typedef enum {
    SEQ_MODE_ONCE = 0,
    SEQ_MODE_LOOP = 1,
    SEQ_MODE_COUNT = 2
} SeqMode_t;

typedef enum {
    SEQ_ON_FALSE_ABORT_SAFE = 0,
    SEQ_ON_FALSE_FINISH_CYCLE = 1,
    SEQ_ON_FALSE_HOLD = 2
} SeqOnFalse_t;

typedef struct {
    char     action[12];       // "ON", "OFF", "SET_DUTY", etc.
    float    value;            // duty %, mA, V, steps
    uint32_t hold_ms;          // hold duration in ms
} SeqStep_t;

typedef struct {
    uint8_t   enabled;
    uint8_t   mode;            // SeqMode_t
    uint8_t   on_false;        // SeqOnFalse_t
    uint8_t   repeat_count;    // For SEQ_MODE_COUNT
    uint8_t   step_count;      // 1..MAX_SEQ_STEPS
    uint8_t   padding[3];
    SeqStep_t steps[MAX_SEQ_STEPS];
} ActionSequence_t;

typedef struct {
    char     rule_id[24];
    char     input_id[16];
    char     operator[4];
    float    threshold;
    char     output_id[16];
    char     action[16];        // Widened to fit "SET_DUTY", "SET_MA", "SET_V", "MOVE"
    uint8_t  active;
    uint8_t  compare_mode;      // 0 = constant, 1 = input (sensor-to-sensor)
    char     input_b_id[16];

    /* Advanced condition extensions */
    uint8_t  condition_type;    // RuleConditionType_t
    float    high_threshold;    // Hysteresis: turn-ON threshold
    float    low_threshold;     // Hysteresis: turn-OFF threshold
    float    min_threshold;     // Range: lower bound
    float    max_threshold;     // Range: upper bound
    uint8_t  range_mode;        // 0 = inside, 1 = outside
    uint32_t delay_ms;          // TON delay / Pulse timer duration in ms

    /* Specialized actuator controls (PWM, PTO, 4-20mA, 0-10V) */
    uint8_t  actuator_type;     // 0=GPIO, 3=PWM, 4=PTO, 5=4-20mA, 6=0-10V, 7=DO
    float    action_value;      // PWM duty (0..100%), 4-20mA mA, 0-10V V
    uint32_t action_frequency;  // PWM / PTO frequency (Hz)
    int32_t  action_steps;      // PTO relative steps
    uint8_t  action_direction;  // PTO direction (0=CW, 1=CCW)

    /* Logic Gates & Latches */
    uint8_t  gate_type;         // Gate type (AND, OR, NOT)
    uint8_t  child_indices[4];  // Indices of child rules in rules[] array
    uint8_t  child_count;       // Number of valid child rules (0..4)
    uint8_t  sr_priority;       // 0 = reset dominant, 1 = set dominant
    uint8_t  auto_reverse;      // 1 = auto-reverse action when condition becomes false

    /* Hierarchical Tree & Sequence Extensions */
    int8_t   root_node;         // Index in cond_nodes[] (-1 if legacy)
    int8_t   sequence_idx;      // Index in sequences[] (-1 if single action)
    uint8_t  priority;          // Rule priority (1..16, higher wins)
    uint8_t  padding_ext;       // struct alignment
} Rule_t;

typedef struct {
    uint32_t         magic;
    char             version_id[36];
    char             timestamp[36];
    uint32_t         rule_count;
    Rule_t           rules[MAX_RULES];
    uint8_t          cond_node_count;
    CondNode_t       cond_nodes[MAX_COND_NODES];
    uint8_t          seq_count;
    ActionSequence_t sequences[MAX_SEQUENCES];
    uint8_t          rules_valid;       // 1 = Validated & Stable, 0 = Under test
    uint8_t          bypass_validation; // 1 = Bypass sensor online validation, 0 = Strict online checking
    uint8_t          padding[2];
    uint32_t         checksum;
} RuleConfig_t;

_Static_assert(sizeof(RuleConfig_t) <= 4096, "RuleConfig_t exceeds 4096-byte flash sector size!");

/* Canvas Layout Blob — opaque JSON stored by the desktop configurator.
   Allows round-trip: export → controller → re-import without losing
   connection topology that the flat Rule_t array cannot represent. */
#define LAYOUT_PARTITION_ADDR    0x00084000U
#define LAYOUT_PARTITION_SIZE    0x00004000U  // 16 KB (Sectors 132 to 135)
#define LAYOUT_MAGIC             0x4C41594FU  // "LAYO"
#define LAYOUT_MAX_DATA          (LAYOUT_PARTITION_SIZE - 12U) // 12 = magic + len + crc

#define PARTITION_QUEUE_ADDR     0x00088000U
#define PARTITION_QUEUE_SIZE     0x0007C000U  // 496 KB (Sectors 136 to 259)
#define QUEUE_RECORD_SIZE        16U
#define QUEUE_MAX_ENTRIES        (PARTITION_QUEUE_SIZE / QUEUE_RECORD_SIZE) // 31744

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

// Canvas Layout Blob (opaque JSON round-trip for desktop configurator)
uint8_t Partition_SaveLayout(const char *json, uint32_t len);
uint32_t Partition_LoadLayout(char *buf, uint32_t buf_size);

// Offline Telemetry Queue (FIFO Ring)
void Partition_Queue_Push(const OfflineRecord_t *rec);
uint8_t Partition_Queue_Pop(OfflineRecord_t *rec);
uint32_t Partition_Queue_Count(void);
uint8_t Partition_Queue_PeekAt(uint32_t index, OfflineRecord_t *rec);
void Partition_Queue_Discard(uint32_t n);
void Partition_Queue_Reset(void);

// Persistent System Audit Logs (Wrap-around Logger)
void Partition_Log_Append(uint32_t timestamp, uint8_t cat_id, const char *msg);
uint32_t Partition_Log_Count(uint32_t *out_bytes);
// Read logs into JSON buffer for API output (max_items limit)
int Partition_Log_FormatJSON(char *buf, int max_len, uint32_t max_items);
int Partition_Log_FormatJSON_Paged(char *buf, int max_len, uint32_t offset, uint32_t limit);
void Partition_Log_Clear(void);

// Streaming functions for full file export over socket
uint32_t Partition_Log_Stream(uint8_t sn, uint8_t (*send_fn)(uint8_t sn, const uint8_t *data, uint32_t total));
uint32_t Partition_Queue_Stream(uint8_t sn, uint8_t (*send_fn)(uint8_t sn, const uint8_t *data, uint32_t total));

// Rules History Archives (Stored in persistent flash partitions / SD Card)
#define PARTITION_RULES_HISTORY_ADDR  0x000EC000U // Sector 236 (96 KB = 24 sectors)
#define PARTITION_RULES_HISTORY_SIZE  0x00018000U // 24 Sectors (96 KB)
#define MAX_HISTORY_ARCHIVES          12U
#define ARCHIVE_SLOT_SIZE             0x00002000U // 8 KB per slot (4KB rules + 4KB layout)
#define ARCHIVE_SLOT_MAGIC            0x52554C41U // "RULA"

uint8_t  Partition_ArchiveRules(const RuleConfig_t *cfg, const char *layout_json, uint32_t layout_len);
uint8_t  Partition_RollbackRules(const char *target_version_id, RuleConfig_t *out_rules);
int      Partition_FormatRulesHistoryJSON(char *buf, int max_len, const char *active_version);
uint8_t  Partition_LoadArchivedRules(const char *version_id, RuleConfig_t *out_rules, char *out_layout, uint32_t layout_max);

#endif // FLASH_PARTITION_H
