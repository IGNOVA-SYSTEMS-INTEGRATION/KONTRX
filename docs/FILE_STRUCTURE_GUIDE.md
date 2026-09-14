# Kontrx Codebase File-by-File Technical Guide

This document provides a comprehensive, exhaustive reference for every file in the **Kontrx Universal Industrial Controller & Edge Gateway** codebase (Firmware v2.0.0).

---

## Directory Tree Overview

```
Kontrx/
├── APP/                                 # Application Layer (Tasks, Protocol Handlers, DSP, Web Assets)
│   ├── bblink22.c                       # Basic LED blink diagnostic
│   ├── bootloader.c                     # Stage-1 Bootloader @ 0x08000000
│   ├── cJSON.c                          # Ultralight ANSI C JSON parser (FreeRTOS heap integrated)
│   ├── cJSON.h                          # cJSON interface header
│   ├── cmsis_os2.c                      # CMSIS-RTOS2 API compatibility layer over FreeRTOS
│   ├── cmsis_os2.h                      # CMSIS-RTOS2 interface header
│   ├── dsp_filter.c                     # DSP digital filtering (moving average, median filter, outlier rejection)
│   ├── dsp_filter.h                     # DSP filter prototypes and state structures
│   ├── freertos_tasks.c                 # RTOS Task manager, rule engine, SD card sync, network guard, MQTT client
│   ├── freertos_tasks.h                 # Task declarations, system state headers, CPU DWT measurement
│   ├── http_server_task.c               # REST API HTTP Server, Ignova SPA server, SD log pagination, OTA handler
│   ├── http_server_task.h               # HTTP task interface & OTA state structures
│   ├── main.c                           # Minimal test entry point
│   ├── main_ammonia_test.c              # Standalone Ammonia sensor test runner
│   ├── main_do_test.c                   # Standalone Dissolved Oxygen test runner
│   ├── main_ec_test.c                   # Standalone Electrical Conductivity test runner
│   ├── main_kontrx.c                    # Master Gateway Application Entry Point
│   ├── main_mqtt_sensors.c              # Standalone MQTT sensor publisher test
│   ├── main_orp_test.c                  # Standalone ORP sensor test runner
│   ├── main_ph_test.c                   # Standalone pH sensor test runner
│   ├── main_relay_chaser.c              # Standalone relay test runner
│   ├── main_rs485_diag_test.c           # Raw RS485 diagnostic tool
│   ├── main_w5500_test.c                # Standalone W5500 SPI Ethernet test runner
│   ├── ota_task.c                       # Background OTA firmware validator & writer
│   ├── ota_task.h                       # OTA task structures & flash layout
│   ├── sparkplug_b_enc.c                # Sparkplug B Protobuf binary serializer
│   ├── sparkplug_b_enc.h                # Sparkplug B encoder interface
│   └── web_assets.h                     # Embedded responsive SPA Dashboard (Light/Dark, QR, dynamic logs)
├── HAL/                                 # Hardware Abstraction Layer
│   ├── analog_010v.c                    # Industrial 0-10V analog voltage output driver
│   ├── analog_010v.h                    # 0-10V analog output interface
│   ├── dac_420ma.c                      # 4-20mA current loop DAC driver
│   ├── dac_420ma.h                      # 4-20mA current loop interface
│   ├── flash_partition.c               # W25Q16 Flash Partition Manager, redundant config/rules & ring buffers
│   ├── flash_partition.h               # Flash partition layout, structures & API
│   ├── interface_discovery.c            # Dynamic hardware peripheral capability scanner & register enumerator
│   ├── interface_discovery.h            # Hardware discovery interface
│   ├── led.c                           # Status LED driver (PA6)
│   ├── led.h                           # LED driver interface
│   ├── modbus.c                         # Legacy synchronous Modbus driver
│   ├── modbus.h                         # Legacy Modbus driver interface
│   ├── modbus_dma.c                     # Production Modbus RTU polling & auto-scan engine
│   ├── modbus_dma.h                     # Sensor structs, actuator configs, shared state & FW_VERSION definition
│   ├── modbus_tcp_server.c              # Modbus TCP Server running on Port 502 for SCADA/PLC integration
│   ├── modbus_tcp_server.h              # Modbus TCP Server interface and register mappings
│   ├── plc_control.c                    # Modbus TCP Client (Socket 4, FC5 Write Single Coil)
│   ├── plc_control.h                    # PLC control interface
│   ├── pto_motion.c                     # 4-channel Pulse Train Output (PTO) stepper/servo motor driver
│   ├── pto_motion.h                     # PTO motion controller API & state structures
│   ├── pwm_controller.c                 # Industrial multi-channel PWM controller (frequency & duty cycle)
│   ├── pwm_controller.h                 # PWM controller interface
│   ├── sdcard.c                         # MicroSD Card driver over SPI3 for multi-sector event & audit logging
│   ├── sdcard.h                         # MicroSD Card interface & error codes
│   ├── w25q16.c                         # Winbond W25Q16 2MB SPI NOR Flash driver
│   └── w25q16.h                         # W25Q16 command definitions & interface
├── MCAL/STM32F4/                        # Microcontroller Abstraction Layer (STM32F407)
│   ├── exti_stm32.c                     # External Interrupt (EXTI) driver for zero-latency limit switches
│   ├── exti_stm32.h                     # EXTI driver interface
│   ├── flash_stm32.c                    # Internal STM32 Flash sector erase & programming
│   ├── flash_stm32.h                    # Internal Flash interface
│   ├── gpio_stm32.c                     # GPIO setup, alternate functions, pin reservation validation
│   ├── gpio_stm32.h                     # GPIO port mapping & pin definitions
│   ├── rcc_stm32.c                      # Reset & Clock Control driver (PLL, HSI, HSE clock gates)
│   ├── rcc_stm32.h                      # RCC control macros and clock configuration
│   ├── rtc_stm32.c                      # Hardware Real-Time Clock & Unix epoch counter
│   ├── rtc_stm32.h                      # RTC interface
│   ├── spi_stm32.c                      # Register-level SPI1 (Flash) and SPI2 (W5500) master driver
│   ├── spi_stm32.h                      # SPI interface
│   ├── spi3_stm32.c                     # Register-level SPI3 master driver (dedicated to MicroSD storage)
│   ├── spi3_stm32.h                     # SPI3 interface
│   ├── stm32f407_regs.h                 # Bare-metal register definitions (RCC, GPIO, SPI, USART, TIM, EXTI)
│   ├── timer_stm32.c                    # General and Advanced Timer (TIM1) driver for PWM & PTO pulses
│   ├── timer_stm32.h                    # Timer driver interface
│   ├── uart_stm32.c                     # Register-level USART1 (Debug) & USART3 (Modbus)
│   ├── uart_stm32.h                     # UART interface
│   ├── uart2_stm32.c                    # Register-level USART2 driver for auxiliary expansion
│   └── uart2_stm32.h                    # USART2 interface
├── FreeRTOS/                            # FreeRTOS v10 Kernel Sources & ARM_CM4F Port
├── ioLibrary_Driver/                    # WIZnet W5500 Ethernet Driver Stack
├── docs/                                # Architecture Specifications & Guides
├── CMakeLists.txt                       # Master CMake build definition
├── arm_toolchain.cmake                  # Cross-compilation toolchain file
├── bootloader.ld                        # Linker script for Stage-1 Bootloader @ 0x08000000
├── app_ota.ld                           # Linker script for Application @ 0x08008000
├── stm32f407vet6.ld                     # Standard full-flash linker script
├── startup_stm32f407xx.c                # Reset vector table & interrupt handlers
├── FreeRTOSConfig.h                     # FreeRTOS kernel configuration
├── control_relays.py                    # Python utility to cycle/test relays via REST API
├── decode_sparkplug.py                  # Python utility to decode Sparkplug B Protobuf MQTT data
├── test_float_decode.py                 # TDD unit test verifying DCBA Little-Endian float decoding
└── test_sparkplug_and_mqtt.py           # Verification test suite for MQTT & Sparkplug B telemetry
```

---

## 1. Root Configuration & Build Files

### `CMakeLists.txt`
* **Purpose**: Master CMake configuration for the entire repository.
* **Key Targets Defined**:
  1. `Bootloader`: Stage-1 bootloader built with `bootloader.ld` (Flash origin `0x08000000`, 16 KB). Produces `Bootloader.bin`.
  2. `KontrxRTOS`: Production RTOS Universal Controller application built with `app_ota.ld` (Flash origin `0x08008000`, 224 KB). Produces `KontrxRTOS.bin` and `KontrxRTOS.hex`.
  3. `RS485_Diag`: Standalone raw-byte RS485 diagnostics tool. Produces `RS485_Diag.bin`.
* **Compiler Flags**: `-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Os -g -Wall -Wextra -ffunction-sections -fdata-sections -D_WIZCHIP_=W5500 -DUSE_FREERTOS`.
* **Linker Flags**: `-Wl,--gc-sections --specs=nosys.specs --specs=nano.specs -u _printf_float`.

### `arm_toolchain.cmake`
* **Purpose**: Cross-compilation toolchain configuration for GNU Arm Embedded Toolchain (`arm-none-eabi-gcc`, `arm-none-eabi-g++`, `arm-none-eabi-objcopy`, `arm-none-eabi-size`).

### `bootloader.ld`
* **Purpose**: Linker script for the Stage-1 Bootloader.
* **Memory Map**:
  - `FLASH (rx)`: `ORIGIN = 0x08000000, LENGTH = 16K`
  - `RAM (xrw)`: `ORIGIN = 0x20000000, LENGTH = 128K`

### `app_ota.ld`
* **Purpose**: Linker script for the production OTA-capable application (`KontrxRTOS`).
* **Memory Map**:
  - `FLASH (rx)`: `ORIGIN = 0x08008000, LENGTH = 224K`
  - `RAM (xrw)`: `ORIGIN = 0x20000000, LENGTH = 128K`
* **Execution**: Bootloader branches to `0x08008000` after vector table relocation (`SCB->VTOR = 0x08008000`).

### `stm32f407vet6.ld`
* **Purpose**: Monolithic 512 KB Flash linker script (`ORIGIN = 0x08000000, LENGTH = 512K`) for non-bootloader standalone debugging.

### `startup_stm32f407xx.c`
* **Purpose**: Vector table definition and startup code for Cortex-M4F.
* **Key Functions**:
  - `Reset_Handler()`: Copies `.data` to SRAM, zeros `.bss`, enables FPU Coprocessors (CP10 and CP11 Full Access via `SCB->CPACR`), and calls `main()`.

### `FreeRTOSConfig.h`
* **Purpose**: Kernel configuration file for FreeRTOS v10.
* **Key Parameters**:
  - `configCPU_CLOCK_HZ`: 16,000,000 Hz (HSI / SYSCLK).
  - `configTICK_RATE_HZ`: 1000 Hz (1 ms tick period).
  - `configTOTAL_HEAP_SIZE`: 44 KB (`heap_4.c` dynamic allocation).
  - `traceTASK_SWITCHED_IN()` / `traceTASK_SWITCHED_OUT()`: Measure exact CPU load percentage using ARM DWT cycle counter.

---

## 2. Application Layer (`APP/`)

### `APP/main_kontrx.c`
* **Purpose**: Master firmware entry point for the production `KontrxRTOS` target.
* **Boot Sequence**:
  1. Sets NVIC priority grouping (4 bits preemption via `SCB_AIRCR`).
  2. Initializes USART1 for 115200 baud debug output via `printf` (`_write` redirect).
  3. Initializes hardware RTC peripheral.
  4. Configures SPI2 and asserts W5500 hardware reset.
  5. Creates recursive `spiMutex` to guard W5500 transactions.
  6. Configures static IP network parameters (`192.168.1.200`, Subnet `255.255.255.0`, Gateway `192.168.1.1`, DNS `8.8.8.8`).
  7. Initializes status LED (PA6).
  8. Initializes Partition Manager (`Partition_Init`), loads configuration from W25Q16 Flash.
  9. Initializes relay GPIO pins, PTO motion channels, PWM outputs, and MicroSD card.
  10. Initializes ARM DWT cycle counter (`KontrxDWT_Init`).
  11. Calls `RTOS_Tasks_Init()` to launch FreeRTOS scheduler.

### `APP/freertos_tasks.c` & `APP/freertos_tasks.h`
* **Purpose**: Manages task lifecycle, task synchronization, circular audit logging, dynamic rule execution, SD card event synchronization, network guard, and MQTT client communication.
* **Tasks Spawned**:
  1. `Task_ControlEngine` (Priority 5, Stack 6KB, 100 Hz / 10 ms cycle): Evaluates 16 user rules against live sensor data in RAM, actuates relays, PTO motion, PWM, and Modbus TCP client.
  2. `Task_ModbusSensorPoll` (Priority 3, Stack 4KB, Continuous): Performs noise-filtered Modbus RTU queries and DSP signal filtering.
  3. `Task_HTTPServer` (Priority 2, Stack 12KB, Event-driven): Handles REST API, streams Ignova SPA, and serves SD card logs.
  4. `Task_OTAUpdate` (Priority 1, Stack 4KB, Event-driven): Performs CRC32 validation and stages firmware.
  5. `Task_MQTTClient` (Priority 4, Stack 8KB, Periodic / CoV): Connects to MQTT broker, manages Sparkplug B Protobuf / Dynamic JSON payloads, and flushes 512KB offline queue.
* **Core Functions**:
  - `Ensure_W5500_Network_Alive()`: Self-healing watchdog that verifies physical link and re-applies static IP upon loss or link flap.
  - `Log_Event(category, message)`: Writes timestamped events into RAM buffer, persists to multi-sector SPI Flash and synchronizes to MicroSD card.
  - `Relay_SetState(idx, state)`: Actuates local GPIO pins, Modbus TCP coils, or remote endpoints instantly without blocking.

### `APP/dsp_filter.c` & `APP/dsp_filter.h`
* **Purpose**: Digital signal processing pipeline for industrial RS485 sensors.
* **Techniques Implemented**:
  - Windowed moving average for high-frequency noise smoothing.
  - Median filter for transient spike elimination.
  - Outlier rejection verifying rate-of-change thresholds.

### `APP/http_server_task.c` & `APP/http_server_task.h`
* **Purpose**: Embedded HTTP REST Server operating on W5500 Socket 0, Port 80.
* **Key Endpoints Handled**:
  - `GET /` & `GET /index.html`: Streams embedded Ignova SPA dashboard.
  - `GET /api/status`: Returns comprehensive JSON snapshot of live sensors, relay states, network settings, system health, and QR data payload.
  - `GET /api/logs`: Returns paginated audit logs with dynamic scroll support.
  - `POST /api/relay` & `POST /api/relay/all`: Controls actuators.
  - `POST /api/config/relays`, `POST /api/config/mqtt`, `POST /api/config/sensors`: Configuration updates.
  - `POST /api/modbus/scan` & `GET /api/modbus/scan`: Modbus 1-247 slave auto-discovery scanner.
  - `GET /api/rules` & `POST /api/rules`: Rule engine configuration and probationary verification.
  - `POST /api/ota/verify`, `POST /api/ota/prepare`, `POST /update`: Chunked OTA upload with OTP authentication (`KontrxOTA2026`).

### `APP/sparkplug_b_enc.c` & `APP/sparkplug_b_enc.h`
* **Purpose**: Zero-heap binary Protobuf serializer implementing Eclipse Sparkplug B.
* **Key Functions**:
  - `sparkplug_encode_nbirth()`: Generates Node Birth message with hardware metadata and initial state.
  - `sparkplug_encode_ddata()`: Generates Device Data payload encoding sample-averaged sensor metrics and actuator states.
  - `sparkplug_encode_ndeath()`: Generates Node Death payload used as MQTT Last Will and Testament (LWT).

### `APP/ota_task.c` & `APP/ota_task.h`
* **Purpose**: Background task responsible for validating staged firmware images and preparing the system for bootloader swap.

### `APP/bootloader.c`
* **Purpose**: Stage-1 Bare-Metal Bootloader running at Flash origin `0x08000000`. Copies staged application binary from Sector 6 to Sector 2, verifies CRC, and jumps to application.

### `APP/cJSON.c` & `APP/cJSON.h`
* **Purpose**: Lightweight ANSI C JSON parser mapped to FreeRTOS heap memory hooks (`pvPortMalloc`, `vPortFree`).

### `APP/cmsis_os2.c` & `APP/cmsis_os2.h`
* **Purpose**: ARM CMSIS-RTOS v2 API wrapper around FreeRTOS kernel objects.

### `APP/web_assets.h`
* **Purpose**: Contains the compiled responsive SPA Dashboard (`KONTRX_HTML`).
* **Features**:
  - Ignova Corporate Brand styling (Light mode default, Dark mode toggle).
  - Dynamic QR Code generator (v1–40 byte-mode engine with Reed-Solomon error correction).
  - Dynamic log viewer with infinite scroll pagination, "Load Next 40", and "Load All" controls.
  - Real-time sensor charts, relay toggles, motion / PWM control panels, and rule visualizer.

---

## 3. Hardware Abstraction Layer (`HAL/`)

### `HAL/modbus_dma.c` & `HAL/modbus_dma.h`
* **Purpose**: Core Modbus RTU communication engine driving USART3 and MAX485 transceiver.
* **Key Features**:
  - Noise rejection state machine with timeout-per-sensor scheduling (40 ms for fast sensors, 150 ms for KWS-630 DO).
  - Automatic direction control for MAX485 (`MAX485_TX()` / `MAX485_RX()`).
  - Standard Modbus CRC-16 polynomial generator (`0xA001`).
  - IEEE-754 Little-Endian DCBA float decoder for KWS-630 DO sensor.
  - Support for 7 sensor types: pH, ORP, EC, DO, Ammonia, Single Ultrasonic, and Multi-Point Ultrasonic Array.
  - Background 1-247 slave auto-discovery scan.
  - Defines single-source-of-truth `FW_VERSION "2.0.0"`.

### `HAL/modbus_tcp_server.c` & `HAL/modbus_tcp_server.h`
* **Purpose**: Modbus TCP Server running on Port 502.
* **Capabilities**: Allows remote SCADA masters and PLCs to read discrete inputs, coils, input registers, and holding registers representing live sensors and actuator states.

### `HAL/pto_motion.c` & `HAL/pto_motion.h`
* **Purpose**: 4-channel Pulse Train Output (PTO) motion engine for stepper and servo motors.
* **Capabilities**: Generates high-speed pulse trains on TIM1 channels (`PE9`, `PE11`, `PE13`, `PE14`), drives direction pins (`PE8`, `PE10`, `PE12`, `PE15`), and responds to hardware limit switches (`PE0`, `PE1`, `PE3`, `PE7`) via EXTI interrupts.

### `HAL/pwm_controller.c` & `HAL/pwm_controller.h`
* **Purpose**: Multi-channel industrial PWM driver with dynamic frequency and duty cycle control.

### `HAL/analog_010v.c` & `HAL/analog_010v.h`
* **Purpose**: 0-10V analog voltage output interface driver for variable speed drives and actuators.

### `HAL/dac_420ma.c` & `HAL/dac_420ma.h`
* **Purpose**: 4-20mA industrial current loop DAC transmitter driver.

### `HAL/sdcard.c` & `HAL/sdcard.h`
* **Purpose**: MicroSD card SPI driver over SPI3. Provides multi-sector persistent event logging, audit trails, and file system export.

### `HAL/flash_partition.c` & `HAL/flash_partition.h`
* **Purpose**: Partition manager for the external Winbond W25Q16 (2 MB SPI NOR Flash).
* **Partitions Managed**:
  1. `PARTITION_WEB_ADDR` (`0x00000000`, 512 KB): Web Assets.
  2. `PARTITION_CONFIG_ADDR` (`0x00080000`, 16 KB): Dual-sector redundant system config & rules.
  3. `PARTITION_QUEUE_ADDR` (`0x00084000`, 512 KB): Offline telemetry FIFO ring buffer (32,768 slots).
  4. `PARTITION_LOG_ADDR` (`0x00104000`, 1008 KB): Multi-sector circular event audit log buffer.

### `HAL/interface_discovery.c` & `HAL/interface_discovery.h`
* **Purpose**: Dynamic hardware capability scanner and interface auto-discovery module.

### `HAL/plc_control.c` & `HAL/plc_control.h`
* **Purpose**: Modbus TCP Client (Socket 4, FC5 Write Single Coil) for controlling remote PLCs over Ethernet.

### `HAL/w25q16.c` & `HAL/w25q16.h`
* **Purpose**: Low-level SPI driver for Winbond W25Q16 16 Mbit (2 MB) NOR Flash.

### `HAL/led.c` & `HAL/led.h`
* **Purpose**: Board status LED driver on GPIO PA6.

---

## 4. Microcontroller Abstraction Layer (`MCAL/STM32F4/`)

### `MCAL/STM32F4/stm32f407_regs.h`
* **Purpose**: Complete bare-metal register definitions and bitmask macros for STM32F407VET6 peripherals (RCC, GPIO, SPI, USART, TIM, EXTI, SYSTICK).

### `MCAL/STM32F4/gpio_stm32.c` & `MCAL/STM32F4/gpio_stm32.h`
* **Purpose**: Register-level GPIO initialization, push-pull configuration, alternate function multiplexing, and port resolution.

### `MCAL/STM32F4/uart_stm32.c` & `MCAL/STM32F4/uart_stm32.h`
* **Purpose**: Register-level UART driver for USART1 (Debug 115200 baud) and USART3 (Modbus 9600 baud).

### `MCAL/STM32F4/uart2_stm32.c` & `MCAL/STM32F4/uart2_stm32.h`
* **Purpose**: Register-level USART2 driver for auxiliary serial interfaces.

### `MCAL/STM32F4/spi_stm32.c` & `MCAL/STM32F4/spi_stm32.h`
* **Purpose**: Register-level SPI1 (Flash) and SPI2 (W5500 @ 21 MHz) master driver.

### `MCAL/STM32F4/spi3_stm32.c` & `MCAL/STM32F4/spi3_stm32.h`
* **Purpose**: Register-level SPI3 master driver dedicated to high-capacity MicroSD card storage.

### `MCAL/STM32F4/timer_stm32.c` & `MCAL/STM32F4/timer_stm32.h`
* **Purpose**: Hardware timer driver configuring TIM1 Advanced Control Timer for PTO motion pulses and general-purpose timers for PWM.

### `MCAL/STM32F4/exti_stm32.c` & `MCAL/STM32F4/exti_stm32.h`
* **Purpose**: External interrupt (EXTI) configuration driver for limit switches and emergency stop signals.

### `MCAL/STM32F4/rcc_stm32.c` & `MCAL/STM32F4/rcc_stm32.h`
* **Purpose**: Reset & Clock Control driver managing PLL, HSI/HSE crystals, and peripheral clock gating.

### `MCAL/STM32F4/flash_stm32.c` & `MCAL/STM32F4/flash_stm32.h`
* **Purpose**: Internal STM32 Flash memory controller for unlocking, sector erasing, and word programming.

### `MCAL/STM32F4/rtc_stm32.c` & `MCAL/STM32F4/rtc_stm32.h`
* **Purpose**: Hardware RTC driver with backup domain unlocking and Unix epoch counter conversion.

---

## 5. Python Utility Scripts & Test Suites

### `control_relays.py`
* **Purpose**: Automated testing script that toggles Kontrx relays via HTTP REST API (`/api/relay` and `/api/relay/all`).

### `decode_sparkplug.py`
* **Purpose**: MQTT listener script that subscribes to `spBv1.0/#`, parses incoming Sparkplug B binary Protobuf payloads, and displays decoded metrics.

### `test_float_decode.py`
* **Purpose**: TDD test suite validating IEEE-754 Little-Endian DCBA float conversion against big-endian BADC format for KWS-630 DO sensor registers.

### `test_sparkplug_and_mqtt.py`
* **Purpose**: End-to-end integration test validating MQTT publishing, dynamic payload shapes, and Sparkplug B Protobuf decoding.
