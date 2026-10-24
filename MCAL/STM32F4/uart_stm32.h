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

/* Non-blocking debug output: queues bytes for the USART6 TXE interrupt.
 * Returns the number queued; bytes that do not fit are dropped and counted
 * in g_dbg_tx_dropped. UART_Debug_WriteBlocking() drains the queue and then
 * sends by polling (boot, fault and reset paths, where no IRQ will run). */
uint32_t UART_Debug_WriteAsync(const uint8_t *data, uint32_t len);
void     UART_Debug_WriteBlocking(const uint8_t *data, uint32_t len);
extern volatile uint32_t g_dbg_tx_dropped;

// Expose these for the Modbus interface
void UART_Modbus_SendByte(uint8_t data);
void UART_Modbus_WaitTransmissionComplete(void);
uint8_t UART_Modbus_ReceiveByte(void);
uint8_t UART_Modbus_ReceiveByte_Timeout(uint8_t* data, uint32_t timeout);

#endif // UART_STM32_H
