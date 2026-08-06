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

/* Relay State Configuration */
typedef struct {
    uint8_t port_id;  // 0=A, 1=B, 2=C, 3=D, 4=E
    uint8_t pin_num;  // 0..15
    uint8_t is_nc;    // 0 = NO, 1 = NC
    uint8_t state;    // 0 = Off, 1 = On
    char    name[20]; // editable label
} Relay_Config_t;

/* Entire System State Config */
typedef struct {
    uint32_t magic;
    SensorList_t sensors;
    Relay_Config_t relays[MAX_RELAYS];
    uint8_t relay_count;
    uint32_t serial;              /* Sequential device serial, e.g. 1 => "KX-0000001".
                                   * Stored in flash, editable from the web UI. */
    char mqtt_broker[64];
    uint16_t mqtt_port;
    char mqtt_client_id[32];
    char mqtt_username[32];
    char mqtt_password[32];
    char device_id[40];
    char provision_status[24];
    char provision_message[128];
    char sparkplug_topic[128];
    char pending_sparkplug_topic[128]; /* New topic received via /api/provision awaiting confirmation */
    uint32_t mqtt_interval;       /* MQTT publish interval in seconds */
    uint8_t mqtt_send_mode;       /* 0 = On Interval (Periodic), 1 = On Change (CoV) */
    uint32_t checksum;
} Gateway_Config_t;

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
