#include "modbus.h"
#include <stddef.h>
#include <stdio.h>

static Modbus_Interface_t* modbus_if = NULL;

// Standard Modbus CRC16 Calculation
static uint16_t Modbus_CRC16(uint8_t *buf, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t pos = 0; pos < len; pos++) {
        crc ^= (uint16_t)buf[pos];
        for (uint8_t i = 8; i != 0; i--) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

void Modbus_Init(Modbus_Interface_t* interface) {
    modbus_if = interface;
    // Default to receive mode to listen to the bus
    if (modbus_if && modbus_if->enable_rx_mode) {
        modbus_if->enable_rx_mode();
    }
}

void Modbus_Send_ReadHoldingRegisters(uint8_t slave_addr, uint16_t start_reg, uint16_t num_regs) {
    if (!modbus_if || !modbus_if->uart_send_byte || !modbus_if->wait_tx_complete || !modbus_if->enable_tx_mode || !modbus_if->enable_rx_mode) {
        return; // Interface not properly configured
    }

    uint8_t frame[8];
    frame[0] = slave_addr;
    frame[1] = 0x03; // Function Code: Read Holding Registers
    frame[2] = (uint8_t)(start_reg >> 8);
    frame[3] = (uint8_t)(start_reg & 0xFF);
    frame[4] = (uint8_t)(num_regs >> 8);
    frame[5] = (uint8_t)(num_regs & 0xFF);

    uint16_t crc = Modbus_CRC16(frame, 6);
    frame[6] = (uint8_t)(crc & 0xFF); // CRC LSB
    frame[7] = (uint8_t)(crc >> 8);   // CRC MSB

    // Modbus RTU requires at least 3.5 character times of silence before a frame begins.
    for (volatile uint32_t delay = 0; delay < 4000; delay++);

    // 1. Enable Transmit Mode on MAX485
    modbus_if->enable_tx_mode();

    // Give MAX485 a microsecond to turn on its driver
    for (volatile uint32_t delay = 0; delay < 10; delay++);

    // 2. Send the frame bytes back-to-back WITHOUT DELAY
    for (uint8_t i = 0; i < 8; i++) {
        modbus_if->uart_send_byte(frame[i]);
    }

    // 3. Wait for all bits to physically leave the UART
    modbus_if->wait_tx_complete();

    // Give MAX485 a tiny moment to finish pushing the STOP bit before dropping DE.
    for (volatile uint32_t delay = 0; delay < 5; delay++);

    // 4. Re-enable Receive Mode on MAX485
    modbus_if->enable_rx_mode();
}

uint8_t Modbus_ReadHoldingRegisters(uint8_t slave_addr, uint16_t start_reg, uint16_t num_regs, uint16_t* out_buffer) {
    if (!modbus_if || !modbus_if->uart_receive_byte_timeout) return 0; // Fail

    // Clear buffer in case of failure
    for (uint16_t i = 0; i < num_regs; i++) {
        out_buffer[i] = 0;
    }

    // 1. Send the request
    Modbus_Send_ReadHoldingRegisters(slave_addr, start_reg, num_regs);

    // Expected Response Length: Slave(1) + Func(1) + ByteCount(1) + Data(num_regs*2) + CRC(2)
    uint8_t expected_len = 3 + (num_regs * 2) + 2;
    uint8_t rx_buffer[256];
    uint8_t rx_idx = 0;

    // 2. Wait for bytes with timeout
    while (rx_idx < expected_len) {
        uint32_t current_timeout = (rx_idx == 0) ? 500000 : 50000;
        if (!modbus_if->uart_receive_byte_timeout(&rx_buffer[rx_idx], current_timeout)) {
            break; // Timeout
        }
        rx_idx++;
    }

    if (rx_idx == 0) return 0; // Timeout
    if (rx_idx < expected_len) return 0; // Incomplete Frame

    // 3. Verify Address and Function
    if (rx_buffer[0] != slave_addr || rx_buffer[1] != 0x03) {
        return 0; // Wrong response
    }

    // 4. Verify CRC
    uint16_t received_crc = rx_buffer[rx_idx - 2] | (rx_buffer[rx_idx - 1] << 8);
    uint16_t calculated_crc = Modbus_CRC16(rx_buffer, rx_idx - 2);
    if (received_crc != calculated_crc) {
        return 0; // CRC Error
    }

    // 5. Extract Data
    uint8_t data_idx = 3; // Data starts at index 3
    for (uint16_t i = 0; i < num_regs; i++) {
        out_buffer[i] = (rx_buffer[data_idx] << 8) | rx_buffer[data_idx + 1];
        data_idx += 2;
    }

    return 1; // Success
}

// Very fast scan function that doesn't block for a long time
// Very fast scan function that doesn't block for a long time
uint8_t Modbus_ScanSensor(uint8_t slave_addr, uint16_t reg_addr) {
    if (!modbus_if || !modbus_if->uart_receive_byte_timeout) return 0;

    uint16_t num_regs = (reg_addr == 0x2600) ? 6 : 1;
    Modbus_Send_ReadHoldingRegisters(slave_addr, reg_addr, num_regs);

    uint8_t expected_len = 3 + (num_regs * 2) + 2;
    uint8_t rx_buffer[32];
    uint8_t rx_idx = 0;

    while (rx_idx < expected_len) {
        uint32_t current_timeout = (rx_idx == 0) ? 500000 : 50000;
        if (!modbus_if->uart_receive_byte_timeout(&rx_buffer[rx_idx], current_timeout)) {
            break;
        }
        rx_idx++;
    }

    if (rx_idx < expected_len) return 0;
    if (rx_buffer[0] != slave_addr || rx_buffer[1] != 0x03) return 0;

    uint16_t received_crc = rx_buffer[rx_idx - 2] | (rx_buffer[rx_idx - 1] << 8);
    uint16_t calculated_crc = Modbus_CRC16(rx_buffer, rx_idx - 2);
    if (received_crc != calculated_crc) return 0;

    return 1; // Success
}

uint8_t Modbus_WriteSingleRegister(uint8_t slave_addr, uint16_t reg_addr, uint16_t reg_value) {
    if (!modbus_if || !modbus_if->uart_send_byte || !modbus_if->wait_tx_complete || !modbus_if->enable_tx_mode || !modbus_if->enable_rx_mode || !modbus_if->uart_receive_byte_timeout) {
        return 0; // Interface not properly configured
    }

    uint8_t frame[8];
    frame[0] = slave_addr;
    frame[1] = 0x06; // Function Code: Write Single Register
    frame[2] = (uint8_t)(reg_addr >> 8);
    frame[3] = (uint8_t)(reg_addr & 0xFF);
    frame[4] = (uint8_t)(reg_value >> 8);
    frame[5] = (uint8_t)(reg_value & 0xFF);

    uint16_t crc = Modbus_CRC16(frame, 6);
    frame[6] = (uint8_t)(crc & 0xFF); // CRC LSB
    frame[7] = (uint8_t)(crc >> 8);   // CRC MSB

    for (volatile uint32_t delay = 0; delay < 4000; delay++);

    modbus_if->enable_tx_mode();
    for (volatile uint32_t delay = 0; delay < 10; delay++);

    for (uint8_t i = 0; i < 8; i++) {
        modbus_if->uart_send_byte(frame[i]);
    }

    modbus_if->wait_tx_complete();
    for (volatile uint32_t delay = 0; delay < 5; delay++);

    modbus_if->enable_rx_mode();

    if (slave_addr == 0x00) {
        // Broadcast messages do not get a reply from slaves
        return 1;
    }

    uint8_t expected_len = 8;
    uint8_t rx_buffer[16];
    uint8_t rx_idx = 0;

    while (rx_idx < expected_len) {
        uint32_t current_timeout = (rx_idx == 0) ? 500000 : 50000;
        if (!modbus_if->uart_receive_byte_timeout(&rx_buffer[rx_idx], current_timeout)) {
            break; // Timeout
        }
        rx_idx++;
    }

    if (rx_idx < expected_len) return 0;
    if (rx_buffer[0] != slave_addr || rx_buffer[1] != 0x06) return 0;

    uint16_t received_crc = rx_buffer[rx_idx - 2] | (rx_buffer[rx_idx - 1] << 8);
    uint16_t calculated_crc = Modbus_CRC16(rx_buffer, rx_idx - 2);
    if (received_crc != calculated_crc) return 0;

    return 1; // Success
}

uint8_t Modbus_WriteMultipleRegisters(uint8_t slave_addr, uint16_t start_reg, uint16_t num_regs, uint16_t* values) {
    if (!modbus_if || !modbus_if->uart_send_byte || !modbus_if->wait_tx_complete || !modbus_if->enable_tx_mode || !modbus_if->enable_rx_mode || !modbus_if->uart_receive_byte_timeout) {
        return 0; // Interface not properly configured
    }

    uint8_t frame[256];
    frame[0] = slave_addr;
    frame[1] = 0x10; // Function Code 16 (0x10): Write Multiple Registers
    frame[2] = (uint8_t)(start_reg >> 8);
    frame[3] = (uint8_t)(start_reg & 0xFF);
    frame[4] = (uint8_t)(num_regs >> 8);
    frame[5] = (uint8_t)(num_regs & 0xFF);
    
    uint8_t byte_count = (uint8_t)(num_regs * 2);
    frame[6] = byte_count;
    
    uint8_t idx = 7;
    for (uint16_t i = 0; i < num_regs; i++) {
        frame[idx++] = (uint8_t)(values[i] >> 8);
        frame[idx++] = (uint8_t)(values[i] & 0xFF);
    }

    uint16_t crc = Modbus_CRC16(frame, idx);
    frame[idx++] = (uint8_t)(crc & 0xFF); // CRC LSB
    frame[idx++] = (uint8_t)(crc >> 8);   // CRC MSB

    for (volatile uint32_t delay = 0; delay < 4000; delay++);

    modbus_if->enable_tx_mode();
    for (volatile uint32_t delay = 0; delay < 10; delay++);

    for (uint8_t i = 0; i < idx; i++) {
        modbus_if->uart_send_byte(frame[i]);
    }

    modbus_if->wait_tx_complete();
    for (volatile uint32_t delay = 0; delay < 5; delay++);

    modbus_if->enable_rx_mode();

    if (slave_addr == 0x00) return 1;

    uint8_t expected_len = 8;
    uint8_t rx_buffer[16];
    uint8_t rx_idx = 0;

    while (rx_idx < expected_len) {
        uint32_t current_timeout = (rx_idx == 0) ? 500000 : 50000;
        if (!modbus_if->uart_receive_byte_timeout(&rx_buffer[rx_idx], current_timeout)) {
            break; // Timeout
        }
        rx_idx++;
    }

    if (rx_idx < expected_len) return 0;
    if (rx_buffer[0] != slave_addr || rx_buffer[1] != 0x10) return 0;

    uint16_t received_crc = rx_buffer[rx_idx - 2] | (rx_buffer[rx_idx - 1] << 8);
    uint16_t calculated_crc = Modbus_CRC16(rx_buffer, rx_idx - 2);
    if (received_crc != calculated_crc) return 0;

    return 1; // Success
}
