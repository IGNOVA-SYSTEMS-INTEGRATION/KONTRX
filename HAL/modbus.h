#ifndef MODBUS_H
#define MODBUS_H

#include <stdint.h>

// Dependency Injection Structure for Hardware Abstraction
typedef struct {
    void (*uart_send_byte)(uint8_t data);
    void (*wait_tx_complete)(void);
    uint8_t (*uart_receive_byte)(void);
    uint8_t (*uart_receive_byte_timeout)(uint8_t* data, uint32_t timeout);
    void (*enable_tx_mode)(void);
    void (*enable_rx_mode)(void);
} Modbus_Interface_t;

void Modbus_Init(Modbus_Interface_t* interface);
void Modbus_Send_ReadHoldingRegisters(uint8_t slave_addr, uint16_t start_reg, uint16_t num_regs);
uint8_t Modbus_ReadHoldingRegisters(uint8_t slave_addr, uint16_t start_reg, uint16_t num_regs, uint16_t* out_buffer);
uint8_t Modbus_ScanSensor(uint8_t slave_addr, uint16_t reg_addr);
uint8_t Modbus_WriteSingleRegister(uint8_t slave_addr, uint16_t reg_addr, uint16_t reg_value);
uint8_t Modbus_WriteMultipleRegisters(uint8_t slave_addr, uint16_t start_reg, uint16_t num_regs, uint16_t* values);

#endif // MODBUS_H
