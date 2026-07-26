/**
 * @file    main_kontrx.c
 * @brief   Kontrx Edge Gateway — Master Application Entry Point
 *
 * Boot sequence (single-core, runs before scheduler starts):
 *   1. NVIC priority grouping — all 4 bits for preemption
 *   2. Debug UART (USART1, 115200 baud)
 *   3. SPI2 + W5500 hardware reset
 *   4. W5500 driver registration (SPI callbacks)
 *   5. Static IP configuration (DHCP optional: comment-in block below)
 *   6. Config EEPROM load from Sector 11
 *   7. Relay GPIO init (from loaded config)
 *   8. RTOS_Tasks_Init() → creates 4 tasks → starts scheduler (no return)
 *
 * Non-returning: vTaskStartScheduler() is called inside RTOS_Tasks_Init().
 *
 * Peripheral Map (all configured before RTOS starts):
 *   USART1 PA9/PA10 AF7  → Debug printf
 *   SPI2   PB13/14/15 AF5, CS=PB12, RST=PC4 → W5500
 *   USART3 PB10/PB11 AF7, DE=PD3, RE=PD2   → MAX485 / Modbus (DMA init in task)
 *   PE2,PE4,PE6,PC0,PC2,PA0,PA2,PA4,PC4,PD8 → Relays (configurable)
 */

#include "stm32f407_regs.h"
#include "uart_stm32.h"
#include "gpio_stm32.h"
#include "spi_stm32.h"
#include "flash_stm32.h"
#include "rtc_stm32.h"
#include "led.h"
#include "modbus_dma.h"
#include "freertos_tasks.h"
#include "w5500.h"
#include "socket.h"
#include "dhcp.h"
#include "wizchip_conf.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* ======================================================================
 *  printf → Debug USART1 bridge
 * ====================================================================== */
int _write(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) UART_Debug_SendByte((uint8_t)ptr[i]);
    return len;
}

/* ======================================================================
 *  WIZnet SPI callbacks (ioLibrary expects byte-at-a-time interface)
 *  CS pin functions are defined in gpio_stm32.c (W5500_CS_Select/Deselect)
 * ====================================================================== */
static uint8_t W5500_SPI_ReadByte(void)           { return SPI2_ReadWriteByte(0xFF); }
static void    W5500_SPI_WriteByte(uint8_t data)  { SPI2_ReadWriteByte(data); }

/* ======================================================================
 *  W5500 hardware reset
 * ====================================================================== */
static void W5500_Reset(void) {
    /* RST = PC4 (active LOW) */
    GPIOC->BSRR = (1U << (4 + 16)); /* Pull LOW */
    for (volatile uint32_t i = 0; i < 50000; i++);
    GPIOC->BSRR = (1U << 4);        /* Release HIGH */
    for (volatile uint32_t i = 0; i < 200000; i++);
}

/* ======================================================================
 *  W5500 Tick integration for DHCP library
 *  (The MilliTimer_Handler is called from our vApplicationTickHook in freertos_tasks.c)
 * ====================================================================== */

/* ======================================================================
 *  Stack overflow / malloc hooks
 * ====================================================================== */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    printf("\r\n!!! STACK OVERFLOW: %s !!!\r\n", pcTaskName);
    while (1);
}

void vApplicationMallocFailedHook(void) {
    printf("\r\n!!! FreeRTOS MALLOC FAILED !!!\r\n");
    while (1);
}

/* ======================================================================
 *  Load gateway config from EEPROM (Sector 11, 0x080E0000)
 *  Falls back to safe defaults if magic is wrong.
 * ====================================================================== */
static void Load_Config_From_Flash(void) {
    #define CONFIG_MAGIC 0xC01D0001U
    Gateway_Config_t *flash_cfg = (Gateway_Config_t *)0x080E0000U;

    if (flash_cfg->magic == CONFIG_MAGIC) {
        Update_Shared_Config(flash_cfg);
        printf("[CFG] Loaded config from flash (magic OK)\r\n");
    } else {
        printf("[CFG] No saved config found, using defaults\r\n");
        /* Defaults are already set by Modbus_DMA_Init() → sharedConfig zeroed */
    }
}

/* ======================================================================
 *  main()
 * ====================================================================== */
int main(void) {

    /* ---- 1. NVIC priority grouping: 4 bits preemption, 0 sub-priority ---- */
    SCB_AIRCR = AIRCR_VECTKEY | (3U << 8);

    /* ---- 2. UART Debug ---- */
    GPIO_Init_USART1_Pins();
    UART_Debug_Init();
    RTC_Init();
    printf("\r\n\r\n");
    printf("╔══════════════════════════════════════════╗\r\n");
    printf("║  Kontrx Edge Gateway  v2.0.0             ║\r\n");
    printf("║  STM32F407VET6 + W5500 + FreeRTOS CMSIS  ║\r\n");
    printf("╚══════════════════════════════════════════╝\r\n\r\n");

    /* ---- 3. SPI2 + W5500 pins ---- */
    GPIO_Init_W5500_Pins();
    SPI2_Init();

    /* PC4 = RST, configure as output */
    RCC->AHB1ENR |= (1U << 2); /* GPIOC clock */
    GPIOC->MODER  &= ~(3U << (4 * 2));
    GPIOC->MODER  |=  (1U << (4 * 2));
    GPIOC->OSPEEDR|=  (2U << (4 * 2));
    GPIOC->OTYPER &= ~(1U << 4);
    GPIOC->BSRR = (1U << 4); /* RST HIGH initially */

    /* ---- 4. W5500 Reset + Driver Init ---- */
    W5500_Reset();

    reg_wizchip_cs_cbfunc(W5500_CS_Select, W5500_CS_Deselect);
    reg_wizchip_spi_cbfunc(W5500_SPI_ReadByte, W5500_SPI_WriteByte);

    /* Set RX/TX buffer sizes for 8 sockets (2KB each = 16KB total) */
    uint8_t rx_tx_buf_sizes[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    if (wizchip_init(rx_tx_buf_sizes, rx_tx_buf_sizes) != 0) {
        printf("[NET] wizchip_init FAILED\r\n");
    }

    /* ---- 5. Static IP Configuration ---- */
    wiz_NetInfo net = {
        .mac  = {0x00, 0x08, 0xDC, 0x11, 0x22, 0x33},
        .ip   = {192, 168, 1, 200},
        .sn   = {255, 255, 255, 0},
        .gw   = {192, 168, 1, 1},
        .dns  = {8, 8, 8, 8},
        .dhcp = NETINFO_STATIC
    };
    ctlnetwork(CN_SET_NETINFO, &net);
    printf("[NET] IP: %d.%d.%d.%d  GW: %d.%d.%d.%d\r\n",
        net.ip[0], net.ip[1], net.ip[2], net.ip[3],
        net.gw[0], net.gw[1], net.gw[2], net.gw[3]);

    /* ---- 6. LED ---- */
    LED_Init();
    LED_On();

    /* ---- 7. Initialise shared RTOS state (mutexes + default sensor values) ---- */
    /* Note: Modbus_DMA_Init() is called inside Task_ModbusSensorPoll.
     * But we need sensorMutex & configMutex to exist BEFORE tasks start.
     * Create them here. modbus_dma.c will skip re-creation if non-NULL. */
    if (!sensorMutex) sensorMutex = osMutexNew(NULL);
    if (!configMutex) configMutex = osMutexNew(NULL);

    /* ---- 8. Load saved relay/MQTT config from EEPROM ---- */
    Load_Config_From_Flash();

    /* ---- 9. Initialise relay GPIO from config ---- */
    Relay_Init();

    printf("[SYS] Hardware init complete. Starting RTOS...\r\n\r\n");

    /* ---- 10. Launch RTOS — does not return ---- */
    RTOS_Tasks_Init();

    /* Unreachable */
    while (1);
    return 0;
}
