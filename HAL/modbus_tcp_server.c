/**
 * @file    modbus_tcp_server.c
 * @brief   Kontrx High-Performance Modbus TCP Server (W5500 Socket 2, Port 502)
 */

#include "modbus_tcp_server.h"
#include "socket.h"
#include "w5500.h"
#include "pwm_controller.h"
#include "pto_motion.h"
#include "dac_420ma.h"
#include "analog_010v.h"
#include "freertos_tasks.h"
#include <string.h>

#define SN                  MODBUS_TCP_SERVER_SOCKET
#define RX_BUF_SIZE         256U

static uint8_t s_rx_buf[RX_BUF_SIZE];
static uint8_t s_tx_buf[RX_BUF_SIZE];

static uint16_t ReadHoldingRegister(uint16_t addr) {
    // 0-15: Relays
    if (addr < 16) {
        return relayStates[addr];
    }
    // 16-22: PWM Duty (0-1000 = 0.0-100.0%)
    if (addr >= 16 && addr <= 22) {
        uint8_t ch = addr - 16;
        const PWM_Channel_Info_t *info = PWM_GetChannelInfo(ch);
        if (info) return (uint16_t)(info->duty_pct * 10.0f + 0.5f);
        return 0;
    }
    // 24-25: 0-10V Outputs (0-1000 = 0.00-10.00 V)
    if (addr == 24 || addr == 25) {
        uint8_t ch = addr - 24;
        return (uint16_t)(Analog_010V_GetVoltage(ch) * 100.0f + 0.5f);
    }
    // 28-35: 4-20mA Outputs (400-2000 = 4.00-20.00 mA)
    if (addr >= 28 && addr <= 35) {
        uint8_t ch = addr - 28;
        return (uint16_t)(DAC_420MA_GetCurrent(ch) * 100.0f + 0.5f);
    }
    // 36-39: PTO Target Positions (signed 16-bit)
    if (addr >= 36 && addr <= 39) {
        uint8_t ch = addr - 36;
        const PTO_Channel_Status_t *st = PTO_GetStatus(ch);
        if (st) return (uint16_t)(st->target & 0xFFFF);
        return 0;
    }
    // 40-43: PTO Current Positions (signed 16-bit)
    if (addr >= 40 && addr <= 43) {
        uint8_t ch = addr - 40;
        const PTO_Channel_Status_t *st = PTO_GetStatus(ch);
        if (st) return (uint16_t)(st->position & 0xFFFF);
        return 0;
    }
    // 44-47: PTO Status (bit 0: moving, bit 1: limit hit)
    if (addr >= 44 && addr <= 47) {
        uint8_t ch = addr - 44;
        const PTO_Channel_Status_t *st = PTO_GetStatus(ch);
        if (st) return (st->moving ? 1U : 0U) | (st->lmt_state ? 2U : 0U);
        return 0;
    }
    // 100-115: Live Sensors
    if (addr >= 100 && addr < 100 + MAX_SENSORS) {
        uint8_t s_idx = addr - 100;
        if (s_idx < sharedSensorData.readings_count) {
            return (uint16_t)(sharedSensorData.readings[s_idx].value * 100.0f);
        }
        return 0;
    }
    return 0;
}

static void WriteHoldingRegister(uint16_t addr, uint16_t val) {
    if (addr < 16) {
        Relay_SetState(addr, val ? 1 : 0);
    } else if (addr >= 16 && addr <= 22) {
        uint8_t ch = addr - 16;
        float duty = (float)val / 10.0f;
        PWM_SetDuty(ch, duty);
    } else if (addr == 24 || addr == 25) {
        uint8_t ch = addr - 24;
        float volt = (float)val / 100.0f;
        Analog_010V_SetVoltage(ch, volt);
    } else if (addr >= 28 && addr <= 35) {
        uint8_t ch = addr - 28;
        float mA = (float)val / 100.0f;
        DAC_420MA_SetCurrent(ch, mA);
    } else if (addr >= 36 && addr <= 39) {
        uint8_t ch = addr - 36;
        int16_t target = (int16_t)val;
        PTO_MoveAbsolute(ch, target, 1000U);
    }
}

void Modbus_TCP_Server_Init(void) {
    close(SN);
    socket(SN, Sn_MR_TCP, MODBUS_TCP_SERVER_PORT, 0);
    listen(SN);
}

void Modbus_TCP_Server_Poll(void) {
    uint8_t sr = getSn_SR(SN);

    if (sr == SOCK_CLOSED) {
        socket(SN, Sn_MR_TCP, MODBUS_TCP_SERVER_PORT, 0);
        listen(SN);
        return;
    }

    if (sr == SOCK_CLOSE_WAIT) {
        disconnect(SN);
        close(SN);
        return;
    }

    if (sr == SOCK_ESTABLISHED) {
        uint16_t rx_size = getSn_RX_RSR(SN);
        if (rx_size < 12) return; // Minimum Modbus TCP ADU length

        if (rx_size > sizeof(s_rx_buf)) rx_size = sizeof(s_rx_buf);
        int32_t len = recv(SN, s_rx_buf, rx_size);
        if (len < 12) return;

        uint16_t trans_id = (s_rx_buf[0] << 8) | s_rx_buf[1];
        uint16_t proto_id = (s_rx_buf[2] << 8) | s_rx_buf[3];
        if (proto_id != 0) return; // Must be 0 for Modbus TCP

        uint8_t unit_id = s_rx_buf[6];
        uint8_t fc = s_rx_buf[7];

        if (fc == 0x03) { // Read Holding Registers
            uint16_t start_reg = (s_rx_buf[8] << 8) | s_rx_buf[9];
            uint16_t reg_count = (s_rx_buf[10] << 8) | s_rx_buf[11];
            if (reg_count > 60) reg_count = 60; // Max per frame

            uint16_t byte_count = reg_count * 2U;
            uint16_t resp_len = 9 + byte_count;

            s_tx_buf[0] = (uint8_t)(trans_id >> 8);
            s_tx_buf[1] = (uint8_t)(trans_id & 0xFF);
            s_tx_buf[2] = 0;
            s_tx_buf[3] = 0;
            s_tx_buf[4] = (uint8_t)((3 + byte_count) >> 8);
            s_tx_buf[5] = (uint8_t)((3 + byte_count) & 0xFF);
            s_tx_buf[6] = unit_id;
            s_tx_buf[7] = fc;
            s_tx_buf[8] = (uint8_t)byte_count;

            for (uint16_t i = 0; i < reg_count; i++) {
                uint16_t val = ReadHoldingRegister(start_reg + i);
                s_tx_buf[9 + i * 2]     = (uint8_t)(val >> 8);
                s_tx_buf[9 + i * 2 + 1] = (uint8_t)(val & 0xFF);
            }

            send(SN, s_tx_buf, resp_len);
        } else if (fc == 0x06) { // Write Single Register
            uint16_t reg_addr = (s_rx_buf[8] << 8) | s_rx_buf[9];
            uint16_t reg_val  = (s_rx_buf[10] << 8) | s_rx_buf[11];
            WriteHoldingRegister(reg_addr, reg_val);

            // Echo response (12 bytes)
            send(SN, s_rx_buf, 12);
        } else if (fc == 0x05) { // Write Single Coil
            uint16_t coil_addr = (s_rx_buf[8] << 8) | s_rx_buf[9];
            uint16_t coil_val  = (s_rx_buf[10] << 8) | s_rx_buf[11];
            if (coil_addr < 16) {
                Relay_SetState(coil_addr, (coil_val == 0xFF00) ? 1 : 0);
            }
            send(SN, s_rx_buf, 12);
        }
    }
}