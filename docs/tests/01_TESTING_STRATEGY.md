# Testing Strategy & Quality Assurance Framework

## 1. Overview
KONTRX implements a multi-tier testing framework designed for critical embedded industrial automation where firmware regressions can lead to physical hardware damage or water quality failures.

```
       +---------------------------------------------+
       |   Tier 4: Hardware-in-the-Loop (HIL)        |  Live RS485 bus, real sensors,
       |           & Electrical Stress Tests          |  oscilloscope pulse checks
       +---------------------------------------------+
       |   Tier 3: Device Integration Tests (Pytest) |  Live HTTP REST APIs, W5500 sockets,
       |           over Ethernet/Local Network       |  OTA payload staging validation
       +---------------------------------------------+
       |   Tier 2: Host C Logic & DSP Unit Tests     |  gcc-compiled isolated algorithmic
       |           (Mocked Hardware Abstractions)    |  logic, hysteresis, Sparkplug B Protobuf
       +---------------------------------------------+
       |   Tier 1: Static Analysis & Compiler Gates  |  ARM GCC -Wall -Wextra, zero warnings,
       |           Linker Section & CCMRAM Sizing    |  Static assertions for flash sectors
       +---------------------------------------------+
```

---

## 2. Testing Levels & Environments

### Tier 1: Static Compiler Verification & Section Assertion
* **Toolchain**: `arm-none-eabi-gcc` via CMake / Ninja.
* **Scope**:
  * Compile with `-Wall -Wextra -Werror` compliance across `APP/`, `HAL/`, and `MCAL/`.
  * Validate `_Static_assert(sizeof(RuleConfig_t) <= 4096)` to guarantee rule sets fit within a single flash sector.
  * Verify memory allocation within linker scripts (`STM32F407VETx_FLASH.ld`):
    * FLASH limit: 512 KB
    * RAM (SRAM1) limit: 112 KB
    * CCMRAM limit: 64 KB

### Tier 2: Host-Side Pure Logic & Protocol Unit Tests
* **Environment**: Runs directly on host workstation (Windows / Linux) without physical STM32 hardware.
* **Scope**:
  * Modbus RTU CRC16 algorithms, DCBA float conversion, frame timeout bounds.
  * Automation rule state machines, hysteresis windows, and condition trees (`tests/test_config_logic.c`).
  * Web asset gzip integrity and byte array sizing (`tests/test_web_assets.py`).
  * QR code onboarding payload generator (`tests/test_qr_payload.py`).

### Tier 3: Network & API Integration Tests
* **Environment**: Runs against a real controller or QEMU/network simulator via Ethernet.
* **Scope**:
  * REST API endpoint compliance (`tests/test_http_api.py`).
  * Session cookie authentication, CSRF/login flows, and unauthorized lockout.
  * Actuator state assertion across Relays, PWM, PTO, 0–10V, and 4–20mA (`tests/test_actuators.py`).
  * Sparkplug B Protobuf topic structure and MQTT broker connectivity (`tests/test_mqtt_and_ui.py`).

### Tier 4: Hardware-in-the-Loop (HIL) & Stress Testing
* **Environment**: Physical test rack equipped with STM32F407 board, W5500 SPI Ethernet module, MAX485 transceivers, oscilloscope, and active Modbus sensors.
* **Scope**:
  * EMI noise resilience and DSP moving average response.
  * Sudden power brownouts during SPI NOR flash write cycles.
  * Dual-bank OTA firmware update rollback and bootloader handoff.

---

## 3. Pre-Commit Verification Gate
Before merging any feature branch into `main`, the following mandatory verification steps must pass:
1. `cmake --build build` (zero errors, zero linker warnings).
2. `pytest tests/test_modbus_protocol.py tests/test_web_assets.py tests/test_actuators.py -v`.
3. Firmware binary size check (`arm-none-eabi-size build/KontrxRTOS.elf`).
