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

static Gateway_Config_t s_modbus_cfg;
#define cfg s_modbus_cfg

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
    Get_Shared_Config(&cfg);
    if (cfg.magic != CONFIG_MAGIC_CURRENT) {
        memset(&cfg, 0, sizeof(cfg));
        cfg.magic = CONFIG_MAGIC_CURRENT;
        cfg.sensors.count = 0;
        cfg.actuator_count = 0;
        cfg.serial = 1; /* default serial for a fresh device */
        cfg.mqtt_port = 1883;
        cfg.mqtt_send_mode = 0;
        Update_Shared_Config(&cfg);
    }
}

/*
 * Modbus_Safe_Transaction — one blocking RS485 read (polling, no DMA).
 *
 * WHY THIS IS SLOW (> 1 ms per call, unavoidable):
 *   UART runs at 9600 baud (USART3 BRR=0x0683 => ~9598 baud), so one byte
 *   takes ~1.04 ms on the wire. A single 8-byte request + 3.5-char gap +
 *   9-byte response is ~21 ms minimum, plus the slave's own reply delay.
 *   Modbus RTU protocol REQUIRES the 3.5-char silent gap between frames,
 *   so this can never be reduced below ~20 ms without raising the baud.
 *
 * CONCURRENCY IMPACT:
 *   This function BUSY-WAITS (no osDelay in the RX loop). It runs in
 *   Task_ModbusSensorPoll (priority AboveNormal=3), which is LOWER than the
 *   1ms Control Engine (Realtime=5). FreeRTOS preempts on the tick, so the
 *   Control Engine still fires every 1ms — it only reads the mutex-protected
 *   snapshot and never touches UART. The HTTP/OTA tasks (priority 1-2) are
 *   the ones starved during the busy-wait, which is acceptable.
 */
/*
 * Modbus_Safe_Transaction — one blocking RS485 read (polling, no DMA).
 *
 * timeout_ms: how long to wait for the slave's first byte.
 *   Fast sensors (pH/ORP/EC/Ammonia/Ultrasonic): 80ms  — respond in <50ms
 *   Slow sensors (DO KWS-630):                  300ms  — response can take >100ms
 *   Multi-US per channel:                        80ms  — each channel is fast
 *
 * WHY timeout matters for stability:
 *   Old code used a fixed 250ms for ALL sensors. A dead sensor wastes 250ms
 *   before the next poll starts. With 5 configured sensors and 2 missing:
 *   2 × 250ms = 500ms wasted per cycle, causing data to look stale and the
 *   MQTT payload to always be incomplete.
 */
static uint8_t Modbus_Safe_Transaction_T(uint8_t slave, uint8_t fc,
                                          uint16_t reg, uint8_t num,
                                          uint16_t *out, uint32_t timeout_ms) {
    uint8_t f[8] = {slave, fc, reg>>8, reg&0xFF, 0, num, 0, 0};
    uint16_t crc = Modbus_CRC16(f, 6);
    f[6] = crc & 0xFF; f[7] = crc >> 8;

    /* 1. Flush any stale bytes from UART RX buffer to guarantee a clean start */
    while (USART3->SR & (1U << 5)) {
        volatile uint32_t junk = USART3->DR;
        (void)junk;
    }
    /* Clear error flags */
    if (USART3->SR & 0x0F) {
        volatile uint32_t junk_dr = USART3->DR;
        (void)junk_dr;
    }

    /* 2. Switch MAX485 to TX mode and settle without yielding the CPU */
    MAX485_TX();
    for (volatile int d = 0; d < 2000; d++); /* microsecond settle delay */

    /* 3. Send Modbus query */
    for(int i=0; i<8; i++) {
        uint32_t tx_timeout = 10000;
        while(!(USART3->SR & (1U << 7)) && --tx_timeout);
        USART3->DR = f[i];
    }
    uint32_t tc_timeout = 20000;
    while(!(USART3->SR & (1U << 6)) && --tc_timeout);

    /* 4. Switch back to RX mode immediately */
    MAX485_RX();

    /* 5. Read response with noise rejection state machine */
    uint8_t rx[50]; int received = 0;
    uint32_t start = osKernelGetTickCount();
    
    while((osKernelGetTickCount() - start) < timeout_ms) {
        if(USART3->SR & (1U << 5)) {
            uint8_t b = (uint8_t)USART3->DR;

            /* Reject leading noise: first byte must be the target slave ID */
            if (received == 0 && b != slave) {
                continue;
            }
            
            /* Reject frame shift: second byte must match function code */
            if (received == 1 && b != fc) {
                received = 0;
                if (b == slave) {
                    rx[received++] = b;
                }
                continue;
            }
            
            if(received < 50) {
                rx[received++] = b;
            }
            
            /* Check if we got the expected packet length */
            if(received >= (3 + num*2 + 2)) {
                break;
            }
        } else {
            /* No byte yet — sleep for 1ms to allow lower-priority tasks
             * (HTTP dashboard, MQTT client) to run. */
            osDelay(1);
        }
        
        /* Clean any hardware errors (Overrun, Framing, Noise) on the fly */
        if(USART3->SR & 0x0F) {
            volatile uint32_t d = USART3->DR;
            (void)d;
        }
    }
    
    if(received < 5 || rx[0] != slave || rx[1] != fc) return 0;
    if(Modbus_CRC16(rx, received-2) != (rx[received-2]|(rx[received-1]<<8))) return 0;
    
    for(int i=0; i<num; i++) out[i] = (rx[3+i*2]<<8) | rx[4+i*2];
    return 1;
}

/* Backward-compatible wrapper with optimized 40ms default */
static uint8_t Modbus_Safe_Transaction(uint8_t slave, uint8_t fc,
                                        uint16_t reg, uint8_t num,
                                        uint16_t *out) {
    return Modbus_Safe_Transaction_T(slave, fc, reg, num, out, 40);
}

void Modbus_DMA_PollSensors(void) {
    /* ----------------------------------------------------------------
     * Latency & Timing Optimization:
     * - Fast sensors (pH/EC/ORP/Ammonia) use 40ms timeout.
     * - DO KWS-630 uses 150ms timeout.
     * - We read only ONE Multi-US channel or ONE slow sensor sub-step
     *   per poll cycle, OR we limit Multi-US polling to avoid starvation.
     *   Since Multi-US has 8 channels, polling them all at once takes too
     *   long. We now poll 2 channels of the Multi-US board per cycle,
     *   distributing the 8 channels over 4 cycles.
     * ---------------------------------------------------------------- */
    static Modbus_SensorData_t local;
    Get_Shared_Sensor_Data(&local);
    Get_Shared_Config(&cfg);
    uint16_t rs[12];

    /* Keep track of Multi-US channel index across poll cycles to distribute load */
    static uint8_t multi_us_channel_offset = 0;

    /* Sync the slot count to match the current config.
     * Carry over values for slots that match by (type, id). */
    if (local.readings_count != cfg.sensors.count) {
        printf("[MODBUS] Syncing sensor slot count from %u to %u\r\n", local.readings_count, cfg.sensors.count);
        static Modbus_SensorData_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.multi_us_count = 0;

        for (uint8_t s = 0; s < cfg.sensors.count && s < MAX_SENSORS; s++) {
            uint8_t t = cfg.sensors.entries[s].type;
            uint8_t id = cfg.sensors.entries[s].id;
            tmp.readings[s].type  = t;
            tmp.readings[s].id    = id;
            tmp.readings[s].stale = 1;
            tmp.readings[s].valid = 0;

            for (uint8_t k = 0; k < local.readings_count; k++) {
                if (local.readings[k].type == t && local.readings[k].id == id) {
                    tmp.readings[s].value      = local.readings[k].value;
                    tmp.readings[s].temp       = local.readings[k].temp;
                    tmp.readings[s].last_ok_ms = local.readings[k].last_ok_ms;
                    tmp.readings[s].stale      = local.readings[k].stale;
                    break;
                }
            }
        }
        tmp.readings_count = cfg.sensors.count;
        local = tmp;
    }

    uint8_t mu_idx = 0;

    for (uint8_t s = 0; s < cfg.sensors.count && s < MAX_SENSORS; s++) {
        uint8_t type = cfg.sensors.entries[s].type;
        uint8_t sid  = cfg.sensors.entries[s].id;
        SensorReading_t *rd = &local.readings[s];

        rd->type = type;
        rd->id   = sid;

        uint8_t ok = 0;

        switch (type) {
            case 1: /* pH — fast sensor, 40ms timeout */
                if (Modbus_Safe_Transaction_T(sid, 0x03, 0, 2, rs, 40)) {
                    rd->value = rs[0] / 100.0f;
                    rd->temp  = rs[1] / 100.0f;
                    ok = 1;
                }
                break;

            case 2: /* ORP — fast, 40ms */
                if (Modbus_Safe_Transaction_T(sid, 0x03, 0, 2, rs, 40)) {
                    rd->value = (float)rs[0];
                    rd->temp  = rs[1] / 100.0f;
                    ok = 1;
                }
                break;

            case 3: /* EC — fast, 40ms */
                if (Modbus_Safe_Transaction_T(sid, 0x03, 0, 2, rs, 40)) {
                    rd->value = rs[0] / 10.0f;
                    rd->temp  = rs[1] / 100.0f;
                    ok = 1;
                }
                break;

            case 4: /* DO (KWS-630) — slow sensor, 150ms timeout */
                if (Modbus_Safe_Transaction_T(sid, 0x03, 0x2600, 6, rs, 150)) {
                    rd->temp  = Decode_Float_DCBA(rs[0], rs[1]);
                    rd->value = Decode_Float_DCBA(rs[4], rs[5]);
                    ok = 1;
                }
                break;

            case 5: /* Ammonia — fast, 40ms */
                if (Modbus_Safe_Transaction_T(sid, 0x03, 0, 2, rs, 40)) {
                    rd->value = (float)rs[0];
                    rd->temp  = rs[1] / 100.0f;
                    ok = 1;
                }
                break;

            case 6: /* Ultrasonic single — fast, 40ms */
                if (Modbus_Safe_Transaction_T(sid, 0x03, 0, 10, rs, 40)) {
                    rd->temp  = rs[8] / 10.0f;
                    rd->value = rs[9] / 10.0f;
                    ok = 1;
                }
                break;

            case 7: /* Multi-US board — 8 channels */
                if (mu_idx < MAX_MULTI_US) {
                    MultiUS_t *mu = &local.multi_us[mu_idx];
                    
                    /* Restore previous board metadata */
                    for (uint8_t k = 0; k < local.multi_us_count; k++) {
                        if (local.multi_us[k].id == sid) {
                            *mu = local.multi_us[k];
                            break;
                        }
                    }
                    mu->id = sid;

                    /* Poll 2 channels of the 8 in this cycle to avoid blocking the bus */
                    for (uint8_t c = 0; c < 2; c++) {
                        uint8_t ch = (multi_us_channel_offset + c) % 8;
                        if (Modbus_Safe_Transaction_T(sid, 0x03, ch * 0x10, 3, rs, 40)) {
                            mu->dist[ch] = (float)rs[0];
                        }
                        osDelay(15); /* 15ms inter-channel gap for line discharge */
                    }

                    /* Calculate average from all channels that have valid values */
                    uint32_t sum = 0; uint8_t valid = 0;
                    for (uint8_t i = 0; i < 8; i++) {
                        if (mu->dist[i] > 0.0f && mu->dist[i] < 10000.0f) {
                            sum += (uint32_t)mu->dist[i];
                            valid++;
                        }
                    }
                    if (valid > 0) {
                        mu->avg = (float)(sum / valid);
                        ok = 1;
                    }

                    /* Poll temp & config compensation once in a while */
                    if (multi_us_channel_offset == 0) {
                        if (Modbus_Safe_Transaction_T(sid, 0x03, 0x0080, 3, rs, 40)) {
                            mu->temp = rs[0] / 10.0f;
                        }
                        if (Modbus_Safe_Transaction_T(sid, 0x04, 0x0000, 3, rs, 40)) {
                            mu->comp = (float)rs[2];
                        }
                    }

                    if (mu_idx >= local.multi_us_count) local.multi_us_count = mu_idx + 1;
                    mu_idx++;
                    rd->value = mu->avg;
                    rd->temp  = mu->temp;
                }
                break;

            default:
                break;
        }

        uint8_t old_valid = rd->valid;
        if (ok) {
            rd->valid      = 1;
            rd->stale      = 0;
            rd->last_ok_ms = osKernelGetTickCount();
            if (old_valid == 0) {
                extern void Log_Event(const char *category, const char *message);
                const char *tname = "Unknown";
                if (rd->type == 1) tname = "pH";
                else if (rd->type == 2) tname = "ORP";
                else if (rd->type == 3) tname = "EC";
                else if (rd->type == 4) tname = "DO";
                else if (rd->type == 5) tname = "Ammonia";
                else if (rd->type == 6) tname = "Ultrasonic";
                else if (rd->type == 7) tname = "Multi-US";
                
                 char log_buf[64];
                snprintf(log_buf, sizeof(log_buf), "Sensor %s (ID %u) connected.", tname, rd->id);
                printf("[MODBUS] Sensor Connection: %s (ID %u) is now ONLINE\r\n", tname, rd->id);
                Log_Event("MODBUS", log_buf);
            }
        } else {
            /* Keep last-known-good value; mark as not valid THIS cycle */
            rd->valid = 0;
            /* stale = 1 only if we've NEVER had a good read */
            if (rd->last_ok_ms > 0) rd->stale = 0;
            if (old_valid == 1) {
                extern void Log_Event(const char *category, const char *message);
                const char *tname = "Unknown";
                if (rd->type == 1) tname = "pH";
                else if (rd->type == 2) tname = "ORP";
                else if (rd->type == 3) tname = "EC";
                else if (rd->type == 4) tname = "DO";
                else if (rd->type == 5) tname = "Ammonia";
                else if (rd->type == 6) tname = "Ultrasonic";
                else if (rd->type == 7) tname = "Multi-US";
                
                char log_buf[64];
                snprintf(log_buf, sizeof(log_buf), "Sensor %s (ID %u) disconnected.", tname, rd->id);
                printf("[MODBUS] Sensor Connection Warning: %s (ID %u) is now OFFLINE\r\n", tname, rd->id);
                Log_Event("MODBUS", log_buf);
            }
        }

        /* 35ms inter-sensor gap (was 15ms) — guarantees line settle time between different devices */
        osDelay(35);
    }

    /* Advance the distributed channel offset for the next poll cycle */
    multi_us_channel_offset = (multi_us_channel_offset + 2) % 8;

    local.last_update_time = osKernelGetTickCount();
    Update_Shared_Sensor_Data(&local);
}

void Modbus_DMA_ConsumeBatch(TelemetryBatch_t *dest) {
    if (!dest) return;

    if (osKernelGetState() != osKernelRunning || sensorMutex == NULL) {
        dest->count = sharedSensorData.readings_count;
        for (uint8_t i = 0; i < sharedSensorData.readings_count && i < MAX_SENSORS; i++) {
            SensorReading_t *rd = &sharedSensorData.readings[i];
            dest->records[i].type = rd->type;
            dest->records[i].id   = rd->id;
            dest->records[i].avg_value         = rd->value;
            dest->records[i].avg_temp          = rd->temp;
            dest->records[i].samples_collected = 1;
            dest->records[i].valid             = rd->valid;
        }
        return;
    }

    if (osMutexAcquire(sensorMutex, 50) == osOK) {
        dest->count = sharedSensorData.readings_count;
        for (uint8_t i = 0; i < sharedSensorData.readings_count && i < MAX_SENSORS; i++) {
            SensorReading_t *rd = &sharedSensorData.readings[i];
            dest->records[i].type = rd->type;
            dest->records[i].id   = rd->id;
            dest->records[i].avg_value         = rd->value;
            dest->records[i].avg_temp          = rd->temp;
            dest->records[i].samples_collected = 1;
            dest->records[i].valid             = rd->valid;
        }
        osMutexRelease(sensorMutex);
    } else {
        dest->count = 0;
    }
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
        osDelay(10); /* 10ms between probed addresses: lets a non-responding
                      * slave's timeout elapse before the next probe. A full
                      * network scan sweeps 247 addresses => many seconds,
                      * far beyond 1ms (it's a one-shot background operation). */
    }
    g_scan_status.is_scanning = 0;
}

/* دوال الوصول الآمنة بأسماء المشروع الأصلية */
void Get_Shared_Sensor_Data(Modbus_SensorData_t *dest) {
    if (osKernelGetState() != osKernelRunning || sensorMutex == NULL) {
        memcpy(dest, &sharedSensorData, sizeof(Modbus_SensorData_t));
        return;
    }

    if(osMutexAcquire(sensorMutex, 10) == osOK) { /* 10ms timeout — copy is fast */
        memcpy(dest, &sharedSensorData, sizeof(Modbus_SensorData_t));
        osMutexRelease(sensorMutex);
    } else {
        /* Timeout: return stale copy rather than blocking the caller */
        memcpy(dest, &sharedSensorData, sizeof(Modbus_SensorData_t));
    }
}

void Update_Shared_Sensor_Data(const Modbus_SensorData_t *src) {
    if (osKernelGetState() != osKernelRunning || sensorMutex == NULL) {
        memcpy(&sharedSensorData, src, sizeof(Modbus_SensorData_t));
        return;
    }

    if(osMutexAcquire(sensorMutex, 10) == osOK) { /* 10ms timeout — copy is fast */
        memcpy(&sharedSensorData, src, sizeof(Modbus_SensorData_t));
        osMutexRelease(sensorMutex);
    }
    /* On timeout: drop the update rather than stalling; Modbus will retry next cycle */
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