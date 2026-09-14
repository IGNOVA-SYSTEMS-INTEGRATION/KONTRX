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
#define SPI3_BASE         (APB1PERIPH_BASE + 0x3C00U)
#define USART2_BASE       (APB1PERIPH_BASE + 0x4400U)
#define USART3_BASE       (APB1PERIPH_BASE + 0x4800U)
#define TIM2_BASE         (APB1PERIPH_BASE + 0x0000U)
#define TIM3_BASE         (APB1PERIPH_BASE + 0x0400U)
#define TIM4_BASE         (APB1PERIPH_BASE + 0x0800U)
#define TIM14_BASE        (APB1PERIPH_BASE + 0x2000U)
#define PWR_BASE          (APB1PERIPH_BASE + 0x7000U)

#define TIM1_BASE         (APB2PERIPH_BASE + 0x0000U)
#define USART1_BASE       (APB2PERIPH_BASE + 0x1000U)
#define USART6_BASE       (APB2PERIPH_BASE + 0x1400U)
#define ADC1_BASE         (APB2PERIPH_BASE + 0x2000U)
#define ADC_COMMON_BASE   (APB2PERIPH_BASE + 0x2300U)
#define SPI1_BASE         (APB2PERIPH_BASE + 0x3000U)
#define SYSCFG_BASE       (APB2PERIPH_BASE + 0x3800U)
#define EXTI_BASE         (APB2PERIPH_BASE + 0x3C00U)
#define TIM9_BASE         (APB2PERIPH_BASE + 0x4000U)
#define TIM10_BASE        (APB2PERIPH_BASE + 0x4400U)
#define TIM11_BASE        (APB2PERIPH_BASE + 0x4800U)

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

typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMCR;
    volatile uint32_t DIER;
    volatile uint32_t SR;
    volatile uint32_t EGR;
    volatile uint32_t CCMR1;
    volatile uint32_t CCMR2;
    volatile uint32_t CCER;
    volatile uint32_t CNT;
    volatile uint32_t PSC;
    volatile uint32_t ARR;
    volatile uint32_t RCR;
    volatile uint32_t CCR1;
    volatile uint32_t CCR2;
    volatile uint32_t CCR3;
    volatile uint32_t CCR4;
    volatile uint32_t BDTR;
    volatile uint32_t DCR;
    volatile uint32_t DMAR;
} TIM_TypeDef;

typedef struct {
    volatile uint32_t CR;
    volatile uint32_t CSR;
} PWR_TypeDef;

typedef struct {
    volatile uint32_t MEMRMP;
    volatile uint32_t PMC;
    volatile uint32_t EXTICR[4];
    volatile uint32_t CMPCR;
} SYSCFG_TypeDef;

typedef struct {
    volatile uint32_t IMR;
    volatile uint32_t EMR;
    volatile uint32_t RTSR;
    volatile uint32_t FTSR;
    volatile uint32_t SWIER;
    volatile uint32_t PR;
} EXTI_TypeDef;

typedef struct {
    volatile uint32_t SR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMPR1;
    volatile uint32_t SMPR2;
    volatile uint32_t JOFR1;
    volatile uint32_t JOFR2;
    volatile uint32_t JOFR3;
    volatile uint32_t JOFR4;
    volatile uint32_t HTR;
    volatile uint32_t LTR;
    volatile uint32_t SQR1;
    volatile uint32_t SQR2;
    volatile uint32_t SQR3;
    volatile uint32_t JSQR;
    volatile uint32_t JDR1;
    volatile uint32_t JDR2;
    volatile uint32_t JDR3;
    volatile uint32_t JDR4;
    volatile uint32_t DR;
} ADC_TypeDef;

typedef struct {
    volatile uint32_t CSR;
    volatile uint32_t CCR;
    volatile uint32_t CDR;
} ADC_Common_TypeDef;

#define RCC         ((RCC_TypeDef *)RCC_BASE)
#define GPIOA       ((GPIO_TypeDef *)GPIOA_BASE)
#define GPIOB       ((GPIO_TypeDef *)GPIOB_BASE)
#define GPIOC       ((GPIO_TypeDef *)GPIOC_BASE)
#define GPIOD       ((GPIO_TypeDef *)GPIOD_BASE)
#define GPIOE       ((GPIO_TypeDef *)GPIOE_BASE)

#define USART1      ((USART_TypeDef *)USART1_BASE)
#define USART2      ((USART_TypeDef *)USART2_BASE)
#define USART3      ((USART_TypeDef *)USART3_BASE)
#define USART6      ((USART_TypeDef *)USART6_BASE)

#define SPI1        ((SPI_TypeDef *)SPI1_BASE)
#define SPI2        ((SPI_TypeDef *)SPI2_BASE)
#define SPI3        ((SPI_TypeDef *)SPI3_BASE)

#define TIM1        ((TIM_TypeDef *)TIM1_BASE)
#define TIM2        ((TIM_TypeDef *)TIM2_BASE)
#define TIM3        ((TIM_TypeDef *)TIM3_BASE)
#define TIM4        ((TIM_TypeDef *)TIM4_BASE)
#define TIM9        ((TIM_TypeDef *)TIM9_BASE)
#define TIM10       ((TIM_TypeDef *)TIM10_BASE)
#define TIM11       ((TIM_TypeDef *)TIM11_BASE)
#define TIM14       ((TIM_TypeDef *)TIM14_BASE)

#define PWR         ((PWR_TypeDef *)PWR_BASE)
#define SYSCFG      ((SYSCFG_TypeDef *)SYSCFG_BASE)
#define EXTI        ((EXTI_TypeDef *)EXTI_BASE)
#define ADC1        ((ADC_TypeDef *)ADC1_BASE)
#define ADC_COMMON  ((ADC_Common_TypeDef *)ADC_COMMON_BASE)

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
