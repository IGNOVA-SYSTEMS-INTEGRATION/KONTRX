#ifndef MODBUS_DMA_H
#define MODBUS_DMA_H

#include <stdint.h>
#include <string.h>
#include "cmsis_os2.h"

#define MAX_SENSORS    16
#define MAX_RELAYS     16
#define MAX_MULTI_US   4

#define SENSOR_TYPE_MULTI_US 7
/* Bumped 0xC01D0004 -> 0xC01D0005: Added mqtt_interval to Gateway_Config_t. */
#define CONFIG_MAGIC_CURRENT 0xC01D0005U

/* Firmware version — single source of truth for /api/status "fw" and the QR.
 * Keep this in sync with the actual release. (beta 1.0.1) */
#define FW_VERSION "1.0.1"

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

/* Multi-US board: 8 distances + avg/comp/temp */
typedef struct {
    float   dist[8];
    float   avg;
    float   comp;
    float   temp;
    uint8_t id;
} MultiUS_t;

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

typedef struct {
    BatchRecord_t records[MAX_SENSORS];
    uint8_t       count;
} TelemetryBatch_t;

/* Actuator Protocol & State Configuration */
typedef enum {
    ACTUATOR_TYPE_LOCAL_GPIO = 0,   // Local onboard Relays (PA0, PE2, etc.)
    ACTUATOR_TYPE_MODBUS_TCP,       // Modbus TCP remote PLC Port
    ACTUATOR_TYPE_OPC_UA_CLIENT     // Direct OPC UA Client Write Node
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
    uint32_t mqtt_interval;       /* MQTT publish interval in seconds */
    uint8_t mqtt_send_mode;       /* 0 = On Interval (Periodic), 1 = On Change (CoV) */
    
    /* Dynamic Schema Mapping */
    Mqtt_Field_Mapping_t mqtt_mappings[MAX_MQTT_MAPPINGS];
    uint8_t              mqtt_mapping_count;

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

#endif /* MODBUS_DMA_H */
