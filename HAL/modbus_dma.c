/**
 * @file    modbus_dma.c
 * @brief   Kontrx — Safe Polling Modbus (Fixed Linker & Reset Issues)
 */

#include "modbus_dma.h"
#include "dsp_filter.h"
#include "stm32f407_regs.h"
#include "rcc_stm32.h"
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
volatile uint8_t    g_config_changed = 1; // Keep for compatibility if needed, but we will define version counter
volatile uint32_t   g_config_version = 1;

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

/* USART3 RX ring buffer, filled by the RXNE interrupt. A single-byte DR
 * with task polling loses bytes (overrun) whenever a higher-priority task
 * runs for more than one character time (~1.04 ms at 9600 baud). */
#define RS485_RX_BUF_SIZE 256U
static volatile uint8_t  s_rx_buf[RS485_RX_BUF_SIZE];
static volatile uint16_t s_rx_head = 0;
static volatile uint16_t s_rx_tail = 0;

/* Bus diagnostics: raw byte count, framing/noise and overrun errors, and the
 * last 4 raw bytes seen. Zero bytes => wiring/slave silent; bytes with
 * framing errors => baud/format mismatch. */
static volatile uint32_t s_rx_bytes = 0;
static volatile uint32_t s_rx_fe    = 0;
static volatile uint32_t s_rx_ore   = 0;
static volatile uint8_t  s_rx_last[4];

void USART3_IRQHandler(void) {
    uint32_t sr = USART3->SR;
    if (sr & ((1U << 5) | 0x0FU)) {
        uint8_t b = (uint8_t)USART3->DR; /* DR read also clears ORE/FE/NE/PE */
        if (sr & ((1U << 1) | (1U << 2))) s_rx_fe++;
        if (sr & (1U << 3)) s_rx_ore++;
        if (sr & (1U << 5)) {
            s_rx_bytes++;
            s_rx_last[0] = s_rx_last[1];
            s_rx_last[1] = s_rx_last[2];
            s_rx_last[2] = s_rx_last[3];
            s_rx_last[3] = b;
            uint16_t next = (uint16_t)((s_rx_head + 1U) % RS485_RX_BUF_SIZE);
            if (next != s_rx_tail) {
                s_rx_buf[s_rx_head] = b;
                s_rx_head = next;
            }
        }
    }
}

static void RS485_RxFlush(void) {
    s_rx_tail = s_rx_head;
}

static int RS485_RxGet(uint8_t *b) {
    if (s_rx_tail == s_rx_head) return 0;
    *b = s_rx_buf[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) % RS485_RX_BUF_SIZE);
    return 1;
}

/* Response window = slave turnaround allowance + wire time of the expected
 * reply (~1.1 ms per byte at 9600 8N1, with margin). */
static uint32_t RS485_ReplyTimeout(uint32_t turnaround_ms, uint32_t reply_bytes) {
    return turnaround_ms + (reply_bytes * 11U + 9U) / 10U;
}

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
    /* Compute BRR from the live PCLK1 (42 MHz at 168 MHz SYSCLK). The old
     * hardcoded 0x0683 assumed 16 MHz HSI and produced ~25.2 kbaud. */
    {
        uint32_t pclk1 = RCC_GetPCLK1Freq();
        uint32_t div_x16 = (2U * pclk1 + 9600U) / (2U * 9600U);
        USART3->BRR = ((div_x16 / 16U) << 4) | (div_x16 % 16U);
    }
    USART3->CR2 = 0;
    USART3->CR1 = (1U << 13) | (1U << 5) | (1U << 3) | (1U << 2); /* UE | RXNEIE | TE | RE */
    NVIC_IPR[39] = (uint8_t)(6U << 4);  /* USART3_IRQn = 39; no RTOS calls in ISR */
    NVIC_ISER[1] |= (1U << (39 - 32));

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
        cfg.mqtt_tx_enabled = 1;
        strncpy(cfg.admin_username, "admin", sizeof(cfg.admin_username));
        strncpy(cfg.admin_password, "adminkontrx", sizeof(cfg.admin_password));
        cfg.actuator_mask = 0x0F;
        snprintf(cfg.sparkplug_edge_node_id, sizeof(cfg.sparkplug_edge_node_id), "KX-%07lu", (unsigned long)cfg.serial);
        snprintf(cfg.sparkplug_device_id, sizeof(cfg.sparkplug_device_id), "KX-%07lu-dev", (unsigned long)cfg.serial);
        Update_Shared_Config(&cfg);
    }

    /* Auto-generate Sparkplug IDs if empty (existing device upgraded to new firmware) */
    if (cfg.sparkplug_edge_node_id[0] == '\0') {
        snprintf(cfg.sparkplug_edge_node_id, sizeof(cfg.sparkplug_edge_node_id), "KX-%07lu", (unsigned long)cfg.serial);
        snprintf(cfg.sparkplug_device_id, sizeof(cfg.sparkplug_device_id), "KX-%07lu-dev", (unsigned long)cfg.serial);
        Update_Shared_Config(&cfg);
    }
}

/*
 * Modbus_Safe_Transaction — one blocking RS485 read (polling, no DMA).
 *
 * WHY THIS IS SLOW (> 1 ms per call, unavoidable):
 *   UART runs at 9600 baud (USART3 BRR computed from PCLK1), so one byte
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
 * timeout_ms: slave turnaround allowance. The wire time of the expected
 *   reply is added on top (see RS485_ReplyTimeout), so long frames such as
 *   the Multi-Sensor FC04 summary (25 bytes) are not cut short.
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
/* Set by the last Modbus_Safe_Transaction_T() when the slave answered with
 * a valid exception frame (it is present but rejected the request). */
static uint8_t s_last_exception = 0;

static uint8_t Modbus_Safe_Transaction_T(uint8_t slave, uint8_t fc,
                                          uint16_t reg, uint8_t num,
                                          uint16_t *out, uint32_t timeout_ms) {
    uint8_t f[8] = {slave, fc, reg>>8, reg&0xFF, 0, num, 0, 0};
    uint16_t crc = Modbus_CRC16(f, 6);
    f[6] = crc & 0xFF; f[7] = crc >> 8;

    /* 1. Drop any stale bytes so the reply starts from a clean buffer */
    RS485_RxFlush();

    /* 2. Switch MAX485 to TX mode and settle without yielding the CPU */
    MAX485_TX();
    for (volatile int d = 0; d < 2000; d++); /* microsecond settle delay */

    /* 3. Send Modbus query */
    USART3->SR &= ~(1U << 6); /* Clear TC flag before sending */
    for(int i=0; i<8; i++) {
        uint32_t tx_timeout = 100000;
        while(!(USART3->SR & (1U << 7)) && --tx_timeout);
        USART3->DR = f[i];
    }
    /* At 9600 baud, 1 byte takes ~1.04ms (175,000 cycles at 168MHz). 
     * tc_timeout must be at least 500,000 cycles so the last byte is fully sent! */
    uint32_t tc_timeout = 500000;
    while(!(USART3->SR & (1U << 6)) && --tc_timeout);

    for (volatile int d = 0; d < 200; d++); /* STOP bit settle delay */

    /* 4. Switch back to RX mode immediately */
    MAX485_RX();
    RS485_RxFlush(); /* discard any echo/glitch captured while DE was high */

    /* 5. Read response with noise rejection state machine. Bytes arrive via
     *    the RXNE interrupt, so sleeping between checks cannot overrun. */
    uint8_t rx[64]; int received = 0;
    const int expected = 3 + num * 2 + 2;
    uint8_t is_exception = 0;
    uint32_t window = RS485_ReplyTimeout(timeout_ms, (uint32_t)expected);
    uint32_t start = osKernelGetTickCount();

    while ((osKernelGetTickCount() - start) < window) {
        uint8_t b;
        if (!RS485_RxGet(&b)) {
            osDelay(1);
            continue;
        }

        /* Reject leading noise: first byte must be the target slave ID */
        if (received == 0 && b != slave) {
            continue;
        }

        /* Second byte must be the function code or its exception (fc|0x80) */
        if (received == 1 && b != fc) {
            if (b == (uint8_t)(fc | 0x80U)) {
                is_exception = 1;
            } else {
                received = 0;
                if (b == slave) {
                    rx[received++] = b;
                }
                continue;
            }
        }

        if (received < (int)sizeof(rx)) {
            rx[received++] = b;
        }

        /* Exception reply: slave + (fc|0x80) + code + CRC = 5 bytes */
        if (is_exception && received >= 5) {
            break;
        }
        if (received >= expected) {
            break;
        }
    }

    s_last_exception = 0;
    if (is_exception && received >= 5 &&
        Modbus_CRC16(rx, 3) == (rx[3] | (rx[4] << 8))) {
        s_last_exception = 1; /* device exists but rejected the request */
        return 0;
    }

    if(received < expected || rx[0] != slave || rx[1] != fc) return 0;
    if(Modbus_CRC16(rx, received-2) != (rx[received-2]|(rx[received-1]<<8))) return 0;
    
    for(int i=0; i<num; i++) out[i] = (rx[3+i*2]<<8) | rx[4+i*2];
    return 1;
}

/* FC06 — Write Single Register (e.g. Simulation Mode Switch at Address 165) */
static uint8_t Modbus_Write_Register_FC06(uint8_t slave, uint16_t reg, uint16_t val, uint32_t timeout_ms) {
    uint8_t f[8] = {slave, 0x06, (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), (uint8_t)(val >> 8), (uint8_t)(val & 0xFF), 0, 0};
    uint16_t crc = Modbus_CRC16(f, 6);
    f[6] = (uint8_t)(crc & 0xFF); f[7] = (uint8_t)(crc >> 8);

    RS485_RxFlush();

    MAX485_TX();
    for (volatile int d = 0; d < 2000; d++);

    USART3->SR &= ~(1U << 6);
    for (int i = 0; i < 8; i++) {
        uint32_t tx_timeout = 100000;
        while (!(USART3->SR & (1U << 7)) && --tx_timeout);
        USART3->DR = f[i];
    }
    uint32_t tc_timeout = 500000;
    while (!(USART3->SR & (1U << 6)) && --tc_timeout);

    for (volatile int d = 0; d < 200; d++);
    MAX485_RX();

    uint8_t rx[16]; int received = 0;
    uint32_t window = RS485_ReplyTimeout(timeout_ms, 8U);
    uint32_t start = osKernelGetTickCount();
    while ((osKernelGetTickCount() - start) < window) {
        uint8_t b;
        if (!RS485_RxGet(&b)) { osDelay(1); continue; }
        if (received == 0 && b != slave) continue;
        if (received == 1 && b != 0x06) { received = 0; if (b == slave) rx[received++] = b; continue; }
        if (received < 16) rx[received++] = b;
        if (received >= 8) break;
    }
    if (received < 8 || rx[0] != slave || rx[1] != 0x06) return 0;
    if (Modbus_CRC16(rx, 6) != (rx[6] | (rx[7] << 8))) return 0;
    return 1;
}

uint8_t Modbus_SetMultiSensorMode(uint8_t slave_id, uint8_t mode) {
    uint8_t ok = Modbus_Write_Register_FC06(slave_id, 165, mode ? 1 : 0, 50);
    if (ok) {
        Modbus_SensorData_t local;
        Get_Shared_Sensor_Data(&local);
        for (uint8_t i = 0; i < local.multi_us_count && i < MAX_MULTI_US; i++) {
            if (local.multi_us[i].id == slave_id) {
                local.multi_us[i].mode = mode ? 1 : 0;
                break;
            }
        }
        Update_Shared_Sensor_Data(&local);
        printf("[MODBUS] Multi-Sensor Board (ID %u) mode set to %s\r\n", slave_id, mode ? "SIMULATION" : "NORMAL");
    }
    return ok;
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
    static uint32_t fail_until_s[MAX_SENSORS] = {0};
    static uint8_t  fail_cnt[MAX_SENSORS]     = {0};
    extern volatile uint32_t g_uptime_seconds;

    for (uint8_t s = 0; s < cfg.sensors.count && s < MAX_SENSORS; s++) {
        /* Smart Backoff: if sensor is offline/unresponsive, skip polling for 4 seconds (exempt type 7) */
        uint8_t sensor_type_peek = cfg.sensors.entries[s].type;
        if (sensor_type_peek != 7 && fail_cnt[s] >= 2 && g_uptime_seconds < fail_until_s[s]) {
            local.readings[s].valid = 0;
            continue;
        }

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

            case 7: /* Multi-Sensor Board (3 Ultrasonic, 3 Flow, 2 Temp) */
                if (mu_idx < MAX_MULTI_US) {
                    MultiSensorBoard_t *mu = &local.multi_us[mu_idx];
                    
                    /* Restore previous board metadata */
                    uint8_t found_meta = 0;
                    for (uint8_t k = 0; k < local.multi_us_count; k++) {
                        if (local.multi_us[k].id == sid) {
                            *mu = local.multi_us[k];
                            found_meta = 1;
                            break;
                        }
                    }
                    if (!found_meta || mu->id != sid) {
                        mu->id = sid;
                        for (uint8_t c = 0; c < 3; c++) mu->dist[c] = -1000.0f;
                        for (uint8_t c = 0; c < 3; c++) {
                            mu->flow[c].rate = 0.0f;
                            mu->flow[c].total_l = 0;
                            mu->flow[c].freq_hz = 0.0f;
                            mu->flow[c].status = 0;
                        }
                        mu->temp[0] = -1000.0f;
                        mu->temp[1] = -1000.0f;
                        mu->mode = 0;
                        mu->avg = -1000.0f;
                        mu->comp = -1000.0f;
                        mu->legacy_temp = -1000.0f;
                        for (uint8_t k = 0; k < 10; k++) mu->summary[k] = 0;
                        mu->summary_ok = 0;
                    }

                    uint8_t target_sid = sid;
                    uint8_t summary_now = 0;

                    /* 1. FC04 Summary (Start 0, Quantity 10). Slave input-register map:
                     *    0 dist avg (mm), 1 temp avg (x10 degC), 2 dist temp-compensated (mm),
                     *    3 total flow rate (x10 L/min), 4-5 combined volume H/L (L),
                     *    6 total pulse freq (Hz), 7 health bitmask, 8 uptime (s), 9 error code.
                     *    Only aggregate fields come from here; per-channel values are
                     *    read from the FC03 blocks below. */
                    if (Modbus_Safe_Transaction_T(target_sid, 0x04, 0x0000, 10, rs, 60)) {
                        for (uint8_t k = 0; k < 10; k++) mu->summary[k] = rs[k];
                        mu->summary_ok = 1;
                        summary_now = 1;
                        ok = 1;
                    }

                    /* 2. Poll FC03 channel blocks for detailed registers (Offsets 0x00..0x70) */
                    for (uint8_t c = 0; c < 2; c++) {
                        uint8_t ch = (multi_us_channel_offset + c) % 8;
                        uint16_t reg_addr = ch * 0x10;

                        if (ch <= 2) {
                            /* Ultrasonic 1..3: distance (mm), signal quality, status (1 = valid) */
                            if (Modbus_Safe_Transaction_T(target_sid, 0x03, reg_addr, 3, rs, 60)) {
                                mu->dist[ch] = (rs[2] == 1U) ? (float)rs[0] : -1000.0f;
                                ok = 1;
                            }
                        } else if (ch >= 3 && ch <= 5) {
                            /* Water Flow 1..3: 5 registers */
                            if (Modbus_Safe_Transaction_T(target_sid, 0x03, reg_addr, 5, rs, 60)) {
                                uint8_t f = ch - 3;
                                mu->flow[f].rate    = (float)rs[0] / 10.0f;
                                mu->flow[f].total_l = ((uint32_t)rs[1] << 16) | (uint32_t)rs[2];
                                mu->flow[f].freq_hz = (float)rs[3];
                                mu->flow[f].status  = (uint8_t)rs[4];
                                ok = 1;
                            }
                        } else if (ch == 6 || ch == 7) {
                            /* Temp 1..2: value (x10 degC), quality, status (1 = valid) */
                            if (Modbus_Safe_Transaction_T(target_sid, 0x03, reg_addr, 3, rs, 60)) {
                                uint8_t t = ch - 6;
                                mu->temp[t] = (rs[2] == 1U) ? ((int16_t)rs[0]) / 10.0f : -1000.0f;
                                ok = 1;
                            }
                        }
                        osDelay(10);
                    }

                    if (summary_now) {
                        /* Aggregates computed by the slave over all of its channels */
                        mu->avg  = (mu->summary[0] > 0U) ? (float)mu->summary[0] : -1000.0f;
                        mu->comp = (mu->summary[2] > 0U) ? (float)mu->summary[2] : -1000.0f;
                    } else {
                        /* FC04 missed this cycle: average the valid US channels locally */
                        uint32_t sum = 0; uint8_t valid_us = 0;
                        for (uint8_t i = 0; i < 3; i++) {
                            if (mu->dist[i] > 0.0f && mu->dist[i] < 10000.0f) {
                                sum += (uint32_t)mu->dist[i];
                                valid_us++;
                            }
                        }
                        mu->avg = (valid_us > 0) ? (float)(sum / valid_us) : -1000.0f;
                    }

                    if (mu_idx >= local.multi_us_count) local.multi_us_count = mu_idx + 1;
                    mu_idx++;

                    rd->value = (mu->avg > 0.0f) ? mu->avg : mu->flow[0].rate;
                    if (summary_now) {
                        rd->temp = ((int16_t)mu->summary[1]) / 10.0f;
                    } else {
                        rd->temp = (mu->temp[0] > -50.0f) ? mu->temp[0] : ((mu->temp[1] > -50.0f) ? mu->temp[1] : 0.0f);
                    }
                    mu->legacy_temp = rd->temp;

                    /* Periodically log diagnostic event */
                    static uint32_t last_dbg_s = 0;
                    if (g_uptime_seconds - last_dbg_s >= 5) {
                        last_dbg_s = g_uptime_seconds;
                        char dbg[64];
                        snprintf(dbg, sizeof(dbg), "MultiSensor: target_sid=%u ok=%u (U1=%.0f, F1=%.1f, T1=%.1f)",
                                 target_sid, ok, mu->dist[0], mu->flow[0].rate, mu->temp[0]);
                        extern void Log_Event(const char *category, const char *message);
                        Log_Event("MODBUS", dbg);
                        snprintf(dbg, sizeof(dbg), "RS485 rx=%lu fe=%lu ore=%lu last=%02X %02X %02X %02X",
                                 (unsigned long)s_rx_bytes, (unsigned long)s_rx_fe, (unsigned long)s_rx_ore,
                                 s_rx_last[0], s_rx_last[1], s_rx_last[2], s_rx_last[3]);
                        Log_Event("MODBUS", dbg);
                    }
                }
                break;

            default:
                break;
        }

        uint8_t old_valid = rd->valid;
        if (ok) {
            fail_cnt[s] = 0;
            fail_until_s[s] = 0;
            extern volatile uint32_t g_uptime_seconds;
            rd->value      = DSP_Filter_ProcessSample(rd->type, rd->id, rd->value, g_uptime_seconds, NULL, NULL);
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
                else if (rd->type == 7) tname = "Multi-Sensor";
                
                 char log_buf[64];
                snprintf(log_buf, sizeof(log_buf), "Sensor %s (ID %u) connected.", tname, rd->id);
                printf("[MODBUS] Sensor Connection: %s (ID %u) is now ONLINE\r\n", tname, rd->id);
                Log_Event("MODBUS", log_buf);
            }
        } else {
            fail_cnt[s]++;
            if (fail_cnt[s] >= 2) {
                fail_until_s[s] = g_uptime_seconds + 8;
            }
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
                else if (rd->type == 7) tname = "Multi-Sensor";
                
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

/* Copies sharedSensorData into a batch. Caller holds sensorMutex (or the
 * kernel is not running yet). */
static void Fill_Batch(TelemetryBatch_t *dest) {
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

    /* Multi-sensor boards: per-channel values, valid with the board's reading */
    dest->multi_count = 0;
    for (uint8_t i = 0; i < sharedSensorData.multi_us_count && i < MAX_MULTI_US; i++) {
        const MultiSensorBoard_t *mu = &sharedSensorData.multi_us[i];
        MultiChRecord_t *mc = &dest->multi[dest->multi_count++];
        mc->id = mu->id;
        mc->valid = 0;
        for (uint8_t r = 0; r < dest->count; r++) {
            if (dest->records[r].type == SENSOR_TYPE_MULTI_US && dest->records[r].id == mu->id) {
                mc->valid = dest->records[r].valid;
                break;
            }
        }
        for (uint8_t c = 0; c < 3; c++) mc->ch[MULTI_CH_US + c]   = mu->dist[c];
        for (uint8_t c = 0; c < 3; c++) mc->ch[MULTI_CH_FLOW + c] = mu->flow[c].rate;
        for (uint8_t c = 0; c < 2; c++) mc->ch[MULTI_CH_TEMP + c] = mu->temp[c];
    }
}

void Modbus_DMA_ConsumeBatch(TelemetryBatch_t *dest) {
    if (!dest) return;

    if (osKernelGetState() != osKernelRunning || sensorMutex == NULL) {
        Fill_Batch(dest);
        return;
    }

    if (osMutexAcquire(sensorMutex, 50) == osOK) {
        Fill_Batch(dest);
        osMutexRelease(sensorMutex);
    } else {
        dest->count = 0;
        dest->multi_count = 0;
    }
}

/* Scan probes use a generous turnaround: unknown slaves (e.g. a Blue Pill
 * board) may take tens of ms to start replying. */
#define SCAN_TURNAROUND_MS 100U

void Modbus_DMA_PerformScan(void) {
    g_scan_status.is_scanning = 1;
    g_scan_status.count = 0;
    uint16_t d[12];
    for(int i=1; i<=247; i++) {
        if(!g_scan_status.is_scanning) break;
        uint8_t type = 0;

        /* 1. Presence probe: any valid reply, including an exception frame,
         *    proves a device lives at this address. */
        uint8_t present = Modbus_Safe_Transaction_T(i, 0x03, 0x0000, 1, d, SCAN_TURNAROUND_MS);
        if (!present && s_last_exception) present = 1;

        if (present) {
            osDelay(10);
            /* 2. Multi-Sensor Board: FC04 summary (0, qty 10) AND the
             *    DS18B20 block at 0x60, which plain sensors do not expose. */
            if (Modbus_Safe_Transaction_T(i, 0x04, 0x0000, 10, d, SCAN_TURNAROUND_MS) &&
                Modbus_Safe_Transaction_T(i, 0x03, 0x0060, 2, d, SCAN_TURNAROUND_MS)) {
                type = 7;
            /* 3. DO sensor (Kacise KWS-630) */
            } else if (Modbus_Safe_Transaction_T(i, 0x03, 0x2600, 6, d, SCAN_TURNAROUND_MS)) {
                type = 4;
            /* 4. Single ultrasonic: 10-register block at 0 */
            } else if (Modbus_Safe_Transaction_T(i, 0x03, 0x0000, 10, d, SCAN_TURNAROUND_MS)) {
                type = 6;
            /* 5. Generic 2-register water-quality sensor (pH/ORP/EC/Ammonia) */
            } else if (Modbus_Safe_Transaction_T(i, 0x03, 0x0000, 2, d, SCAN_TURNAROUND_MS)) {
                type = 1;
            } else {
                type = 255; /* present but unrecognised */
            }
        }

        if(type && g_scan_status.count < SCAN_MAX_DEVICES) {
            g_scan_status.devices[g_scan_status.count].id = i;
            g_scan_status.devices[g_scan_status.count].type = type;
            g_scan_status.count++;
            printf("[SCAN] Found device ID %d type %u\r\n", i, type);
        }
        g_scan_status.progress = (i * 100) / 247;
        osDelay(10); /* inter-frame silence before probing the next address */
    }
    g_scan_status.progress = 100;
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
    } else {
        memcpy(dest, &sharedConfig, sizeof(Gateway_Config_t));
    }
}

void Update_Shared_Config(const Gateway_Config_t *src) {
    if (osKernelGetState() != osKernelRunning || configMutex == NULL) {
        memcpy(&sharedConfig, src, sizeof(Gateway_Config_t));
        g_config_changed = 1;
        g_config_version++;
        return;
    }

    if (osMutexAcquire(configMutex, 100) == osOK) {
        memcpy(&sharedConfig, src, sizeof(Gateway_Config_t));
        g_config_changed = 1;
        g_config_version++;
        osMutexRelease(configMutex);
    }
}   

void DMA1_Stream3_IRQHandler(void) { }