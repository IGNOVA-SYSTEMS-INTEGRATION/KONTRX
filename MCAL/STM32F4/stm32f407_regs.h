#ifndef STM32F407_REGS_H
#define STM32F407_REGS_H

#include <stdint.h>

#define __IO volatile

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
#define USART6_BASE       (APB2PERIPH_BASE + 0x1400U)

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
#define USART6  ((USART_TypeDef *)USART6_BASE)

#define SPI2    ((SPI_TypeDef *)SPI2_BASE)

/* ------------------------------------------------------------------ */
/*  Flash Interface Registers (STM32F4)                                */
/* ------------------------------------------------------------------ */
#define FLASH_BASE_ADDR   (AHB1PERIPH_BASE + 0x3C00U)

typedef struct {
    volatile uint32_t ACR;      /* 0x00 — Access control           */
    volatile uint32_t KEYR;     /* 0x04 — Key register (unlock)    */
    volatile uint32_t OPTKEYR;  /* 0x08 — Option key register      */
    volatile uint32_t SR;       /* 0x0C — Status register          */
    volatile uint32_t CR;       /* 0x10 — Control register         */
    volatile uint32_t OPTCR;    /* 0x14 — Option control register  */
} FLASH_TypeDef;

#define FLASH_CTRL  ((FLASH_TypeDef *)FLASH_BASE_ADDR)

/* Flash CR bits */
#define FLASH_CR_PG      (1U << 0)    /* Programming                 */
#define FLASH_CR_SER     (1U << 1)    /* Sector erase                */
#define FLASH_CR_MER     (1U << 2)    /* Mass erase                  */
#define FLASH_CR_SNB_POS (3U)         /* Sector number bit position  */
#define FLASH_CR_PSIZE32 (2U << 8)    /* x32 parallelism (VCC=2.7V+) */
#define FLASH_CR_STRT    (1U << 16)   /* Start erase                 */
#define FLASH_CR_LOCK    (1U << 31)   /* Lock bit                    */

/* Flash SR bits */
#define FLASH_SR_EOP     (1U << 0)    /* End of operation            */
#define FLASH_SR_OPERR   (1U << 1)    /* Operation error             */
#define FLASH_SR_WRPERR  (1U << 4)    /* Write protection error      */
#define FLASH_SR_BSY     (1U << 16)   /* Busy                        */

/* Unlock keys */
#define FLASH_KEY1  0x45670123U
#define FLASH_KEY2  0xCDEF89ABU

/* SCB for system reset */
#define SCB_AIRCR   (*((volatile uint32_t *)0xE000ED0CU))
#define SCB_VTOR    (*((volatile uint32_t *)0xE000ED08U))
#define AIRCR_VECTKEY    (0x05FA0000U)
#define AIRCR_SYSRESET   (1U << 2)

#endif // STM32F407_REGS_H
