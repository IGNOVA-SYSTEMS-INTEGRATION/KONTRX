# 02 - KONTRX Hardware Abstraction Layer (HAL) & Peripherals Guide

## 1. Overview
The Hardware Abstraction Layer isolates platform-specific silicon details from the high-level business logic, FreeRTOS tasks, and web services.

---

## 2. SPI NOR Flash Partitioning (W25Q16 - 2 MB Total)
The 2 MB flash chip is divided into deterministic sectors:

| Partition Name | Start Address | Size | Sectors | Purpose |
|---|---|---|---|---|
| **Bootloader Config** | `0x00000000` | 4 KB | Sector 0 | Boot flags, OTA status, firmware validation markers. |
| **Gateway Config** | `0x00001000` | 16 KB | Sectors 1–4 | Network IP, MQTT credentials, serial number, sensor map. |
| **Active Ruleset** | `0x00005000` | 4 KB | Sector 5 | Active compiled binary ruleset (`RuleConfig_t`). |
| **Rules Backup** | `0x00006000` | 4 KB | Sector 6 | Last-known-good stable ruleset for automatic rollback. |
| **Canvas Layout Blob** | `0x00084000` | 16 KB | Sectors 132–135 | Opaque JSON blob preserving desktop visual graph coordinates. |
| **Rules History Archives** | `0x000EC000` | 96 KB | Sectors 236–259 | 12 archived historical ruleset slots (8 KB each: 4KB rule + 4KB layout). |
| **Offline Telemetry Queue** | `0x00088000` | 496 KB | Sectors 136–259 | Ring buffer for unsent offline telemetry records (31,744 entries). |
| **Audit Log Partition** | `0x00104000` | 1008 KB | Sectors 260–511 | Wrap-around system events and error log records. |

---

## 3. Peripheral Hardware Safety
### Critical Pin Protection Mask:
To prevent user misconfiguration or web UI manipulation from compromising system-critical communication buses, the GPIO actuator engine enforces a hardware pin blacklist:
* **Port B Blacklist**:
  * `PB0`: W25Q16 Flash Chip Select (`CS`).
  * `PB3`, `PB4`, `PB5`: SPI1 SCK, MISO, MOSI (Flash Bus).
  * `PB10`, `PB11`: USART3 / Modbus RS485.
  * `PB12`–`PB15`: SPI2 Bus (W5500 Ethernet Controller).

Attempting to register any actuator on these pins is rejected at the API layer and blocked inside `freertos_tasks.c`.

---

## 4. Actuator Subsystem Matrix
KONTRX v2.2+ supports multi-domain actuator types:

1. **Local GPIO Relays (0–8)**:
   * Isolated optocoupler-driven digital outputs mapped to `PE2`, `PE4`, `PE6`, `PC0`, `PC2`, `PA1`, `PA0`, `PA2`.
2. **PWM Frequency & Duty Channels**:
   * TIM4 Channels 1 & 2 mapped to `PD12` and `PD13` (0–100% duty cycle, configurable up to 20 kHz for VFDs and variable fans).
3. **PTO (Pulse Train Output) Stepper Motion**:
   * High-speed directional pulse generation via TIM1 (`PE9`/`PE8`), TIM9, TIM3, and TIM2.
   * Step counting managed deterministically via dedicated NVIC update interrupts (`NVIC_ISER[0]`).
4. **Analog 0–10V Output**:
   * Industrial 0–10V DC control for proportional valves and variable frequency drives.
5. **Analog 4–20mA Current Loop Output**:
   * Standard 2-wire industrial current loop for chemical dosing pumps and precision positioning.
