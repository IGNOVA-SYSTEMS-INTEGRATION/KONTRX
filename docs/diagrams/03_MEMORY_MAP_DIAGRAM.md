# Detailed Memory Map Diagrams

## 1. STM32F407 Internal Flash Memory Map (512 KB Total)

```
0x08000000 +---------------------------------------------------+
           | Sector 0 (16 KB)  : Bootloader Core               |
0x08004000 +---------------------------------------------------+
           | Sector 1 (16 KB)  : OTA Metadata (OTA_Meta_t)     |
0x08008000 +---------------------------------------------------+
           | Sector 2 (16 KB)  : User Application Vector Table |
           |                     & Startup Code                |
           +---------------------------------------------------+
           | Sector 3 (16 KB)  :                               |
           +-------------------+  Active Production Firmware   |
           | Sector 4 (64 KB)  :  (FreeRTOS Kernel, HAL,       |
           |                   :   HTTP SPA Gzip, App Tasks)   |
           +-------------------+                               |
           | Sector 5 (128 KB) :                               |
0x08040000 +---------------------------------------------------+
           | Sector 6 (128 KB) :                               |
           |                   :  OTA Staging Area (240 KB)    |
           +-------------------+  (Receives incoming firmware  |
           | Sector 7 (112 KB) :   over HTTP before reboot)    |
0x0807C000 +---------------------------------------------------+
           | Sector 7 Tail (16 KB): Non-Volatile Config EEPROM |
0x0807FFFF +---------------------------------------------------+
```

---

## 2. STM32F407 Internal RAM Map (192 KB Total)

```
0x10000000 +---------------------------------------------------+
           | Core Coupled RAM (CCMRAM) - 64 KB                 |
           | - HTTP Server Large Response TX Buffer (12 KB)    |
           | - Fast ISR Critical Scratch Buffers               |
           | - Zero-wait-state Core Execution                  |
0x1000FFFF +---------------------------------------------------+

0x20000000 +---------------------------------------------------+
           | Main SRAM1 - 112 KB                               |
           | - .data / .bss sections                           |
           | - FreeRTOS Task Stacks (MODBUS, CTRL, HTTP, MQTT) |
           | - FreeRTOS Dynamic Heap (heap_4.c ~ 48 KB)        |
           | - Interrupt Vector Table Relocation               |
0x2001C000 +---------------------------------------------------+
           | Auxiliary SRAM2 - 16 KB                           |
           | - DMA Target Buffers (USART3 DMA RX/TX)           |
           | - W5500 SPI Burst Frame Scratch Buffer            |
0x2001FFFF +---------------------------------------------------+
```

---

## 3. External Winbond W25Q16 SPI NOR Flash Map (2 MB Total)

```
Address Range           Size      Sectors       Function / Contents
--------------------------------------------------------------------------------------
0x00000000 - 0x0007FFFF 512 KB    0 to 127      Partition Web: Compressed Assets
0x00080000 - 0x00080FFF   4 KB    Sector 128    Primary Configuration (Gateway_Config_t)
0x00081000 - 0x00081FFF   4 KB    Sector 129    Backup Configuration (Auto-Recovery)
0x00082000 - 0x00082FFF   4 KB    Sector 130    Primary Rules (RuleConfig_t, Magic 0xC01D7792)
0x00083000 - 0x00083FFF   4 KB    Sector 131    Backup Rules (Rollback Target)
0x00084000 - 0x00087FFF  16 KB    132 to 135    Canvas Layout Blob (JSON, Magic "LAYO")
0x00088000 - 0x00103FFF 496 KB    136 to 259    Offline Telemetry Queue (31,744 Records)
0x00104000 - 0x001FFFFF 1008 KB   260 to 511    System Event & Audit Logs
--------------------------------------------------------------------------------------
```
