#ifndef GPIO_STM32_H
#define GPIO_STM32_H

#include <stdint.h>
#include "stm32f407_regs.h"

// GPIO Ports Lookup Table (GPIOA=0, GPIOB=1, GPIOC=2, GPIOD=3, GPIOE=4)
extern GPIO_TypeDef * const GPIO_Ports[5];

// Generic GPIO Functions
void GPIO_InitOutput(GPIO_TypeDef* port, uint8_t pin);
void GPIO_TogglePin(GPIO_TypeDef* port, uint8_t pin);

// Specific hardware initializations for this project
void GPIO_Init_USART1_Pins(void);
void GPIO_Init_USART3_Pins(void);
void GPIO_Init_MAX485_Pins(void);
void GPIO_Init_PE4_Button(void);
uint8_t GPIO_Read_PE4(void);

// MAX485 Control
void MAX485_Transmit_Enable(void);
void MAX485_Receive_Enable(void);

// W5500 Pins
void GPIO_Init_W5500_Pins(void);
void W5500_CS_Select(void);
void W5500_CS_Deselect(void);
void W5500_Hardware_Reset(void);

#endif // GPIO_STM32_H
