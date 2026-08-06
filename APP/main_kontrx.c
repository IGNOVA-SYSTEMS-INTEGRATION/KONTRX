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
 *   SPI2   PB13/14/15 AF5, CS=PB12        → W5500
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
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

/* ======================================================================
 *  W5500 SPI mutex — shared between all tasks that call WIZnet library
 *  functions (HTTP server socket 0, MQTT client socket 1).  Must be
 *  acquired before every multi-byte SPI transaction and released after,
 *  otherwise concurrent access corrupts the W5500 register state and
 *  causes random ERR_CONNECTION_TIMED_OUT drops in the browser.
 *
 *  IMPORTANT: Created with xSemaphoreCreateMutex() (not osMutexNew()) so
 *  it can be created BEFORE osKernelStart(). osMutexNew() is a CMSIS-RTOS2
 *  wrapper that internally calls OS services unavailable before the scheduler
 *  starts, resulting in a NULL handle and an unprotected SPI bus.
 * ====================================================================== */
#include "semphr.h"
SemaphoreHandle_t spiMutex = NULL;

static void SPI_CritEnter(void) {
    if (spiMutex && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreTakeRecursive(spiMutex, portMAX_DELAY);
    }
}
static void SPI_CritExit(void) {
    if (spiMutex && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreGiveRecursive(spiMutex);
    }
}


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
    #define CONFIG_MAGIC CONFIG_MAGIC_CURRENT
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

    /* ---- 4. W5500 Driver Init ---- */
    /* (Hard reset already done by bootloader's W5500_BootReset() via SPI,
     * so we only need the software reset + init here.) */

    /* Create SPI mutex BEFORE registering WIZnet callbacks.
     * Use xSemaphoreCreateRecursiveMutex() so that nested critical section
     * entry (which is extremely common inside WIZnet ioLibrary functions like
     * high-level socket APIs calling low-level register writes) does not deadlock the task. */
    spiMutex = xSemaphoreCreateRecursiveMutex();
    if (!spiMutex) {
        printf("[SPI] FATAL: spiMutex creation failed!\r\n");
        while (1);  /* halt — SPI bus would be unprotected */
    }

    reg_wizchip_cris_cbfunc(SPI_CritEnter, SPI_CritExit); /* SPI bus mutex */
    reg_wizchip_cs_cbfunc(W5500_CS_Select, W5500_CS_Deselect);
    reg_wizchip_spi_cbfunc(W5500_SPI_ReadByte, W5500_SPI_WriteByte);

    /* Start from known W5500 register state.  This also clears a stale
     * PING-block/PPPoE mode or an abandoned socket left by a brown-out. */
    wizchip_sw_reset();

    /* Set RX/TX buffer sizes for 8 sockets (2KB each = 16KB total) */
    uint8_t rx_tx_buf_sizes[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    if (wizchip_init(rx_tx_buf_sizes, rx_tx_buf_sizes) != 0) {
        printf("[NET] wizchip_init FAILED\r\n");
    }

    /* VERSIONR is a direct SPI sanity check.  It must be 0x04 on a W5500;
     * report a wiring/reset fault explicitly instead of only printing an IP
     * address that was stored in software. */
    uint8_t w5500_version = getVERSIONR();
    if (w5500_version != 0x04U) {
        printf("[NET] W5500 SPI FAILED: VERSIONR=0x%02X (expected 0x04)\r\n",
               w5500_version);
    } else {
        /* Wait for PHY link (bootloader may have started auto-negotiation
         * but the link may not be up yet). */
        uint32_t phy_tries = 30;
        while (wizphy_getphylink() != PHY_LINK_ON && phy_tries-- > 0) {
            printf("[NET] Waiting for PHY link...\r\n");
            /* Busy-wait ~?ms per try. Runs BEFORE the RTOS scheduler starts
             * (main loop, no tasks yet), so it does NOT affect the 1ms Control
             * Engine. Max ~30 tries; only delays boot, not runtime timing. */
            for (volatile uint32_t i = 0; i < 1000000; i++);
        }
        printf("[NET] W5500 SPI OK: VERSIONR=0x04, PHY link %s\r\n",
               (wizphy_getphylink() == PHY_LINK_ON) ? "UP" : "DOWN");
    }

    /* Keep normal ping reception enabled. */
    setMR(0);

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

    /* Read the registers back from the chip.  Printing the requested values
     * alone can hide a failed SPI write and makes ARP failures opaque. */
    wiz_NetInfo net_readback = {0};
    ctlnetwork(CN_GET_NETINFO, &net_readback);
    printf("[NET] IP readback: %d.%d.%d.%d  GW: %d.%d.%d.%d  MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
        net_readback.ip[0], net_readback.ip[1], net_readback.ip[2], net_readback.ip[3],
        net_readback.gw[0], net_readback.gw[1], net_readback.gw[2], net_readback.gw[3],
        net_readback.mac[0], net_readback.mac[1], net_readback.mac[2],
        net_readback.mac[3], net_readback.mac[4], net_readback.mac[5]);

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
    KontrxDWT_Init();   /* Start DWT cycle counter for CPU usage measurement */
    RTOS_Tasks_Init();

    /* Unreachable */
    while (1);
    return 0;
}
