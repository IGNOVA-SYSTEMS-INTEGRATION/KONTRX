#ifndef UART_STM32_H
#define UART_STM32_H

#include <stdint.h>

void UART_Init(void);
void UART_SendByte(uint8_t data);
void UART_WaitTransmissionComplete(void);
uint8_t UART_ReceiveByte(void);

// USART1 Debug
void UART_Debug_Init(void);
void UART_Debug_SendByte(uint8_t data);
uint8_t UART_Debug_ReceiveByte(void);

// Expose these for the Modbus interface
void UART_Modbus_SendByte(uint8_t data);
void UART_Modbus_WaitTransmissionComplete(void);
uint8_t UART_Modbus_ReceiveByte(void);
uint8_t UART_Modbus_ReceiveByte_Timeout(uint8_t* data, uint32_t timeout);

#endif // UART_STM32_H
