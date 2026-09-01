# Kontrx Codebase File-by-File Technical Guide

This document provides a comprehensive, exhaustive reference for every file in the **Kontrx Edge Gateway** codebase.

---

## Directory Tree Overview

```
Kontrx/
├── APP/                                 # Application Layer (Tasks, Protocol Handlers, Web Assets)
│   ├── bblink22.c                       # Basic LED blink diagnostic
│   ├── bootloader.c                     # Stage-1 Bootloader @ 0x08000000
│   ├── cJSON.c                          # Ultralight ANSI C JSON parser
│   ├── cJSON.h                          # cJSON interface header
│   ├── cmsis_os2.c                      # CMSIS-RTOS2 API compatibility layer
│   ├── cmsis_os2.h                      # CMSIS-RTOS2 interface header
│   ├── freertos_tasks.c                 # RTOS Task manager, rule engine & MQTT client
│   ├── freertos_tasks.h                 # Task declarations & shared state headers
│   ├── http_server_task.c               # REST API HTTP Server & Web Asset server
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
│   └── web_assets.h                     # Embedded Dark-Themed SPA Dashboard & QR generator
├── HAL/                                 # Hardware Abstraction Layer
│   ├── flash_partition.c               # W25Q16 Flash Partition Manager & Ring Queues
│   ├── flash_partition.h               # Flash partition layout & data structures
│   ├── led.c                           # Status LED driver (PA6)
│   ├── led.h                           # LED driver interface
│   ├── modbus.c                         # Legacy synchronous Modbus driver
│   ├── modbus.h                         # Legacy Modbus driver interface
│   ├── modbus_dma.c                     # Production Modbus RTU polling & auto-scan engine
│   ├── modbus_dma.h                     # Shared data structures, sensor & config schemas
│   ├── plc_control.c                    # Modbus TCP (Socket 4) & OPC UA Client
│   ├── plc_control.h                    # PLC control interface
│   ├── w25q16.c                         # Winbond W25Q16 2MB SPI NOR Flash driver
│   └── w25q16.h                         # W25Q16 command definitions & interface
├── MCAL/STM32F4/                        # Microcontroller Abstraction Layer (STM32F407)
│   ├── flash_stm32.c                    # Internal STM32 Flash sector erase & programming
│   ├── flash_stm32.h                    # Internal Flash interface
│   ├── gpio_stm32.c                     # GPIO setup, alternate functions, pin validation
│   ├── gpio_stm32.h                     # GPIO port mapping & pin definitions
│   ├── rtc_stm32.c                      # Hardware Real-Time Clock & epoch counter
│   ├── rtc_stm32.h                      # RTC interface
│   ├── spi_stm32.c                      # Register-level SPI2 driver (W5500 & W25Q16)
│   ├── spi_stm32.h                      # SPI interface
│   ├── stm32f407_regs.h                 # Bare-metal register definitions (RCC, GPIO, SPI, USART, DWT)
│   ├── uart_stm32.c                     # Register-level USART1 (Debug) & USART3 (Modbus)
│   └── uart_stm32.h                     # UART interface
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
└── test_float_decode.py                 # TDD unit test verifying DCBA Little-Endian float decoding
```

---

## 1. Root Configuration & Build Files

### `CMakeLists.txt`
* **Purpose**: Master CMake configuration for the entire repository.
* **Key Targets Defined**:
  1. `Bootloader`: Stage-1 bootloader built with `bootloader.ld` (Flash origin `0x08000000`, 16 KB). Produces `Bootloader.bin`.
  2. `KontrxRTOS`: Production RTOS Gateway application built with `app_ota.ld` (Flash origin `0x08008000`, 224 KB). Produces `KontrxRTOS.bin` and `KontrxRTOS.hex`.
  3. `Kontrx`: Legacy bare-metal non-RTOS test build. Produces `Kontrx.bin`.
  4. `RS485_Diag`: Standalone raw-byte RS485 diagnostics tool. Produces `RS485_Diag.bin`.
* **Compiler Flags**: `-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Os -g -Wall -Wextra -D_WIZCHIP_=W5500 -DUSE_FREERTOS`.
* **Linker Flags**: `-Wl,--gc-sections --specs=nosys.specs --specs=nano.specs -u _printf_float`.

### `arm_toolchain.cmake`
* **Purpose**: Cross-compilation toolchain configuration for GNU Arm Embedded Toolchain (`arm-none-eabi-gcc`, `arm-none-eabi-g++`, `arm-none-eabi-objcopy`, `arm-none-eabi-size`).

### `bootloader.ld`
* **Purpose**: Linker script for the Stage-1 Bootloader.
* **Memory Map**:
  - `FLASH (rx)`: `ORIGIN = 0x08000000, LENGTH = 16K`
  - `RAM (xrw)`: `ORIGIN = 0x20000000, LENGTH = 128K`
* **Sections**: Maps `.isr_vector`, `.text`, `.rodata` into Flash Sector 0, and initializes `.data` and `.bss` in SRAM.

### `app_ota.ld`
* **Purpose**: Linker script for the production OTA-capable application (`KontrxRTOS`).
* **Memory Map**:
  - `FLASH (rx)`: `ORIGIN = 0x08008000, LENGTH = 224K`
  - `RAM (xrw)`: `ORIGIN = 0x20000000, LENGTH = 128K`
* **Execution**: Allows the bootloader to jump to address `0x08008000` after vector table relocation (`SCB->VTOR = 0x08008000`).

### `stm32f407vet6.ld`
* **Purpose**: Standard monolithic linker script using full 512 KB Flash (`ORIGIN = 0x08000000, LENGTH = 512K`). Used for standalone single-binary firmware testing.

### `startup_stm32f407xx.c`
* **Purpose**: Vector table definition and startup code for Cortex-M4F.
* **Key Functions**:
  - `Reset_Handler()`: Copies `.data` from Flash to RAM, clears `.bss` to zero, enables FPU Coprocessors (CP10 and CP11 Full Access via `SCB->CPACR`), calls `main()`.
  - Default interrupt handlers with weak aliases (`NMI_Handler`, `HardFault_Handler`, `MemManage_Handler`, `BusFault_Handler`, `UsageFault_Handler`).

### `FreeRTOSConfig.h`
* **Purpose**: Kernel configuration file for FreeRTOS v10.
* **Key Parameters**:
  - `configCPU_CLOCK_HZ`: 16,000,000 Hz (HSI / SYSCLK).
  - `configTICK_RATE_HZ`: 1000 Hz (1 ms tick period).
  - `configTOTAL_HEAP_SIZE`: 44 KB (`heap_4.c` dynamic allocation).
  - `configCHECK_FOR_STACK_OVERFLOW`: 2 (Method 2: Stack pattern check).
  - `traceTASK_SWITCHED_IN()` / `traceTASK_SWITCHED_OUT()`: Hooks into task context switches to measure exact CPU load using ARM DWT cycle counter.

---

## 2. Application Layer (`APP/`)

### `APP/main_kontrx.c`
* **Purpose**: Master firmware entry point for the production `KontrxRTOS` target.
* **Boot Sequence**:
  1. Sets NVIC priority grouping (4 bits preemption via `SCB_AIRCR`).
  2. Initializes USART1 for 115200 baud debug output via `printf` (`_write` redirect).
  3. Initializes RTC peripheral.
  4. Configures SPI2 and asserts W5500 hardware reset.
  5. Creates recursive `spiMutex` to prevent SPI bus collisions between HTTP and MQTT tasks.
  6. Registers WIZnet callbacks (`reg_wizchip_cris_cbfunc`, `reg_wizchip_cs_cbfunc`, `reg_wizchip_spi_cbfunc`).
  7. Performs W5500 software reset and verifies `VERSIONR == 0x04`.
  8. Configures static IP network parameters (`192.168.1.200`, Subnet `255.255.255.0`, Gateway `192.168.1.1`, DNS `8.8.8.8`).
  9. Initializes status LED (PA6).
  10. Creates RTOS mutexes (`sensorMutex`, `configMutex`).
  11. Initializes Partition Manager (`Partition_Init`) and loads configuration from external W25Q16 Flash (`Partition_LoadConfig`).
  12. Verifies and extracts embedded Web Assets from `web_assets.h` into W25Q16 Flash Sector 0.
  13. Initializes relay GPIO pins from active configuration (`Relay_Init`).
  14. Initializes ARM DWT cycle counter (`KontrxDWT_Init`).
  15. Calls `RTOS_Tasks_Init()` to launch FreeRTOS scheduler.

### `APP/freertos_tasks.c` & `APP/freertos_tasks.h`
* **Purpose**: Manages task lifecycle, task synchronization, circular audit logging, dynamic rule execution, and MQTT / Sparkplug B client communication.
* **Tasks Spawned**:
  1. `Task_ControlEngine` (Priority 5, Stack 6KB, 100 Hz / 10 ms cycle): Evaluates 16 user rules against live sensor data in RAM and controls local/remote actuators.
  2. `Task_ModbusSensorPoll` (Priority 3, Stack 4KB, Continuous): Performs noise-filtered Modbus RTU queries across all active sensors.
  3. `Task_HTTPServer` (Priority 2, Stack 12KB, Event-driven): Handles REST API and serves web dashboard.
  4. `Task_OTAUpdate` (Priority 1, Stack 4KB, Event-driven): Performs CRC32 validation and stages firmware.
  5. `Task_MQTTClient` (Priority 4, Stack 8KB, Periodic / CoV): Connects to MQTT broker, handles DNS resolution, encodes Sparkplug B payloads, subscribes to commands, and manages 512 KB offline telemetry queue.
* **Core Functions**:
  - `Log_Event(category, message)`: Writes timestamped events into a 16 KB RAM circular buffer and persists to W25Q16 Flash.
  - `Relay_SetState(idx, state)`: Actuates Local GPIO pins, Modbus TCP coils, or OPC UA nodes.
  - `vApplicationTickHook()`: Ticks MQTT timer, DNS handler, and increments uptime counter every second.
  - `KontrxDWT_Init()` / `Update_CPU_Usage()`: Computes exact CPU load percentage (0-100%).

### `APP/http_server_task.c` & `APP/http_server_task.h`
* **Purpose**: Embedded HTTP REST Server operating on W5500 Socket 0, Port 80.
* **Key Endpoints Handled**:
  - `GET /` & `GET /index.html`: Streams embedded SPA dashboard directly from W25Q16 Flash.
  - `GET /api/status`: Returns JSON snapshot of all live sensor values, relay states, network settings, system health, and QR data payload.
  - `POST /api/relay` & `POST /api/relay/all`: Controls individual or all actuators.
  - `POST /api/config/relays`: Updates actuator configuration (port, pin, NO/NC, Modbus TCP IP/Port/Coil).
  - `POST /api/config/mqtt`: Configures MQTT broker, port, client credentials, and topic mappings.
  - `POST /api/config/sensors`: Updates Modbus sensor ID mapping.
  - `POST /api/modbus/scan` & `GET /api/modbus/scan`: Initiates and reads 1-247 Modbus address scan results.
  - `GET /api/rules` & `POST /api/rules`: Reads and updates user automation rules.
  - `POST /api/rules/accept` & `POST /api/rules/reject`: Manages rule probationary watchdog state.
  - `POST /api/ota/verify`, `POST /api/ota/prepare`, `POST /update`: Handles chunked firmware uploads with OTP authentication (`KontrxOTA2026`).

### `APP/sparkplug_b_enc.c` & `APP/sparkplug_b_enc.h`
* **Purpose**: High-performance, zero-allocation binary Protobuf serializer implementing the Eclipse Sparkplug B specification.
* **Key Functions**:
  - `sparkplug_encode_nbirth()`: Generates Node Birth message containing full hardware metadata, sensor IDs, and initial state.
  - `sparkplug_encode_ddata()`: Generates Device Data payload encoding sample-averaged sensor metrics (Float/Int32) and relay states (Boolean).
  - `sparkplug_encode_ndeath()`: Generates Node Death payload used as MQTT Last Will and Testament (LWT).

### `APP/ota_task.c` & `APP/ota_task.h`
* **Purpose**: Background task responsible for validating staged firmware images and preparing the system for bootloader swap.
* **Verification Logic**:
  - Suspends Modbus and MQTT tasks to prevent CPU and SPI contention.
  - Computes CRC32 over the entire staged binary in Flash Sector 6-7 (`0x08040000`).
  - Verifies that the initial 4 bytes represent a valid Cortex-M4 Stack Pointer (`0x20000000` - `0x20030000`).
  - Erases Flash Sector 1 (`0x08004000`) and writes `OTA_Meta_t` structure with `OTA_MAGIC_VALUE` (`0xC01D0001`) and `OTA_STATUS_PENDING`.
  - Triggers software system reset via `SCB->AIRCR = AIRCR_VECTKEY | (1U << 2)`.

### `APP/bootloader.c`
* **Purpose**: Stage-1 Bare-Metal Bootloader running at Flash origin `0x08000000`.
* **Execution Flow**:
  1. Initializes USART1 debug console.
  2. Resets W5500 SPI to a clean state (`W5500_BootReset`).
  3. Inspects Sector 1 (`0x08004000`) for pending OTA metadata.
  4. If pending:
     - Erases Application Sectors 2, 3, 4, 5 (`0x08008000` - `0x0803FFFF`).
     - Copies binary word-by-word from Staging (`0x08040000`) to App (`0x08008000`).
     - Reads back and verifies byte integrity.
     - Updates OTA metadata status to `OTA_STATUS_OK`.
  5. Relocates Vector Table (`SCB->VTOR = 0x08008000`), initializes Stack Pointer (`__set_MSP`), and jumps to application entry point.

### `APP/cJSON.c` & `APP/cJSON.h`
* **Purpose**: Lightweight ANSI C JSON parser modified to allocate strictly through FreeRTOS heap memory hooks (`pvPortMalloc`, `vPortFree`).

### `APP/cmsis_os2.c` & `APP/cmsis_os2.h`
* **Purpose**: ARM CMSIS-RTOS v2 API wrapper around FreeRTOS kernel objects (Threads, Mutexes, Semaphores, Kernel Tick).

### `APP/web_assets.h`
* **Purpose**: Contains the compiled Dark-Themed SPA Dashboard (`KONTRX_HTML`) stored in flash memory.
* **Features**:
  - Embedded CSS styling and responsive layout.
  - Live WebSocket / AJAX telemetry polling (1-second refresh).
  - Dynamic drag-and-drop rule flowchart visualizer.
  - Dynamic sensor and actuator configuration tables.
  - Modbus 1-247 auto-discovery network scanner UI.
  - Full client-side QR code generator (v1–40 byte-mode engine with Reed-Solomon error correction).

---

## 3. Hardware Abstraction Layer (`HAL/`)

### `HAL/modbus_dma.c` & `HAL/modbus_dma.h`
* **Purpose**: Core Modbus RTU communication engine driving USART3 and MAX485 transceiver.
* **Key Features**:
  - Noise rejection state machine with timeout-per-sensor scheduling (40 ms for fast sensors, 150 ms for KWS-630 DO).
  - Automatic direction control for MAX485 (`MAX485_TX()` / `MAX485_RX()`).
  - Standard Modbus CRC-16 polynomial generator (`0xA001`).
  - IEEE-754 Little-Endian DCBA float decoder for KWS-630 DO sensor.
  - Support for 7 sensor types: pH, ORP, EC, DO, Ammonia, Single Ultrasonic, and Multi-Point Ultrasonic Array (8 channels).
  - Distributed multi-point ultrasonic polling (2 channels per cycle with 15 ms line discharge gap).
  - Background 1-247 slave auto-discovery scan.
  - `Modbus_DMA_ConsumeBatch()`: Computes mathematical average across all samples collected during the MQTT publish interval.

### `HAL/flash_partition.c` & `HAL/flash_partition.h`
* **Purpose**: Partition manager for the external Winbond W25Q16 (2 MB SPI NOR Flash).
* **Partitions Managed**:
  1. `PARTITION_WEB_ADDR` (`0x00000000`, 512 KB): Web Assets.
  2. `PARTITION_CONFIG_ADDR` (`0x00080000`, 16 KB): Dual-sector redundant system config & rules (Primary & Backup sectors).
  3. `PARTITION_QUEUE_ADDR` (`0x00084000`, 512 KB): Offline telemetry FIFO ring buffer (32,768 slots).
  4. `PARTITION_LOG_ADDR` (`0x00104000`, 1008 KB): Circular event audit log buffer.

### `HAL/plc_control.c` & `HAL/plc_control.h`
* **Purpose**: Industrial PLC integration driver.
* **Key APIs**:
  - `Modbus_TCP_WriteCoil(ip, port, slave_id, coil_addr, state)`: Opens W5500 Socket 4, connects to remote PLC, sends Modbus TCP Function Code 0x05 request, verifies response, and closes socket.
  - `OPC_UA_Client_WriteNode()`: Interface stub for OPC UA endpoints.

### `HAL/w25q16.c` & `HAL/w25q16.h`
* **Purpose**: Low-level SPI driver for Winbond W25Q16 16 Mbit (2 MB) NOR Flash.
* **Commands Implemented**:
  - `0x06`: Write Enable (`WREN`).
  - `0x05`: Read Status Register 1 (`RDSR` - Busy bit polling).
  - `0x03`: Read Data (`READ`).
  - `0x02`: Page Program (`PP` - up to 256 bytes per page).
  - `0x20`: Sector Erase (`SE` - 4 KB sector erase).
  - `0x9F`: Read JEDEC ID (`0xEF4015`).

### `HAL/led.c` & `HAL/led.h`
* **Purpose**: Board status LED driver on GPIO PA6. Provides `LED_Init()`, `LED_On()`, `LED_Off()`, `LED_Toggle()`.

---

## 4. Microcontroller Abstraction Layer (`MCAL/STM32F4/`)

### `MCAL/STM32F4/stm32f407_regs.h`
* **Purpose**: Complete bare-metal register definitions and bitmask macros for STM32F407VET6 peripherals without external HAL dependencies:
  - Base addresses: `RCC_BASE`, `GPIOA_BASE` - `GPIOE_BASE`, `SPI1_BASE`, `SPI2_BASE`, `USART1_BASE`, `USART3_BASE`, `USART6_BASE`, `SYSTICK_BASE`.
  - Type structures: `RCC_TypeDef`, `GPIO_TypeDef`, `SPI_TypeDef`, `USART_TypeDef`, `SysTick_TypeDef`.

### `MCAL/STM32F4/gpio_stm32.c` & `MCAL/STM32F4/gpio_stm32.h`
* **Purpose**: Register-level GPIO initialization and pin multiplexing.
* **Functions**:
  - `GPIO_Init_USART1_Pins()`: PA9 (TX AF7), PA10 (RX AF7).
  - `GPIO_Init_W5500_Pins()`: PB12 (CS), PB13 (SCK AF5), PB14 (MISO AF5), PB15 (MOSI AF5).
  - `W5500_CS_Select()` / `W5500_CS_Deselect()`: Drives PB12 low/high.
  - `GPIO_GetPortId(port_str)`: Converts `"PA"` - `"PE"` string to port index 0-4.

### `MCAL/STM32F4/uart_stm32.c` & `MCAL/STM32F4/uart_stm32.h`
* **Purpose**: Register-level UART driver for USART1 (Debug 115200 baud) and USART3 (Modbus 9600 baud).

### `MCAL/STM32F4/spi_stm32.c` & `MCAL/STM32F4/spi_stm32.h`
* **Purpose**: Register-level SPI2 driver configured as 8-bit Full-Duplex Master, CPOL=0, CPHA=0, Prescaler /2 (21 MHz).

### `MCAL/STM32F4/flash_stm32.c` & `MCAL/STM32F4/flash_stm32.h`
* **Purpose**: Internal STM32 Flash memory controller. Unlocks flash via `KEY1` (`0x45670123`) and `KEY2` (`0xCDEF89AB`), erases sectors 0-7, and programs 32-bit words with busy bit validation.

### `MCAL/STM32F4/rtc_stm32.c` & `MCAL/STM32F4/rtc_stm32.h`
* **Purpose**: Hardware RTC driver with backup domain unlocking (`PWR_CR_DBP`) and Unix epoch counter conversion.

---

## 5. Python Utility Scripts

### `control_relays.py`
* **Purpose**: Automated testing script that toggles Kontrx relays via HTTP REST API (`/api/relay` and `/api/relay/all`). Supports configurable ON/OFF cycle times.

### `decode_sparkplug.py`
* **Purpose**: MQTT listener script that subscribes to `spBv1.0/#`, parses incoming Sparkplug B binary Protobuf payloads, and prints decoded metric names, data types, and values.

### `test_float_decode.py`
* **Purpose**: TDD test suite validating IEEE-754 Little-Endian DCBA float conversion against big-endian BADC format for KWS-630 DO sensor registers.
