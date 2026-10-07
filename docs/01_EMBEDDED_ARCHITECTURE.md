# 01 - KONTRX Embedded Architecture & FreeRTOS Task Matrix

## 1. Executive Summary
KONTRX is an industrial-grade Edge IoT Gateway and Automation Controller based on the STMicroelectronics STM32F407VET6/ZGT6 (ARM Cortex-M4 with FPU running at 168 MHz). It delivers deterministic real-time control, protocol translation, and edge computing for mission-critical industrial plants, water treatment facilities, and smart aquaculture installations.

---

## 2. Hardware Foundations
* **MCU**: STM32F407 (ARM Cortex-M4, 168 MHz, 1024 KB Flash, 128 KB SRAM + 64 KB Core Coupled Memory CCMRAM).
* **Ethernet**: WIZnet W5500 SPI Hardwired TCP/IP controller with 32 KB internal buffer.
* **Non-Volatile Storage**: Winbond W25Q16 (16 Mbit / 2 MB SPI NOR Flash) partitioned for configs, rules, layout, offline queue, and audit logs.
* **Secondary Storage**: MicroSD Card over SPI / SDIO supporting FATFS for high-capacity log rotation.
* **RS485 Transceiver**: MAX485 / SP3485 connected to USART2 with DMA support for continuous Modbus RTU sensor polling.

---

## 3. Memory Architecture: Why CCMRAM Was Chosen
The STM32F407 contains 64 KB of Core Coupled Memory (CCMRAM) accessible at zero-wait-states via the D-bus:
```text
┌────────────────────────────────────────────────────────┐
│               STM32F407 Memory Layout                  │
├───────────────────────────────┬────────────────────────┤
│ SRAM1 + SRAM2 (128 KB)        │ CCMRAM (64 KB @ 0x10000000)
│ • FreeRTOS Kernel & Stacks    │ • Large HTTP RX/TX Buffers
│ • Modbus Sensor Cache         │ • Full Ruleset Working Buffers
│ • Peripheral DMA Descriptors  │ • Telemetry Batch Serialization
└───────────────────────────────┴────────────────────────┘
```
### Technical Rationale:
Large HTTP POST payloads (such as 16-actuator configuration JSONs or dense canvas rule graphs up to 12 KB) would cause severe heap fragmentation or exhaust the 128 KB general SRAM. By placing static communication buffers (`rx_buf`, `tx_buf`, `s_tempRules`) in CCMRAM via `__attribute__((section(".ccmram")))`, the main SRAM remains completely unburdened and immune to stack overflow.

---

## 4. FreeRTOS Task Priority & Allocation Matrix

| Task Name | Priority | Stack Size | Period / Trigger | Description |
|---|---|---|---|---|
| `Task_Modbus_Poller` | `tskIDLE_PRIORITY + 4` (High) | 1024 words | 100 ms | Polls RS485 sensors via DMA, calculates averages, and updates shared state. |
| `Task_Control_Engine` | `tskIDLE_PRIORITY + 3` (Medium-High) | 1536 words | 100 ms | Evaluates hierarchical condition trees, timers, latches, and executes actuator outputs. |
| `Task_HTTPServer` | `tskIDLE_PRIORITY + 2` (Medium) | 2048 words | Event-driven | Handles HTTP REST API, web assets streaming, configuration changes, and OTA uploads. |
| `Task_MQTTClient` | `tskIDLE_PRIORITY + 2` (Medium) | 2048 words | Periodic / CoV | Maintains broker connection, publishes Sparkplug B payloads, drains offline queue. |
| `Task_Diagnostics` | `tskIDLE_PRIORITY + 1` (Low) | 512 words | 1000 ms | Feeds hardware watchdog (IWDG), monitors heap/stack high-water marks. |

---

## 5. Fail-Safe & Watchdog Strategy
1. **Independent Hardware Watchdog (IWDG)**: Configured with a 3.2-second timeout window.
2. **Task Health Check-In**: The diagnostic task checks that both the Modbus Poller and Control Engine have completed loops within expected time slices before kicking the IWDG.
3. **Safe State on Boot**: All PTO stepper motion channels, PWM channels, and relay pins are explicitly initialized to their safe/disabled states prior to FreeRTOS scheduler launch.
