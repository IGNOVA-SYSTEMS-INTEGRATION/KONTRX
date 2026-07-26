#ifndef MODBUS_DMA_H
#define MODBUS_DMA_H

#include <stdint.h>
#include "cmsis_os2.h"

#define MAX_RELAYS 10

/* Shared Sensor Data Structure */
typedef struct {
    float ph;
    float ph_temp;
    float orp;
    float orp_temp;
    float ec;
    float ec_temp;
    float do_val;
    float do_temp;
    float ammonia;
    float ammonia_temp;
    float ultrasonic_dist;
    float ultrasonic_temp;
    uint32_t last_update_time;
} Modbus_SensorData_t;

/* Relay State Configuration */
typedef struct {
    uint8_t port_id;  // 0=A, 1=B, 2=C, 3=D, 4=E
    uint8_t pin_num;  // 0..15
    uint8_t is_nc;    // 0 = NO, 1 = NC
    uint8_t state;    // 0 = Off, 1 = On
} Relay_Config_t;

/* Entire System State Config */
typedef struct {
    uint32_t magic;
    Relay_Config_t relays[MAX_RELAYS];
    char mqtt_broker[64];
    uint16_t mqtt_port;
    char mqtt_client_id[32];
    char mqtt_username[32];
    char mqtt_password[32];
    uint8_t sensor_ids[6]; // [0]=ph, [1]=orp, [2]=ec, [3]=do, [4]=ammonia, [5]=ultrasonic
    uint8_t padding[2];
    uint32_t checksum;
} Gateway_Config_t;

#define SCAN_MAX_DEVICES 32
typedef struct {
    uint8_t id;
    uint8_t type; // 1=pH, 2=ORP, 3=EC, 4=DO, 5=Ammonia, 6=Ultrasonic, 255=Unknown Device
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

/* Helper Primitives to safely read/write states */
void Get_Shared_Sensor_Data(Modbus_SensorData_t *dest);
void Update_Shared_Sensor_Data(const Modbus_SensorData_t *src);
void Get_Shared_Config(Gateway_Config_t *dest);
void Update_Shared_Config(const Gateway_Config_t *src);

#endif /* MODBUS_DMA_H */
