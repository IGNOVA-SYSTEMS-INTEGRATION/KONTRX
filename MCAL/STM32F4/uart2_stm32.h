#ifndef UART2_STM32_H
#define UART2_STM32_H

#include <stdint.h>

/**
 * @brief Initialize USART2 for RS485 Port 2 (PD5 TX, PD6 RX, PD4 DE, PD7 RE#)
 */
void UART2_Init(uint32_t baud);
void UART2_SendByte(uint8_t data);
uint8_t UART2_ReceiveByte(void);
uint8_t UART2_ReceiveByte_Timeout(uint8_t* data, uint32_t timeout);

#endif // UART2_STM32_H