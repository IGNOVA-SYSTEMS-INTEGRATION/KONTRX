# Feature: Hardware Interface & Peripheral Discovery Engine

## 1. Description & Purpose (بتعمل ايه وليه اتعملت)
Provides unified runtime discovery, status interrogation, and capacity reporting across all 12 on-board hardware interfaces and fieldbus industrial protocols.
* **Why it was built**: Modular automation installations contain various optional daughterboards (e.g. EtherCAT or Profibus ASICs) and variable wiring configurations. Instead of requiring hardcoded firmware builds for each hardware combination, this subsystem discovers active controllers, checks silicon IDs, counts active channels, and reports this metadata to supervisory SCADA and the web dashboard.

---

## 2. Technical Implementation & How It Works (بتشتغل ازاي)
* **Code Location**: `HAL/interface_discovery.c`, `HAL/interface_discovery.h`, `APP/http_server_task.c`.
* **Enumerated Interfaces (12 Subsystems)**:
  1. **Industrial Ethernet**: WIZnet W5500 via SPI2 @ 21 MHz (8 hardware sockets).
  2. **EtherCAT Slave**: Beckhoff ET1100 expansion slot.
  3. **PROFINET IO Device**: Hilscher netX expansion slot.
  4. **RS485 Bus 1**: Modbus RTU / BACnet via MAX485 on USART3.
  5. **RS485 Bus 2**: Auxiliary fieldbus via MAX485 on USART2.
  6. **Profibus DP**: VPC3+C / SPC3 ASIC expansion slot.
  7. **PTO Motion Control**: 4 Axes (TIM1, TIM9, TIM3, TIM2 + EXTI limit switches).
  8. **PWM Outputs**: 7 Channels proportional outputs (TIM4, TIM10, TIM11, TIM14).
  9. **Analog Current Loop**: 8 Channels 4–20mA via SPI3 MCP4922 DAC.
  10. **Analog Voltage**: 2 Channels 0–10V DC via PWM + 2-pole active low-pass filter.
  11. **Modbus TCP Server**: Listening on Port 502 (W5500 Socket 2).
  12. **BACnet/IP Gateway**: Ready on UDP Port 47808 (W5500 Socket 3).
* **Silicon Health Probing**:
  * Probes W5500 register `getVERSIONR()` (verifies `0x04`). If unreachable or desoldered, marks interface as `IF_STATUS_WAITING_ASIC` and falls back gracefully.
  * Formats state dynamically into JSON via `Interface_BuildArrayJSON()` and serves it through `/api/interfaces`.

---

## 3. Tests Performed & Verification (ايه الـ Tests اللي اتعملت عليها وازاي)
* **Automated Python Discovery Test**:
  * File: `tests/test_http_api.py`
  * Execution:
    ```bash
    pytest tests/test_http_api.py -k "interfaces" -v
    ```
  * What it verifies: JSON schema compliance, 12 interfaces represented, valid status enums (`0`–`3`), and channel count arithmetic.
* **Hot-Plug / ASIC Absence Emulation**:
  * Tested boot with unpopulated EtherCAT/Profibus slots; verified controller sets status `WAITING_ASIC` without panicking or halting execution.
* **Web Dashboard Rendering**:
  * Verified `/interfaces` tab on the embedded SPA renders color-coded interface cards showing live channel usage.

---

## 4. System Impact & Inter-Dependencies (بتاثر علي ايه ومرتبطه بايه)
* **Exposed Interfaces**:
  * Queried by `APP/http_server_task.c` during `/api/interfaces` and `/api/status`.
  * Queried by Desktop Rule Configurator to constrain draggable node types on the canvas based on active physical hardware.
* **Dependencies**:
  * Depends on low-level SPI and UART register health.

---

## 5. Development Status & Blockers (حالتها والمعوقات)
* **Status**: ✅ **COMPLETED & OPERATIONAL (مكتملة ومدمجة في لوحة التحكم)**.
* **Blockers / Known Gaps**: None. EtherCAT and Profibus remain marked `WAITING_ASIC` until modular add-on hardware is installed.
