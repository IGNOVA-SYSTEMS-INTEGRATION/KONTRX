#ifndef MODBUS_DMA_H
#define MODBUS_DMA_H

#include <stdint.h>
#include <string.h>
#include "cmsis_os2.h"

#define MAX_SENSORS    16
#define MAX_RELAYS     16
#define MAX_MULTI_US   4

#define SENSOR_TYPE_MULTI_US 7
/* Bumped 0xC01D0006 -> 0xC01D0007: Added admin auth & actuator mask to Gateway_Config_t. */
#define CONFIG_MAGIC_CURRENT 0xC01D0007U

/* Firmware version — single source of truth for /api/status "fw" and the QR.
 * Keep this in sync with the actual release. */
#ifdef KONTRX_FAULT_TEST
#define FW_VERSION "2.2.3-faulttest"   /* test image: fault-injection endpoint built in */
#else
#define FW_VERSION "2.2.3"
#endif

/* Sensor types: 1=pH, 2=ORP, 3=EC, 4=DO, 5=Ammonia, 6=Ultrasonic, 7=Multi-US */
typedef struct {
    uint8_t type;
    uint8_t id;
} SensorEntry_t;

typedef struct {
    uint8_t count;
    SensorEntry_t entries[MAX_SENSORS];
} SensorList_t;

/* Dynamic per-sensor reading */
typedef struct {
    uint8_t type;
    uint8_t id;
    float   value;
    float   temp;
    uint8_t valid;        /* 1 = last read was good */
    uint8_t stale;        /* 1 = no successful read yet in this session */
    uint32_t last_ok_ms;  /* osKernelGetTickCount() of last good read */

    /* Accumulators for sample-count averaging */
    float    sum_value;
    float    sum_temp;
    uint32_t sample_count;
} SensorReading_t;

/* Water Flow Sensor Channel (5 registers: Rate 0.1 L/min, Total High/Low Word, Freq Hz, Status) */
typedef struct {
    float    rate;       /* Flow Rate in L/min (Reg[0] / 10.0f) */
    uint32_t total_l;    /* Total Volume in Liters ((Reg[1] << 16) | Reg[2]) */
    float    freq_hz;    /* Frequency in Hz (Reg[3]) */
    uint8_t  status;     /* Status: 1=Normal Flow, 0=Stopped (Reg[4]) */
} WaterFlow_Channel_t;

/* Multi-Sensor Board (3 Ultrasonic, 3 Water Flow, 2 DS18B20 Temp, Mode Switch & FC04 Summary) */
typedef struct {
    uint8_t             id;
    float               dist[3];        /* 3 Ultrasonic distances (mm) - Offsets 0x00, 0x10, 0x20 */
    WaterFlow_Channel_t flow[3];        /* 3 Water flow sensors - Offsets 0x30, 0x40, 0x50 */
    float               temp[2];        /* 2 DS18B20 temp sensors (°C) - Offsets 0x60, 0x70 */
    uint16_t            summary[10];    /* FC04 Input Registers summary (Start 0, Qty 10) */
    uint8_t             summary_ok;     /* 1 once summary[] holds a real FC04 reply */
    uint8_t             mode;           /* 0=Normal Mode, 1=Simulation Mode (FC06 Reg 165) */
    uint8_t             valid;

    /* Legacy / Compatibility fields */
    float               avg;            /* Average distance (FC04 reg 0, else local mean of valid US) */
    float               comp;           /* Temperature-compensated distance (FC04 reg 2) */
    float               legacy_temp;    /* Primary temp */
} MultiSensorBoard_t;

typedef MultiSensorBoard_t MultiUS_t;

/* Shared Sensor Data Structure */
typedef struct {
    SensorReading_t readings[MAX_SENSORS];
    uint8_t  readings_count;
    MultiUS_t multi_us[MAX_MULTI_US];
    uint8_t  multi_us_count;
    uint32_t last_update_time;
} Modbus_SensorData_t;

/* Batch Record for MQTT transmission */
typedef struct {
    uint8_t  type;
    uint8_t  id;
    float    avg_value;
    float    avg_temp;
    uint32_t samples_collected;
    uint8_t  valid;
} BatchRecord_t;

/* Multi-Sensor Board per-channel values for MQTT transmission.
 * ch[] order: US1..3 distance (mm), Flow1..3 rate (L/min), Temp1..2 (degC). */
#define MULTI_CH_US       0
#define MULTI_CH_FLOW     3
#define MULTI_CH_TEMP     6
#define MULTI_CH_COUNT    8
#define MULTI_CH_INVALID  (-1000.0f)   /* channel has no valid reading */

/* Offline queue (OfflineRecord_t.source_type): QUEUE_SOURCE_MULTI_CH + p holds
 * channel pair p of a board, ch[2p] in value and ch[2p + 1] in temp. */
#define QUEUE_SOURCE_MULTI_CH   0x10U
#define QUEUE_MULTI_CH_RECORDS  (MULTI_CH_COUNT / 2)

typedef struct {
    uint8_t id;
    uint8_t valid;                 /* board reading valid (same as its BatchRecord_t) */
    float   ch[MULTI_CH_COUNT];
} MultiChRecord_t;

/* Mean of the valid channels in [first, first + n); MULTI_CH_INVALID if none. */
static inline float MultiCh_Avg(const MultiChRecord_t *m, uint8_t first, uint8_t n) {
    float sum = 0.0f;
    uint8_t cnt = 0;
    for (uint8_t i = first; i < first + n && i < MULTI_CH_COUNT; i++) {
        if (m->ch[i] > MULTI_CH_INVALID + 1.0f) { sum += m->ch[i]; cnt++; }
    }
    return cnt ? (sum / cnt) : MULTI_CH_INVALID;
}

typedef struct {
    BatchRecord_t records[MAX_SENSORS];
    uint8_t       count;
    MultiChRecord_t multi[MAX_MULTI_US];
    uint8_t         multi_count;
} TelemetryBatch_t;

/* Returns the batch's channel record for multi-sensor board `id`, or NULL. */
static inline const MultiChRecord_t *MultiCh_Find(const TelemetryBatch_t *b, uint8_t id) {
    for (uint8_t i = 0; i < b->multi_count && i < MAX_MULTI_US; i++) {
        if (b->multi[i].id == id) return &b->multi[i];
    }
    return NULL;
}

/* Actuator Protocol & State Configuration */
typedef enum {
    ACTUATOR_TYPE_LOCAL_GPIO = 0,   // Local onboard Relays (PA0, PE2, etc.)
    ACTUATOR_TYPE_MODBUS_TCP = 1,   // Modbus TCP remote PLC Port
    ACTUATOR_TYPE_OPC_UA_CLIENT = 2, // Direct OPC UA Client Write Node
    ACTUATOR_TYPE_PWM = 3,          // PWM Output Channel
    ACTUATOR_TYPE_PTO = 4,          // PTO Motion Axis Channel
    ACTUATOR_TYPE_ANALOG_MA = 5,    // 4-20mA Current Output Channel
    ACTUATOR_TYPE_ANALOG_V = 6,     // 0-10V Voltage Output Channel
    ACTUATOR_TYPE_DIGITAL_OUT = 7   // General Digital Output (D/O)
} ActuatorType_t;

typedef struct {
    uint8_t  id;                    // Actuator ID (0 to 9)
    char     name[16];              // e.g. "PLC_Valve_1", "LocalRelay1"
    uint8_t  type;                  // ActuatorType_t
    uint8_t  is_active_low;         // 1 = Active Low (NC), 0 = Active High (NO)
    uint8_t  state;                 // 0 = Off, 1 = On
    
    /* Connection details (flat for binary serialization safety) */
    char     port_or_ip[16];        // GPIO: "PA", "PC", "PE" / PLC: IP (e.g. "192.168.1.50")
    uint16_t port;                  // PLC/OPC UA Port (e.g. 502 or 4840)
    uint8_t  pin_or_slave;          // GPIO: Pin index (0-15) / PLC: Slave ID
    uint16_t reg_addr;              // PLC register/coil address
    char     opc_node_id[32];       // OPC UA Node Identifier string
} Actuator_Config_t;

/* Dynamic MQTT Payload Mapping */
typedef enum {
    MAP_SOURCE_SENSOR = 0,
    MAP_SOURCE_ACTUATOR
} MapSourceType_t;

typedef struct {
    uint8_t  source_type;        // MapSourceType_t
    uint8_t  source_id;          // ID of target sensor/actuator
    char     json_key[24];       // Target JSON key in the published MQTT message
    uint8_t  enabled;            // 1 = Active, 0 = Disabled
} Mqtt_Field_Mapping_t;

#define MAX_MQTT_MAPPINGS 16

/* Entire System State Config */
typedef struct {
    uint32_t magic;
    SensorList_t sensors;
    Actuator_Config_t actuators[MAX_RELAYS];
    uint8_t actuator_count;
    uint32_t serial;              /* Device serial number */
    char mqtt_broker[64];
    uint16_t mqtt_port;
    char mqtt_client_id[32];
    char mqtt_username[32];
    char mqtt_password[32];
    char device_id[40];
    char provision_status[24];
    char provision_message[128];
    char sparkplug_topic[128];
    char pending_sparkplug_topic[128];
    char sparkplug_edge_node_id[48];
    char sparkplug_device_id[48];
    uint32_t mqtt_interval;       /* MQTT publish interval in seconds */
    uint8_t mqtt_send_mode;       /* 0 = On Interval (Periodic), 1 = On Change (CoV) */
    uint8_t mqtt_payload_shape;   /* 0 = Standard Nested JSON, 1 = Flat Key-Value JSON, 2 = Sparkplug B */
    
    /* Dynamic Schema Mapping */
    Mqtt_Field_Mapping_t mqtt_mappings[MAX_MQTT_MAPPINGS];
    uint8_t              mqtt_mapping_count;

    /* Web Authentication & Dynamic Actuators */
    char                 admin_username[32];
    char                 admin_password[32];
    char                 master_password[32];
    uint8_t              actuator_mask; /* bit0=PTO, bit1=0-10V, bit2=4-20mA, bit3=PWM */
    uint8_t              test_mode;     /* 0 = Normal/Rules Automation, 1 = Manual Test Mode */
    uint8_t              mqtt_tx_enabled; /* 0 = Publishing Paused, 1 = Publishing Active */
    uint8_t              mqtt_skip_offline; /* 1 = Skip offline sensors in MQTT messages */

    uint32_t checksum;
} Gateway_Config_t;

#define CONFIG_FLASH_SECTOR     7
#define CONFIG_FLASH_ADDR       0x0807C000U

#define SCAN_MAX_DEVICES 32
typedef struct {
    uint8_t id;
    uint8_t type; // 1=pH, 2=ORP, 3=EC, 4=DO, 5=Ammonia, 6=Ultrasonic, 7=Multi-US, 255=Unknown
} ScannedDevice_t;

typedef struct {
    volatile uint8_t is_scanning;
    volatile uint8_t progress;
    volatile uint8_t count;
    ScannedDevice_t devices[SCAN_MAX_DEVICES];
} ModbusScanStatus_t;

/* Global RTOS variables for safe state sharing */
extern osMutexId_t sensorMutex;
extern osMutexId_t configMutex;
extern Modbus_SensorData_t sharedSensorData;
extern Gateway_Config_t sharedConfig;
extern uint8_t relayStates[MAX_RELAYS];
extern volatile ModbusScanStatus_t g_scan_status;
extern volatile uint8_t g_config_changed;
extern volatile uint32_t g_config_version;

/* Modbus DMA Driver Initialization & Execution */
void Modbus_DMA_Init(void);
void Modbus_DMA_PollSensors(void);
void Modbus_DMA_PerformScan(void);
void Modbus_DMA_ConsumeBatch(TelemetryBatch_t *dest);

/* Helper Primitives to safely read/write states */
void Get_Shared_Sensor_Data(Modbus_SensorData_t *dest);
void Update_Shared_Sensor_Data(const Modbus_SensorData_t *src);
void Get_Shared_Config(Gateway_Config_t *dest);
void Update_Shared_Config(const Gateway_Config_t *src);

uint8_t Modbus_SetMultiSensorMode(uint8_t slave_id, uint8_t mode);

#endif /* MODBUS_DMA_H */
