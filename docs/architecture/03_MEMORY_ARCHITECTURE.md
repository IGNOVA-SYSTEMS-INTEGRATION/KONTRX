# 03 - Memory & Storage Architecture: Internal Flash, CCMRAM & External Media

## 1. Overview
KONTRX features a hybrid memory model designed to prevent heap fragmentation, guarantee zero stack overflows during network bursts, and protect critical persistent data against power failure.

---

## 2. Microcontroller Internal Memory Map (STM32F407)

```text
0x08000000 ┌────────────────────────────────────────────────────────┐
           │ Sector 0 (16 KB)  : Stage-1 Bootloader (Bootloader.bin)│
0x08004000 ├────────────────────────────────────────────────────────┤
           │ Sector 1 (16 KB)  : Boot Metadata & Magic Status Flags │
0x08008000 ├────────────────────────────────────────────────────────┤
           │ Sectors 2-5 (224 KB): Active Application (KontrxRTOS)   │
0x08040000 ├────────────────────────────────────────────────────────┤
           │ Sectors 6-7 (256 KB): Firmware Staging Buffer (OTA)    │
0x08080000 └────────────────────────────────────────────────────────┘

0x10000000 ┌────────────────────────────────────────────────────────┐
           │ CCMRAM (64 KB Fast Core-Coupled Memory)                │
           │ • Static HTTP TX Buffer (12 KB / 32 KB)                │
           │ • Static HTTP RX Buffer (12 KB)                        │
           │ • Temporary Ruleset Compiler Buffer (s_tempRules)      │
           │ • Volatile MQTT Scrubbing & Logging Buffers            │
0x10010000 └────────────────────────────────────────────────────────┘

0x20000000 ┌────────────────────────────────────────────────────────┐
           │ SRAM1 + SRAM2 (128 KB System Memory)                   │
           │ • FreeRTOS Kernel Data & Task Stacks                   │
           │ • DMA Peripheral Buffers (Modbus UART & SPI)           │
           │ • Dynamic CJSON Working Heaps                          │
0x20020000 └────────────────────────────────────────────────────────┘
```

### Why CCMRAM Matters:
The STM32F407 Core Coupled Memory (CCMRAM) connects directly to the D-bus with zero wait states. By mapping large static communication buffers (`rx_buf`, `tx_buf`, `s_tempRules`) into `.ccmram` via `__attribute__((section(".ccmram")))`, the main SRAM remains completely immune to exhaustion even when parsing 12 KB canvas layout JSON payloads.

---

## 3. External SPI NOR Flash Partitioning (Winbond W25Q16 - 2 MB Total)

| Partition | Base Address | Size | Sectors | Description |
|---|---|---|---|---|
| **Web Assets** | `0x00000000` | 512 KB | 0–127 | Gzipped HTML5/CSS3/JS Single-Page Application (41 KB compressed). |
| **Config: Primary** | `0x00080000` | 4 KB | Sector 128 | Primary `Gateway_Config_t` (Network, credentials, sensor table). |
| **Config: Backup** | `0x00081000` | 4 KB | Sector 129 | Backup configuration sector with CRC validation. |
| **Active Ruleset** | `0x00082000` | 4 KB | Sector 130 | Active compiled `RuleConfig_t` struct evaluated at 100 Hz. |
| **Backup Ruleset** | `0x00083000` | 4 KB | Sector 131 | Last-known-good stable ruleset for automatic rollback. |
| **Canvas Layout Blob** | `0x00084000` | 16 KB | Sectors 132–135 | Opaque JSON blob preserving desktop visual graph layout. |
| **Offline Telemetry Queue** | `0x00088000` | 496 KB | Sectors 136–235 | 31,744 entries (16 bytes each) FIFO circular ring buffer. |
| **Rules History Archives** | `0x000EC000` | 96 KB | Sectors 236–259 | 12 archived slots (8 KB each: 4KB rule + 4KB layout). |
| **System Audit Logs** | `0x00104000` | 1,008 KB | Sectors 260–511 | Wrap-around circular event logger for audit diagnostics. |

---

## 4. MicroSD Card Interface (SPI3)
* **Bus Speed**: Configured on SPI3 with DMA support.
* **Filesystem**: FATFS filesystem handling long filenames and ISO-8601 timestamps.
* **Streaming Engine**: `SDCard_Stream_Download` directly pipes file blocks into W5500 sockets in 1 KB chunks, completely bypassing internal RAM buffers.
