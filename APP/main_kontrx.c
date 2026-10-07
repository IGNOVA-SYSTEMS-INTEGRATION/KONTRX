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
#include "rcc_stm32.h"
#include "uart_stm32.h"
#include "gpio_stm32.h"
#include "spi_stm32.h"
#include "flash_stm32.h"
#include "rtc_stm32.h"
#include "led.h"
#include "modbus_dma.h"
#include "freertos_tasks.h"
#include "flash_partition.h"
#include "w25q16.h"
#include "w5500.h"
#include "socket.h"
#include "dhcp.h"
#include "wizchip_conf.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "pwm_controller.h"
#include "pto_motion.h"
#include "dac_420ma.h"
#include "analog_010v.h"
#include "interface_discovery.h"
#include "modbus_tcp_server.h"
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

uint8_t g_diag_w5500_version = 0;
uint8_t g_diag_ip[4] = {0};
uint8_t g_diag_gw[4] = {0};

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
    printf("\r\n!!! FreeRTOS MALLOC FAILED -> Auto-resetting !!!\r\n");
    SCB_AIRCR = AIRCR_VECTKEY | AIRCR_SYSRESET;
    while (1);
}

/* ======================================================================
 *  Load gateway config from EEPROM (Sector 7 tail, CONFIG_FLASH_ADDR)
 *  Falls back to safe defaults if magic is wrong.
 * ====================================================================== */
static void Load_Config_From_Flash(void) {
    // 1. Initialize Partitions (which also initializes W25Q16 physical flash)
    Partition_Init();

    // 2. Try loading config from Partition Manager
    uint8_t config_ok = 0;
    if (Partition_LoadConfig(&sharedConfig)) {
        if (sharedConfig.actuator_count <= MAX_RELAYS &&
            sharedConfig.sensors.count <= MAX_SENSORS &&
            sharedConfig.mqtt_mapping_count <= MAX_MQTT_MAPPINGS) {
            config_ok = 1;
            printf("[CFG] Loaded config from external flash partition successfully.\r\n");
        } else {
            printf("[CFG] Loaded config failed validation (counts out of bounds)! Resetting to defaults...\r\n");
        }
    }

    if (!config_ok) {
        printf("[CFG] Configuration partition blank/corrupted. Initializing defaults...\r\n");
        memset(&sharedConfig, 0, sizeof(Gateway_Config_t));
        
        sharedConfig.magic = CONFIG_MAGIC_CURRENT;
        sharedConfig.serial = 2; // KX-0000002
        sharedConfig.mqtt_port = 1883;
        sharedConfig.mqtt_interval = 2;
        sharedConfig.mqtt_send_mode = 0; // Periodic
        sharedConfig.mqtt_tx_enabled = 1; // Publishing Active
        sharedConfig.test_mode = 0;      // Normal/Automated Rules Mode
        
        sharedConfig.mqtt_broker[0] = '\0';
        strcpy(sharedConfig.sparkplug_topic, "test/topic/12345");
        strcpy(sharedConfig.provision_status, "Active");
        strcpy(sharedConfig.provision_message, "Provisioned manually via Cloud MQTT settings");

        // Map default 8 local GPIO relays
        const char *names[8] = {"Relay1", "Relay2", "Relay3", "Relay4", "Relay5", "Relay6", "Relay7", "Relay8"};
        const char *ports[8] = {"PE", "PE", "PE", "PC", "PC", "PA", "PA", "PA"};
        const uint8_t pins[8]  = {2, 4, 6, 0, 2, 1, 0, 2};
        
        for (int i = 0; i < 8; i++) {
            sharedConfig.actuators[i].id = i;
            strcpy(sharedConfig.actuators[i].name, names[i]);
            sharedConfig.actuators[i].type = ACTUATOR_TYPE_LOCAL_GPIO;
            sharedConfig.actuators[i].is_active_low = 0;
            sharedConfig.actuators[i].state = 0;
            
            strcpy(sharedConfig.actuators[i].port_or_ip, ports[i]);
            sharedConfig.actuators[i].pin_or_slave = pins[i];
            sharedConfig.actuators[i].port = 0;
            sharedConfig.actuators[i].reg_addr = 0;
        }

        // Slot 8: PWM1 (TIM4_CH1 on PD12, Channel 0)
        sharedConfig.actuators[8].id = 8;
        strcpy(sharedConfig.actuators[8].name, "PWM1_Fan");
        sharedConfig.actuators[8].type = ACTUATOR_TYPE_PWM;
        sharedConfig.actuators[8].pin_or_slave = 0; // Channel 0 (PD12)
        sharedConfig.actuators[8].reg_addr = 1000;  // 1000 Hz default frequency
        strcpy(sharedConfig.actuators[8].port_or_ip, "CH1");

        // Slot 9: PWM2 (TIM4_CH2 on PD13, Channel 1)
        sharedConfig.actuators[9].id = 9;
        strcpy(sharedConfig.actuators[9].name, "PWM2_Pump");
        sharedConfig.actuators[9].type = ACTUATOR_TYPE_PWM;
        sharedConfig.actuators[9].pin_or_slave = 1; // Channel 1 (PD13)
        sharedConfig.actuators[9].reg_addr = 1000;
        strcpy(sharedConfig.actuators[9].port_or_ip, "CH2");

        // Slot 10: PTO Axis 1 (TIM1 on PE9/PE8)
        sharedConfig.actuators[10].id = 10;
        strcpy(sharedConfig.actuators[10].name, "PTO_Axis1");
        sharedConfig.actuators[10].type = ACTUATOR_TYPE_PTO;
        sharedConfig.actuators[10].pin_or_slave = 0; // Axis 0
        sharedConfig.actuators[10].reg_addr = 1000;  // Default speed
        strcpy(sharedConfig.actuators[10].port_or_ip, "AXIS1");

        // Slot 11: Analog 0-10V Channel 1 (PD14)
        sharedConfig.actuators[11].id = 11;
        strcpy(sharedConfig.actuators[11].name, "0-10V_VFD");
        sharedConfig.actuators[11].type = ACTUATOR_TYPE_ANALOG_V;
        sharedConfig.actuators[11].pin_or_slave = 0; // Channel 0
        strcpy(sharedConfig.actuators[11].port_or_ip, "CH1");

        // Slot 12: Analog 4-20mA Current Output 1 (PA4)
        sharedConfig.actuators[12].id = 12;
        strcpy(sharedConfig.actuators[12].name, "4-20mA_Dose");
        sharedConfig.actuators[12].type = ACTUATOR_TYPE_ANALOG_MA;
        sharedConfig.actuators[12].pin_or_slave = 0; // Channel 0
        strcpy(sharedConfig.actuators[12].port_or_ip, "CH1");

        sharedConfig.actuator_count = 13;

        // Default telemetry field mappings
        sharedConfig.mqtt_mappings[0] = (Mqtt_Field_Mapping_t){.source_type=MAP_SOURCE_SENSOR, .source_id=1, .json_key="ph", .enabled=1};
        sharedConfig.mqtt_mappings[1] = (Mqtt_Field_Mapping_t){.source_type=MAP_SOURCE_SENSOR, .source_id=3, .json_key="ec", .enabled=1};
        sharedConfig.mqtt_mappings[2] = (Mqtt_Field_Mapping_t){.source_type=MAP_SOURCE_SENSOR, .source_id=4, .json_key="do", .enabled=1};
        sharedConfig.mqtt_mappings[3] = (Mqtt_Field_Mapping_t){.source_type=MAP_SOURCE_SENSOR, .source_id=5, .json_key="ammonia", .enabled=1};
        sharedConfig.mqtt_mappings[4] = (Mqtt_Field_Mapping_t){.source_type=MAP_SOURCE_SENSOR, .source_id=10, .json_key="multi_us", .enabled=1};
        sharedConfig.mqtt_mapping_count = 5;

        strncpy(sharedConfig.admin_username, "admin", sizeof(sharedConfig.admin_username));
        strncpy(sharedConfig.admin_password, "adminkontrx", sizeof(sharedConfig.admin_password));
        sharedConfig.actuator_mask = 0x0F; // All enabled by default (PTO, 0-10V, 4-20mA, PWM)

        Partition_SaveConfig(&sharedConfig);
        printf("[CFG] Default configurations written to flash partitions.\r\n");
    }

    // 3. Web Assets are served directly from MCU Internal Flash (KONTRX_HTML in APP/web_assets.h)
}

/* ======================================================================
 *  main()
 * ====================================================================== */
int main(void) {
    memset((void *)&g_mqtt_status, 0, sizeof(g_mqtt_status));

    /* ---- 0. High-Performance Clock: 168 MHz via PLL (HSE / HSI auto-fallback) ---- */
    RCC_SystemClock_168MHz_Init();

    /* ---- 1. NVIC priority grouping: 4 bits preemption, 0 sub-priority ---- */
    SCB_AIRCR = AIRCR_VECTKEY | (3U << 8);

    /* ---- 2. UART Debug ---- */
    GPIO_Init_USART1_Pins();
    UART_Debug_Init();
    RTC_Init();
    printf("\r\n\r\n");
    Log_Event("SYS", "System initialized and booted successfully at 168 MHz.");
    printf("╔══════════════════════════════════════════╗\r\n");
    printf("║  Kontrx Edge Gateway  v2.1.0 (Universal) ║\r\n");
    printf("║  STM32F407VET6 @ 168 MHz + W5500 + RTOS  ║\r\n");
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
        printf("[NET] W5500 SPI OK: VERSIONR=0x04. PHY auto-negotiation running in background.\r\n");
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
    
    g_diag_w5500_version = w5500_version;
    memcpy(g_diag_ip, net_readback.ip, 4);
    memcpy(g_diag_gw, net_readback.gw, 4);

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
    Log_Event("SYS", "Configuration loaded from Flash Sector 7.");

    /* ---- 9. Initialise relay GPIO from config ---- */
    Relay_Init();

    /* ---- 9b. Universal Controller Hardware Subsystems ---- */
    PWM_Controller_Init();
    PTO_Motion_Init();
    DAC_420MA_Init();
    Analog_010V_Init();
    Interface_Discovery_Init();
    Modbus_TCP_Server_Init();
    printf("[SYS] Universal Hardware Subsystems Initialized (PWM, PTO, DAC 4-20mA, 0-10V, Modbus TCP Server).\r\n");

    printf("[SYS] Hardware init complete. Starting RTOS...\r\n\r\n");

    /* ---- 10. Launch RTOS — does not return ---- */
    KontrxDWT_Init();   /* Start DWT cycle counter for CPU usage measurement */
    RTOS_Tasks_Init();

    /* Unreachable */
    while (1);
    return 0;
}
