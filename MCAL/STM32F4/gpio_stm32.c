#include "gpio_stm32.h"

void GPIO_InitOutput(GPIO_TypeDef* port, uint8_t pin) {
    // Enable Clock for Port
    if (port == GPIOA) RCC->AHB1ENR |= (1 << 0);
    else if (port == GPIOB) RCC->AHB1ENR |= (1 << 1);
    else if (port == GPIOD) RCC->AHB1ENR |= (1 << 3);

    // Set MODER to 01 (General purpose output mode)
    port->MODER &= ~(3U << (pin * 2));
    port->MODER |= (1U << (pin * 2));

    // Set OTYPER to 0 (Output push-pull)
    port->OTYPER &= ~(1U << pin);

    // Set OSPEEDR to 10 (High speed)
    port->OSPEEDR &= ~(3U << (pin * 2));
    port->OSPEEDR |= (2U << (pin * 2));

    // Set PUPDR to 00 (No pull-up, pull-down)
    port->PUPDR &= ~(3U << (pin * 2));
}

void GPIO_TogglePin(GPIO_TypeDef* port, uint8_t pin) {
    port->ODR ^= (1U << pin);
}

void GPIO_Init_USART3_Pins(void) {
    // Enable GPIOB Clock
    RCC->AHB1ENR |= (1 << 1);

    // USART3 TX is PB10, RX is PB11
    // 1. Set MODER to 10 (Alternate function mode)
    GPIOB->MODER &= ~((3U << (10 * 2)) | (3U << (11 * 2)));
    GPIOB->MODER |=  ((2U << (10 * 2)) | (2U << (11 * 2)));

    // Ensure Push-Pull output type
    GPIOB->OTYPER &= ~((1U << 10) | (1U << 11));

    // 2. Set OSPEEDR to 10 (High speed)
    GPIOB->OSPEEDR &= ~((3U << (10 * 2)) | (3U << (11 * 2)));
    GPIOB->OSPEEDR |=  ((2U << (10 * 2)) | (2U << (11 * 2)));

    // 3. Set PUPDR to 01 (Pull-up)
    GPIOB->PUPDR &= ~((3U << (10 * 2)) | (3U << (11 * 2)));
    GPIOB->PUPDR |=  ((1U << (10 * 2)) | (1U << (11 * 2)));

    // 4. Set Alternate Function to AF7 (USART3)
    // AF7 for USART3 on PB10/PB11
    // AFR[1] is AFRH. PB10 -> AFRH[8:11], PB11 -> AFRH[12:15]
    GPIOB->AFR[1] &= ~((0xFU << ((10 - 8) * 4)) | (0xFU << ((11 - 8) * 4)));
    GPIOB->AFR[1] |=  ((7U << ((10 - 8) * 4)) | (7U << ((11 - 8) * 4)));
}

void GPIO_Init_USART1_Pins(void) {
    // Enable GPIOA Clock
    RCC->AHB1ENR |= (1 << 0);

    // USART1 TX is PA9, RX is PA10
    GPIOA->MODER &= ~((3U << (9 * 2)) | (3U << (10 * 2)));
    GPIOA->MODER |=  ((2U << (9 * 2)) | (2U << (10 * 2)));

    GPIOA->OSPEEDR &= ~((3U << (9 * 2)) | (3U << (10 * 2)));
    GPIOA->OSPEEDR |=  ((2U << (9 * 2)) | (2U << (10 * 2)));

    GPIOA->PUPDR &= ~((3U << (9 * 2)) | (3U << (10 * 2)));
    GPIOA->PUPDR |=  ((1U << (9 * 2)) | (1U << (10 * 2)));

    // AF7 for USART1 on PA9/PA10
    // AFR[1] is AFRH. PA9 -> AFRH[4:7], PA10 -> AFRH[8:11]
    GPIOA->AFR[1] &= ~((0xFU << ((9 - 8) * 4)) | (0xFU << ((10 - 8) * 4)));
    GPIOA->AFR[1] |=  ((7U << ((9 - 8) * 4)) | (7U << ((10 - 8) * 4)));
}

void GPIO_Init_MAX485_Pins(void) {
    // DE: PD3, RE: PD2
    // Both need to be output pins
    GPIO_InitOutput(GPIOD, 3);
    GPIO_InitOutput(GPIOD, 2);

    // Initially receive mode (DE=0, RE=0)
    MAX485_Receive_Enable();
}

void MAX485_Transmit_Enable(void) {
    // DE (PD3) = 1, RE (PD2) = 1
    GPIOD->BSRR = (1U << 3) | (1U << 2);
}

void MAX485_Receive_Enable(void) {
    // DE (PD3) = 0, RE (PD2) = 0
    // To reset, shift by 16 in BSRR
    GPIOD->BSRR = (1U << (3 + 16)) | (1U << (2 + 16));
}

void GPIO_Init_W5500_Pins(void) {
    // Enable GPIOB and GPIOC Clock
    RCC->AHB1ENR |= (1 << 1) | (1 << 2);

    // SPI2: PB13 (SCK), PB14 (MISO), PB15 (MOSI)
    // 1. Set MODER to 10 (Alternate function mode)
    GPIOB->MODER &= ~((3U << (13 * 2)) | (3U << (14 * 2)) | (3U << (15 * 2)));
    GPIOB->MODER |=  ((2U << (13 * 2)) | (2U << (14 * 2)) | (2U << (15 * 2)));

    // 2. Set OSPEEDR to 10 (High speed)
    GPIOB->OSPEEDR &= ~((3U << (13 * 2)) | (3U << (14 * 2)) | (3U << (15 * 2)));
    GPIOB->OSPEEDR |=  ((2U << (13 * 2)) | (2U << (14 * 2)) | (2U << (15 * 2)));

    // 3. Set AF5 for SPI2
    GPIOB->AFR[1] &= ~((0xFU << ((13 - 8) * 4)) | (0xFU << ((14 - 8) * 4)) | (0xFU << ((15 - 8) * 4)));
    GPIOB->AFR[1] |=  ((5U << ((13 - 8) * 4)) | (5U << ((14 - 8) * 4)) | (5U << ((15 - 8) * 4)));

    // CS: PB12 (Output)
    GPIO_InitOutput(GPIOB, 12);
    W5500_CS_Deselect(); // Initially High

    // RST: PC4 (Output)
    // Wait, GPIO_InitOutput only supports GPIOA, GPIOB, GPIOD in its current implementation!
    // I will manually init PC4 here to be safe.
    GPIOC->MODER &= ~(3U << (4 * 2));
    GPIOC->MODER |= (1U << (4 * 2));
    GPIOC->OTYPER &= ~(1U << 4);
    GPIOC->OSPEEDR &= ~(3U << (4 * 2));
    GPIOC->OSPEEDR |= (2U << (4 * 2));
    GPIOC->PUPDR &= ~(3U << (4 * 2));
    
    // Set RST high initially
    GPIOC->BSRR = (1U << 4);
}

void W5500_CS_Select(void) {
    GPIOB->BSRR = (1U << (12 + 16)); // Low
}

void W5500_CS_Deselect(void) {
    GPIOB->BSRR = (1U << 12); // High
}

void W5500_Hardware_Reset(void) {
    GPIOC->BSRR = (1U << (4 + 16)); // Low
    for (volatile uint32_t i = 0; i < 50000; i++) __asm__("nop"); // Delay
    GPIOC->BSRR = (1U << 4); // High
    for (volatile uint32_t i = 0; i < 500000; i++) __asm__("nop"); // Delay
}

void GPIO_Init_PE4_Button(void) {
    // Enable GPIOE Clock
    RCC->AHB1ENR |= (1U << 4);
    
    // Configure PE4 as Input
    GPIOE->MODER &= ~(3U << (4 * 2)); // 00: Input
    
    // Enable Pull-up on PE4
    GPIOE->PUPDR &= ~(3U << (4 * 2));
    GPIOE->PUPDR |= (1U << (4 * 2));  // 01: Pull-up
}

uint8_t GPIO_Read_PE4(void) {
    // Return 1 if pressed (assuming active low with pull-up)
    return (GPIOE->IDR & (1U << 4)) == 0 ? 1 : 0;
}
