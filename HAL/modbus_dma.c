/**
 * @file    modbus_dma.c
 * @brief   Kontrx — Safe Polling Modbus (Fixed Linker & Reset Issues)
 */

#include "modbus_dma.h"
#include "stm32f407_regs.h"
#include "cmsis_os2.h"
#include <string.h>
#include <stdio.h>

/* التعريفات العالمية المطلوبة للمشروع */
osMutexId_t         sensorMutex = NULL;
osMutexId_t         configMutex = NULL;
Modbus_SensorData_t sharedSensorData = { .last_update_time = 0 };
Gateway_Config_t    sharedConfig;
uint8_t             relayStates[MAX_RELAYS];
volatile ModbusScanStatus_t g_scan_status = {0};

/* المساعدات */
static uint16_t Modbus_CRC16(const uint8_t *buf, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
            else               { crc >>= 1; }
        }
    }
    return crc;
}

static float Decode_Float_DCBA(uint16_t r0, uint16_t r1) {
    /* DCBA (little-endian) byte order — used by KWS-630 DO sensor.
     * reg0 = D(high) | C(low),  reg1 = B(high) | A(low) */
    union { float f; uint8_t b[4]; } u;
    u.b[0] = (uint8_t)(r0 >> 8);    /* D = LSB */
    u.b[1] = (uint8_t)(r0 & 0xFF);  /* C */
    u.b[2] = (uint8_t)(r1 >> 8);    /* B */
    u.b[3] = (uint8_t)(r1 & 0xFF);  /* A = MSB */
    return u.f;
}

static inline void MAX485_TX(void) { GPIOD->BSRR = (1U << 3) | (1U << 2); }
static inline void MAX485_RX(void) { GPIOD->BSRR = (1U << (3+16)) | (1U << (2+16)); }

void Modbus_DMA_Init(void) {
    if (!sensorMutex) sensorMutex = osMutexNew(NULL);
    if (!configMutex) configMutex = osMutexNew(NULL);
    RCC->AHB1ENR |= (1U << 1) | (1U << 3); RCC->APB1ENR |= (1U << 18);
    GPIOB->MODER &= ~((3U << 20) | (3U << 22));
    GPIOB->MODER |=  ((2U << 20) | (2U << 22));
    GPIOB->OSPEEDR |= ((2U << 20) | (2U << 22));
    GPIOB->PUPDR |= ((1U << 20) | (1U << 22));
    GPIOB->AFR[1] &= ~((0xFU << 8) | (0xFU << 12));
    GPIOB->AFR[1] |= ((7U << 8) | (7U << 12));
    GPIOD->MODER &= ~((3U << 4) | (3U << 6));
    GPIOD->MODER |= ((1U << 4) | (1U << 6));
    GPIOD->OSPEEDR |= ((2U << 4) | (2U << 6));
    MAX485_RX();
    USART3->BRR = 0x0683;
    USART3->CR2 = 0;
    USART3->CR1 = (1U << 13) | (1U << 3) | (1U << 2);

    /* Default config: empty sensor list (user adds via Auto-Scan / UI) + default relays */
    Gateway_Config_t cfg;
    Get_Shared_Config(&cfg);
    if (cfg.magic != CONFIG_MAGIC_CURRENT) {
        memset(&cfg, 0, sizeof(cfg));
        cfg.magic = CONFIG_MAGIC_CURRENT;
        cfg.sensors.count = 0;
        cfg.relay_count = 0;
        cfg.mqtt_port = 1883;
        Update_Shared_Config(&cfg);
    }
}

static uint8_t Modbus_Safe_Transaction(uint8_t slave, uint8_t fc, uint16_t reg, uint8_t num, uint16_t *out) {
    uint8_t f[8] = {slave, fc, reg>>8, reg&0xFF, 0, num, 0, 0};
    uint16_t crc = Modbus_CRC16(f, 6);
    f[6] = crc & 0xFF; f[7] = crc >> 8;

    volatile uint32_t sr = USART3->SR; volatile uint32_t dr = USART3->DR; (void)sr; (void)dr;

    MAX485_TX();
    osDelay(2);
    for(int i=0; i<8; i++) {
        while(!(USART3->SR & (1U << 7)));
        USART3->DR = f[i];
    }
    uint32_t tc_timeout = 20000;
    while(!(USART3->SR & (1U << 6)) && --tc_timeout);
    MAX485_RX();

    uint8_t rx[50]; int received = 0;
    uint32_t start = osKernelGetTickCount();
    while((osKernelGetTickCount() - start) < 100) {
        if(USART3->SR & (1U << 5)) {
            if(received < 50) rx[received++] = (uint8_t)USART3->DR;
            if(received >= (3 + num*2 + 2)) break;
        }
        if(USART3->SR & 0x0F) { volatile uint32_t d = USART3->DR; (void)d; }
    }
    if(received < 5 || rx[0] != slave || rx[1] != fc) return 0;
    if(Modbus_CRC16(rx, received-2) != (rx[received-2]|(rx[received-1]<<8))) return 0;
    for(int i=0; i<num; i++) out[i] = (rx[3+i*2]<<8) | rx[4+i*2];
    return 1;
}

void Modbus_DMA_PollSensors(void) {
    Modbus_SensorData_t local;
    Get_Shared_Sensor_Data(&local);
    Gateway_Config_t cfg;
    Get_Shared_Config(&cfg);
    uint16_t rs[12];

    local.readings_count = 0;
    local.multi_us_count = 0;
    memset(local.readings, 0, sizeof(local.readings));
    memset(local.multi_us, 0, sizeof(local.multi_us));

    for (uint8_t s = 0; s < cfg.sensors.count && s < MAX_SENSORS; s++) {
        uint8_t type = cfg.sensors.entries[s].type;
        uint8_t sid  = cfg.sensors.entries[s].id;
        SensorReading_t *rd = &local.readings[local.readings_count];
        rd->type = type;
        rd->id   = sid;
        rd->valid = 0;

        switch (type) {
            case 1: /* pH  */
                if (Modbus_Safe_Transaction(sid, 0x03, 0, 2, rs)) {
                    rd->value = rs[0] / 100.0f;
                    rd->temp  = rs[1] / 100.0f;
                    rd->valid = 1;
                }
                break;
            case 2: /* ORP */
                if (Modbus_Safe_Transaction(sid, 0x03, 0, 2, rs)) {
                    rd->value = (float)rs[0];
                    rd->temp  = rs[1] / 100.0f;
                    rd->valid = 1;
                }
                break;
            case 3: /* EC  */
                if (Modbus_Safe_Transaction(sid, 0x03, 0, 2, rs)) {
                    rd->value = rs[0] / 10.0f;
                    rd->temp  = rs[1] / 100.0f;
                    rd->valid = 1;
                }
                break;
            case 4: /* DO  */
                if (Modbus_Safe_Transaction(sid, 0x03, 0x2600, 6, rs)) {
                    rd->temp  = Decode_Float_DCBA(rs[0], rs[1]);
                    rd->value = Decode_Float_DCBA(rs[4], rs[5]);
                    rd->valid = 1;
                }
                break;
            case 5: /* Ammonia */
                if (Modbus_Safe_Transaction(sid, 0x03, 0, 2, rs)) {
                    rd->value = (float)rs[0];
                    rd->temp  = rs[1] / 100.0f;
                    rd->valid = 1;
                }
                break;
            case 6: /* Ultrasonic (single) */
                if (Modbus_Safe_Transaction(sid, 0x03, 0, 10, rs)) {
                    rd->temp  = rs[8] / 10.0f;
                    rd->value = rs[9] / 10.0f;
                    rd->valid = 1;
                }
                break;
            case 7: /* Multi-US board */
                if (local.multi_us_count < MAX_MULTI_US) {
                    MultiUS_t *mu = &local.multi_us[local.multi_us_count];
                    memset(mu, 0, sizeof(*mu));
                    mu->id = sid;
                    uint32_t sum = 0; uint8_t valid = 0;
                    for (uint8_t i = 0; i < 8; i++) {
                        if (Modbus_Safe_Transaction(sid, 0x03, i * 0x10, 3, rs)) {
                            mu->dist[i] = (float)rs[0];
                            sum += rs[0]; valid++;
                        } else {
                            mu->dist[i] = -1000.0f;
                        }
                        osDelay(20);
                    }
                    if (valid) mu->avg = (float)(sum / valid);
                    if (Modbus_Safe_Transaction(sid, 0x03, 0x0080, 3, rs)) {
                        mu->temp = rs[0] / 10.0f;
                    }
                    if (Modbus_Safe_Transaction(sid, 0x04, 0x0000, 3, rs)) {
                        mu->comp = (float)rs[2];
                        if (!valid) mu->avg = (float)rs[0];
                        if (valid == 0) mu->temp = rs[1] / 10.0f;
                    }
                    local.multi_us_count++;
                    rd->value = mu->avg;
                    rd->temp  = mu->temp;
                    rd->valid = valid;
                }
                break;
            default:
                break;
        }

        if (rd->valid) local.readings_count++;
        osDelay(50);
    }

    local.last_update_time = osKernelGetTickCount();
    Update_Shared_Sensor_Data(&local);
}

void Modbus_DMA_PerformScan(void) {
    g_scan_status.is_scanning = 1;
    g_scan_status.count = 0;
    uint16_t d[12];
    for(int i=1; i<=247; i++) {
        if(!g_scan_status.is_scanning) break;
        uint8_t type = 0;
        /* Check for DO sensor (Kacise KWS-630) */
        if(Modbus_Safe_Transaction(i, 0x03, 0x2600, 6, d)) {
            type = 4;
        /* Check for multi-ultrasonic board (read US#1 block at 0x00) */
        } else if(Modbus_Safe_Transaction(i, 0x03, 0x00, 3, d)) {
            if(d[0] > 0 && d[0] < 10000 && d[2] == 0) {
                if(Modbus_Safe_Transaction(i, 0x03, 0x10, 3, d))
                    type = 7;
            }
            if(!type && Modbus_Safe_Transaction(i, 0x03, 0, 10, d)) {
                type = 6;
            }
            if(!type) type = 1;
        }
        if(type && g_scan_status.count < 32) {
            g_scan_status.devices[g_scan_status.count].id = i;
            g_scan_status.devices[g_scan_status.count].type = type;
            g_scan_status.count++;
        }
        g_scan_status.progress = (i * 100) / 247;
        osDelay(10);
    }
    g_scan_status.is_scanning = 0;
}

/* دوال الوصول الآمنة بأسماء المشروع الأصلية */
void Get_Shared_Sensor_Data(Modbus_SensorData_t *dest) {
    if (osKernelGetState() != osKernelRunning || sensorMutex == NULL) {
        memcpy(dest, &sharedSensorData, sizeof(Modbus_SensorData_t));
        return;
    }

    if(osMutexAcquire(sensorMutex, 100) == osOK) {
        memcpy(dest, &sharedSensorData, sizeof(Modbus_SensorData_t));
        osMutexRelease(sensorMutex);
    }
}

void Update_Shared_Sensor_Data(const Modbus_SensorData_t *src) {
    if (osKernelGetState() != osKernelRunning || sensorMutex == NULL) {
        memcpy(&sharedSensorData, src, sizeof(Modbus_SensorData_t));
        return;
    }

    if(osMutexAcquire(sensorMutex, 100) == osOK) {
        memcpy(&sharedSensorData, src, sizeof(Modbus_SensorData_t));
        osMutexRelease(sensorMutex);
    }
}

void Get_Shared_Config(Gateway_Config_t *dest) {
    if (osKernelGetState() != osKernelRunning || configMutex == NULL) {
        memcpy(dest, &sharedConfig, sizeof(Gateway_Config_t));
        return;
    }
    
    if (osMutexAcquire(configMutex, 100) == osOK) {
        memcpy(dest, &sharedConfig, sizeof(Gateway_Config_t));
        osMutexRelease(configMutex);
    }
}

void Update_Shared_Config(const Gateway_Config_t *src) {
    if (osKernelGetState() != osKernelRunning || configMutex == NULL) {
        memcpy(&sharedConfig, src, sizeof(Gateway_Config_t));
        return;
    }

    if (osMutexAcquire(configMutex, 100) == osOK) {
        memcpy(&sharedConfig, src, sizeof(Gateway_Config_t));
        osMutexRelease(configMutex);
    }
}   

void USART3_IRQHandler(void) { }
void DMA1_Stream3_IRQHandler(void) { }