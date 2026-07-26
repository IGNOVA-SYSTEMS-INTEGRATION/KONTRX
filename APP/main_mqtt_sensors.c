#include "modbus.h"
#include "uart_stm32.h"
#include "gpio_stm32.h"
#include "spi_stm32.h"
#include "led.h"
#include "w5500.h"
#include "socket.h"
#include "dhcp.h"
#include "mqtt_interface.h"
#include "MQTTClient.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

// Shared structures for multi-tasking
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
} SensorData_t;

SensorData_t latest_sensor_data = {
    .ph = -999.0f, .ph_temp = -999.0f,
    .orp = -999.0f, .orp_temp = -999.0f,
    .ec = -999.0f, .ec_temp = -999.0f,
    .do_val = -999.0f, .do_temp = -999.0f,
    .ammonia = -999.0f, .ammonia_temp = -999.0f
};

SemaphoreHandle_t sensorMutex;
SemaphoreHandle_t mqttMutex;

// Sensor type encoding
typedef enum {
    SENSOR_TYPE_UNKNOWN  = 0,
    SENSOR_TYPE_PH       = 1,  // 16-bit @ 0x0000, ID 0x01
    SENSOR_TYPE_ORP      = 2,  // 16-bit @ 0x0000, ID 0x02
    SENSOR_TYPE_EC       = 3,  // 16-bit @ 0x0000, ID 0x03
    SENSOR_TYPE_DO       = 4,  // 32-bit float @ 0x2600 (any ID)
    SENSOR_TYPE_AMMONIA  = 5,  // 16-bit @ 0x0000, ID 0x05
} SensorType_t;

typedef struct {
    uint8_t      id;
    SensorType_t type;
} ActiveSensor_t;

ActiveSensor_t active_sensors[16];
int num_active_sensors = 0;

// Global state
bool hardware_led_state = false;
bool aerator_state = false;
Network n;
MQTTClient c;
uint8_t dhcp_buffer[1024];
unsigned char mqtt_tx_buf[512];
unsigned char mqtt_rx_buf[512];

// Override _write so standard printf() uses our Debug USART (USART1)
int _write(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) {
        UART_Debug_SendByte((uint8_t)ptr[i]);
    }
    return len;
}

// WIZnet SPI Callbacks
uint8_t W5500_SPI_ReadByte(void) { return SPI2_ReadWriteByte(0xFF); }
void W5500_SPI_WriteByte(uint8_t data) { SPI2_ReadWriteByte(data); }

// WIZnet Timer Hook for FreeRTOS
void vApplicationTickHook(void) {
    MilliTimer_Handler();
}

// Decode DCBA float format from registers
float decodeFloat_DCBA(uint16_t reg0, uint16_t reg1) {
    float value;
    uint8_t *ptr = (uint8_t*)&value;
    ptr[0] = (reg0 >> 8) & 0xFF; // D
    ptr[1] = reg0 & 0xFF;        // C
    ptr[2] = (reg1 >> 8) & 0xFF; // B
    ptr[3] = reg1 & 0xFF;        // A
    return value;
}

// Read both value and temperature for a Modbus sensor in a single transaction
bool Read_Sensor_Data(uint8_t slave_id, SensorType_t type, float *val, float *temp) {
    *val = -999.0f;
    *temp = -999.0f;
    
    if (type == SENSOR_TYPE_DO) {
        // DO Sensor uses 32-bit floats at 0x2600
        uint16_t regs[6];
        if (Modbus_ReadHoldingRegisters(slave_id, 0x2600, 6, regs)) {
            *temp = decodeFloat_DCBA(regs[0], regs[1]); // Temp
            *val  = decodeFloat_DCBA(regs[4], regs[5]); // DO mg/L
            return true;
        }
        return false;
    }
    
    // Other sensors use consecutive 16-bit registers starting at 0x0000
    uint16_t regs[2];
    if (Modbus_ReadHoldingRegisters(slave_id, 0x0000, 2, regs)) {
        uint16_t raw_val  = regs[0];
        uint16_t raw_temp = regs[1];
        
        if (raw_temp != 0x7FFF) {
            *temp = raw_temp / 100.0f;
        }
        if (raw_val != 0x7FFF) {
            switch (type) {
                case SENSOR_TYPE_PH:      *val = raw_val / 100.0f;             break;
                case SENSOR_TYPE_ORP:     *val = (float)((int16_t)raw_val);    break;
                case SENSOR_TYPE_EC:      *val = raw_val / 10.0f;              break;
                case SENSOR_TYPE_AMMONIA: *val = (float)raw_val;               break;
                default: break;
            }
        }
        return true;
    }
    return false;
}

typedef struct {
    float last_val;
    float last_temp;
    uint8_t fail_count;
} SensorFilter_t;

// One filter slot per sensor type (indexed by SensorType_t, 1..5)
static SensorFilter_t sensor_filters[6] = {
    [SENSOR_TYPE_PH]      = { .last_val = -999.0f, .last_temp = -999.0f, .fail_count = 0 },
    [SENSOR_TYPE_ORP]     = { .last_val = -999.0f, .last_temp = -999.0f, .fail_count = 0 },
    [SENSOR_TYPE_EC]      = { .last_val = -999.0f, .last_temp = -999.0f, .fail_count = 0 },
    [SENSOR_TYPE_DO]      = { .last_val = -999.0f, .last_temp = -999.0f, .fail_count = 0 },
    [SENSOR_TYPE_AMMONIA] = { .last_val = -999.0f, .last_temp = -999.0f, .fail_count = 0 },
};

void Update_Sensor_With_Filtering(uint8_t sid, SensorType_t type, float *out_val, float *out_temp) {
    float val, temp;
    if (Read_Sensor_Data(sid, type, &val, &temp)) {
        sensor_filters[type].fail_count = 0;
        sensor_filters[type].last_val   = val;
        sensor_filters[type].last_temp  = temp;
    } else {
        sensor_filters[type].fail_count++;
        if (sensor_filters[type].fail_count >= 5) {
            sensor_filters[type].last_val  = -999.0f;
            sensor_filters[type].last_temp = -999.0f;
        }
    }
    *out_val  = sensor_filters[type].last_val;
    *out_temp = sensor_filters[type].last_temp;
}

// MQTT Downlink Callback
void messageArrived(MessageData* data) {
    char payload_str[128];
    int len = data->message->payloadlen;
    if (len >= sizeof(payload_str)) len = sizeof(payload_str) - 1;
    memcpy(payload_str, data->message->payload, len);
    payload_str[len] = '\0';
    
    printf("\r\n--- MQTT Command Received ---\r\n%s\r\n", payload_str);
    
    if (strstr(payload_str, "\"target\":\"led\"") || strstr(payload_str, "\"target\": \"led\"")) {
        if (strstr(payload_str, "\"value\":\"ON\"") || strstr(payload_str, "\"value\": \"ON\"")) {
            hardware_led_state = true;
            LED_On();
            printf("LED turned ON via MQTT\r\n");
        } else if (strstr(payload_str, "\"value\":\"OFF\"") || strstr(payload_str, "\"value\": \"OFF\"")) {
            hardware_led_state = false;
            LED_Off();
            printf("LED turned OFF via MQTT\r\n");
        }
    }
}

void Scan_Sensors(void) {
    printf("\r\n--- Scanning for connected sensors (IDs 0x01 to 0x10) ---\r\n");
    printf("Please wait...\r\n");
    num_active_sensors = 0;
    
    for (uint8_t id = 1; id <= 0x10; id++) {
        SensorType_t found_type = SENSOR_TYPE_UNKNOWN;
        
        // First try float-series (DO) at 0x2600
        uint8_t resp_2600 = Modbus_ScanSensor(id, 0x2600);
        uint8_t resp_0000 = 0;
        
        if (resp_2600) {
            found_type = SENSOR_TYPE_DO;
        } else {
            // Try 16-bit series at 0x0000 and map by known ID
            resp_0000 = Modbus_ScanSensor(id, 0x0000);
            if (resp_0000) {
                switch (id) {
                    case 0x01: found_type = SENSOR_TYPE_PH;      break;
                    case 0x02: found_type = SENSOR_TYPE_ORP;     break;
                    case 0x03: found_type = SENSOR_TYPE_EC;      break;
                    case 0x04: found_type = SENSOR_TYPE_DO;      break; // DO can respond on 0x0000 too
                    case 0x05: found_type = SENSOR_TYPE_AMMONIA; break;
                    default:   found_type = SENSOR_TYPE_UNKNOWN; break;
                }
            }
        }

        if (found_type != SENSOR_TYPE_UNKNOWN) {
            active_sensors[num_active_sensors].id   = id;
            active_sensors[num_active_sensors].type = found_type;
            num_active_sensors++;
            const char *type_names[] = {"?","pH","ORP","EC","DO","Ammonia"};
            printf(">>> FOUND [%s] ID=0x%02X | 0x2600=%d 0x0000=%d <<<\r\n",
                   type_names[found_type], id, resp_2600, resp_0000);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("Scan complete. Found %d sensor(s).\r\n", num_active_sensors);
}

// --- FREE RTOS TASKS ---

void ModbusTask(void *pvParameters) {
    (void)pvParameters;
    SensorData_t temp_sensor_data;

    // Allow sensors to warm up, then scan them once on boot
    vTaskDelay(pdMS_TO_TICKS(2000));
    Scan_Sensors();

    while(1) {
        // Initialize temp_sensor_data to default values (-999.0f)
        temp_sensor_data.ph = -999.0f;
        temp_sensor_data.ph_temp = -999.0f;
        temp_sensor_data.orp = -999.0f;
        temp_sensor_data.orp_temp = -999.0f;
        temp_sensor_data.ec = -999.0f;
        temp_sensor_data.ec_temp = -999.0f;
        temp_sensor_data.do_val = -999.0f;
        temp_sensor_data.do_temp = -999.0f;
        temp_sensor_data.ammonia = -999.0f;
        temp_sensor_data.ammonia_temp = -999.0f;

        // Poll only connected sensors — dispatch by detected type, not by hardcoded ID
        for (int i = 0; i < num_active_sensors; i++) {
            uint8_t      sid  = active_sensors[i].id;
            SensorType_t type = active_sensors[i].type;
            switch (type) {
                case SENSOR_TYPE_PH:
                    Update_Sensor_With_Filtering(sid, type, &temp_sensor_data.ph, &temp_sensor_data.ph_temp);
                    break;
                case SENSOR_TYPE_ORP:
                    Update_Sensor_With_Filtering(sid, type, &temp_sensor_data.orp, &temp_sensor_data.orp_temp);
                    break;
                case SENSOR_TYPE_EC:
                    Update_Sensor_With_Filtering(sid, type, &temp_sensor_data.ec, &temp_sensor_data.ec_temp);
                    break;
                case SENSOR_TYPE_DO:
                    Update_Sensor_With_Filtering(sid, type, &temp_sensor_data.do_val, &temp_sensor_data.do_temp);
                    break;
                case SENSOR_TYPE_AMMONIA:
                    Update_Sensor_With_Filtering(sid, type, &temp_sensor_data.ammonia, &temp_sensor_data.ammonia_temp);
                    break;
                default: break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // Update latest_sensor_data under protection of sensorMutex
        if (xSemaphoreTake(sensorMutex, portMAX_DELAY)) {
            latest_sensor_data = temp_sensor_data;
            xSemaphoreGive(sensorMutex);
        }
        
        // Read all every 0.5s (500ms)
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void MqttPublishTask(void *pvParameters) {
    (void)pvParameters;
    
    // Give MqttSubscribeTask 5 seconds to establish DHCP and TCP/MQTT connection
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    char local_payload[512];
    SensorData_t local_data;
    
    while(1) {
        // Publish every 1 second
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        if (c.isconnected) {
            // 1. Get latest sensor data safely
            if (xSemaphoreTake(sensorMutex, portMAX_DELAY)) {
                local_data = latest_sensor_data;
                xSemaphoreGive(sensorMutex);
            }
            
            // 2. Format JSON payload
            snprintf(local_payload, sizeof(local_payload),
                "{\n"
                "  \"sensors\": {\n"
                "    \"ph\": %.2f,\n    \"ph_temp\": %.2f,\n"
                "    \"orp\": %.2f,\n    \"orp_temp\": %.2f,\n"
                "    \"ec\": %.2f,\n    \"ec_temp\": %.2f,\n"
                "    \"do\": %.2f,\n    \"do_temp\": %.2f,\n"
                "    \"ammonia\": %.2f,\n    \"ammonia_temp\": %.2f,\n"
                "    \"waterLevel\": 0,\n"
                "    \"flowRate\": 2.1\n"
                "  },\n"
                "  \"actuators\": {\n"
                "    \"ledStatus\": \"%s\",\n"
                "    \"aeratorStatus\": \"%s\"\n"
                "  }\n"
                "}",
                local_data.ph, local_data.ph_temp,
                local_data.orp, local_data.orp_temp,
                local_data.ec, local_data.ec_temp,
                local_data.do_val, local_data.do_temp,
                local_data.ammonia, local_data.ammonia_temp,
                hardware_led_state ? "ON" : "OFF",
                aerator_state ? "ON" : "OFF");
            
            // 3. Publish to MQTT under protection of mqttMutex
            if (xSemaphoreTake(mqttMutex, portMAX_DELAY)) {
                MQTTMessage message;
                message.qos = QOS0;
                message.retained = 0;
                message.dup = 0;
                message.payload = (void*)local_payload;
                message.payloadlen = strlen(local_payload);
                
                int rc = MQTTPublish(&c, "telemetry/dev_stm32_005", &message);
                if (rc != SUCCESSS) {
                    printf("MQTT Publish failed, rc = %d\r\n", rc);
                } else {
                    printf("MQTT Published telemetry successfully!\r\n");
                }
                xSemaphoreGive(mqttMutex);
            }
        }
    }
}

void MqttSubscribeTask(void *pvParameters) {
    (void)pvParameters;
    
    // 1. Hardware Reset W5500
    printf("Hardware Resetting W5500...\r\n");
    W5500_Hardware_Reset();
    
    reg_wizchip_cs_cbfunc(W5500_CS_Select, W5500_CS_Deselect);
    reg_wizchip_spi_cbfunc(W5500_SPI_ReadByte, W5500_SPI_WriteByte);

    uint8_t memsize[2][8] = { {2,2,2,2,2,2,2,2}, {2,2,2,2,2,2,2,2} };
    if(ctlwizchip(CW_INIT_WIZCHIP, (void*)memsize) == -1) {
        printf("WIZCHIP Initialization Failed.\r\n");
        vTaskDelete(NULL);
    }

    uint8_t mac[6] = {0x00, 0x08, 0xDC, 0x11, 0x22, 0x33};
    setSHAR(mac);
    DHCP_init(1, dhcp_buffer);

    if (wizphy_getphylink() == PHY_LINK_OFF) {
        printf("Warning: LAN Cable is Disconnected!\r\n");
        while (wizphy_getphylink() == PHY_LINK_OFF) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("LAN Cable Connected!\r\n");

    printf("Requesting IP via DHCP...\r\n");
    uint32_t dhcp_tick = xTaskGetTickCount();
    int loop_count = 0;
    while (1) {
        uint8_t ret = DHCP_run();
        printf("[DHCP] Loop %d, Tick %lu, Ret %d\r\n", loop_count++, (unsigned long)xTaskGetTickCount(), ret);
        if (ret == DHCP_IP_LEASED || ret == DHCP_IP_ASSIGN) {
            printf("DHCP Success!\r\n");
            break;
        } else if (ret == DHCP_FAILED) {
            printf("DHCP Failed. Retrying...\r\n");
            DHCP_stop();
            DHCP_init(1, dhcp_buffer);
        }
        
        if ((xTaskGetTickCount() - dhcp_tick) >= pdMS_TO_TICKS(1000)) {
            DHCP_time_handler();
            dhcp_tick = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    wiz_NetInfo net_info;
    memcpy(net_info.mac, mac, 6);
    getIPfromDHCP(net_info.ip);
    getGWfromDHCP(net_info.gw);
    getSNfromDHCP(net_info.sn);
    getDNSfromDHCP(net_info.dns);
    net_info.dhcp = NETINFO_DHCP;
    ctlnetwork(CN_SET_NETINFO, (void*)&net_info);

    printf("IP: %d.%d.%d.%d\r\n", net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);

    uint8_t targetIP[4] = {192, 168, 1, 13};
    uint16_t targetPort = 1883;
    NewNetwork(&n, 0); 
    
    MQTTPacket_connectData connect_data = MQTTPacket_connectData_initializer;
    connect_data.willFlag = 0;
    connect_data.MQTTVersion = 3;
    connect_data.clientID.cstring = (char*)"dev_stm32_005";
    connect_data.keepAliveInterval = 60;
    connect_data.cleansession = 1;

    while(1) {
        if (!c.isconnected) {
            printf("Connecting to TCP Network %d.%d.%d.%d:%d...\r\n", targetIP[0], targetIP[1], targetIP[2], targetIP[3], targetPort);
            if (ConnectNetwork(&n, targetIP, targetPort) == SOCK_OK) {
                printf("TCP Connected! Initializing MQTT client...\r\n");
                
                if (xSemaphoreTake(mqttMutex, portMAX_DELAY)) {
                    MQTTClientInit(&c, &n, 5000, mqtt_tx_buf, sizeof(mqtt_tx_buf), mqtt_rx_buf, sizeof(mqtt_rx_buf));
                    
                    int rc = MQTTConnect(&c, &connect_data);
                    if (rc == SUCCESSS) {
                        printf("MQTT Connected successfully!\r\n");
                        MQTTSubscribe(&c, "commands/dev_stm32_005", QOS0, messageArrived);
                    } else {
                        printf("MQTT Connect failed, rc = %d\r\n", rc);
                    }
                    xSemaphoreGive(mqttMutex);
                }
            } else {
                printf("TCP Connection failed.\r\n");
            }
            // Delay before retry
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            // We are connected. Call MQTTYield continuously to receive messages.
            if (xSemaphoreTake(mqttMutex, portMAX_DELAY)) {
                int rc = MQTTYield(&c, 10);
                if (rc != SUCCESSS) {
                    printf("MQTTYield connection error, rc = %d\r\n", rc);
                }
                xSemaphoreGive(mqttMutex);
            }
            
            // Handle DHCP maintenance
            if ((xTaskGetTickCount() - dhcp_tick) >= pdMS_TO_TICKS(1000)) {
                DHCP_time_handler();
                DHCP_run();
                dhcp_tick = xTaskGetTickCount();
            }
            
        }
    }
}

void ButtonTask(void *pvParameters) {
    (void)pvParameters;
    uint8_t last_button_state = 0;
    
    while(1) {
        uint8_t current_button_state = GPIO_Read_PE4();
        if (current_button_state == 1 && last_button_state == 0) {
            aerator_state = !aerator_state;
            printf("Button Pressed! Aerator State: %s\r\n", aerator_state ? "ON" : "OFF");
        }
        last_button_state = current_button_state;
        
        vTaskDelay(pdMS_TO_TICKS(20)); // Poll button every 20ms
    }
}

int main(void) {
    // 1. Hardware Initialization
    GPIO_Init_USART1_Pins();
    UART_Debug_Init();
    GPIO_Init_USART3_Pins();
    UART_Init();
    
    static Modbus_Interface_t modbus_if = {
        .uart_send_byte = UART_Modbus_SendByte,
        .wait_tx_complete = UART_Modbus_WaitTransmissionComplete,
        .uart_receive_byte = UART_Modbus_ReceiveByte,
        .uart_receive_byte_timeout = UART_Modbus_ReceiveByte_Timeout,
        .enable_tx_mode = MAX485_Transmit_Enable,
        .enable_rx_mode = MAX485_Receive_Enable
    };
    Modbus_Init(&modbus_if);
    
    LED_Init();
    GPIO_Init_MAX485_Pins();
    GPIO_Init_PE4_Button();

    GPIO_Init_W5500_Pins();
    SPI2_Init();

    printf("\r\n=== STM32F407 FreeRTOS MQTT Multi-Sensor ===\r\n");

    // Set Priority Grouping to 4 (all preemption, no subpriority) for FreeRTOS
    uint32_t *AIRCR = (uint32_t *)0xE000ED0C;
    *AIRCR = 0x05FA0000 | (3 << 8);

    // Create Mutexes
    sensorMutex = xSemaphoreCreateMutex();
    mqttMutex = xSemaphoreCreateMutex();

    // Create Tasks
    xTaskCreate(MqttSubscribeTask, "MqttSubTask", 1024, NULL, 2, NULL);
    xTaskCreate(MqttPublishTask,   "MqttPubTask", 1024, NULL, 2, NULL);
    xTaskCreate(ModbusTask,        "ModbusTask",  1024, NULL, 1, NULL);
    xTaskCreate(ButtonTask,        "ButtonTask",  256,  NULL, 1, NULL);

    // Start Scheduler
    vTaskStartScheduler();

    // Should never reach here
    while(1);
    return 0;
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    printf("\r\n!!! STACK OVERFLOW in task: %s !!!\r\n", pcTaskName);
    while(1);
}

void vApplicationMallocFailedHook(void) {
    printf("\r\n!!! MALLOC FAILED !!!\r\n");
    while(1);
}
