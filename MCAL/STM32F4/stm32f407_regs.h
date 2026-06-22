#ifndef STM32F407_REGS_H
#define STM32F407_REGS_H

#include <stdint.h>

#define PERIPH_BASE       0x40000000U
#define APB1PERIPH_BASE   (PERIPH_BASE + 0x00000000U)
#define APB2PERIPH_BASE   (PERIPH_BASE + 0x00010000U)
#define AHB1PERIPH_BASE   (PERIPH_BASE + 0x00020000U)

#define RCC_BASE          (AHB1PERIPH_BASE + 0x3800U)
#define GPIOA_BASE        (AHB1PERIPH_BASE + 0x0000U)
#define GPIOB_BASE        (AHB1PERIPH_BASE + 0x0400U)
#define GPIOC_BASE        (AHB1PERIPH_BASE + 0x0800U)
#define GPIOD_BASE        (AHB1PERIPH_BASE + 0x0C00U)
#define GPIOE_BASE        (AHB1PERIPH_BASE + 0x1000U)

#define SPI2_BASE         (APB1PERIPH_BASE + 0x3800U)
#define USART3_BASE       (APB1PERIPH_BASE + 0x4800U)
#define USART1_BASE       (APB2PERIPH_BASE + 0x1000U)

#define SYSTICK_BASE      (0xE000E010U)

typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t LOAD;
    volatile uint32_t VAL;
    volatile uint32_t CALIB;
} SysTick_TypeDef;

#define SysTick           ((SysTick_TypeDef *) SYSTICK_BASE)


typedef struct {
    volatile uint32_t CR;
    volatile uint32_t PLLCFGR;
    volatile uint32_t CFGR;
    volatile uint32_t CIR;
    volatile uint32_t AHB1RSTR;
    volatile uint32_t AHB2RSTR;
    volatile uint32_t AHB3RSTR;
    uint32_t RESERVED0;
    volatile uint32_t APB1RSTR;
    volatile uint32_t APB2RSTR;
    uint32_t RESERVED1[2];
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB2ENR;
    volatile uint32_t AHB3ENR;
    uint32_t RESERVED2;
    volatile uint32_t APB1ENR;
    volatile uint32_t APB2ENR;
} RCC_TypeDef;

typedef struct {
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
} GPIO_TypeDef;

typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t CRCPR;
    volatile uint32_t RXCRCR;
    volatile uint32_t TXCRCR;
    volatile uint32_t I2SCFGR;
    volatile uint32_t I2SPR;
} SPI_TypeDef;

typedef struct {
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t BRR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t GTPR;
} USART_TypeDef;

#define RCC     ((RCC_TypeDef *)RCC_BASE)
#define GPIOA   ((GPIO_TypeDef *)GPIOA_BASE)
#define GPIOB   ((GPIO_TypeDef *)GPIOB_BASE)
#define GPIOC   ((GPIO_TypeDef *)GPIOC_BASE)
#define GPIOD   ((GPIO_TypeDef *)GPIOD_BASE)
#define GPIOE   ((GPIO_TypeDef *)GPIOE_BASE)
#define USART1  ((USART_TypeDef *)USART1_BASE)
#define USART3  ((USART_TypeDef *)USART3_BASE)

#define SPI2    ((SPI_TypeDef *)SPI2_BASE)

#endif // STM32F407_REGS_H
