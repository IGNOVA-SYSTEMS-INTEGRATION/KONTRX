# Kontrx

Kontrx Edge Gateway — a FreeRTOS-based industrial water-quality monitoring gateway built around the **STM32F407VET6** (ARM Cortex-M4F). It reads up to 7 water-quality sensors over RS485 Modbus RTU, drives 10 configurable relays, serves a live dashboard over Ethernet, and supports in-field Over-The-Air (OTA) firmware updates.

## Hardware

| Peripheral  | Interface  | Notes                                        |
|-------------|------------|----------------------------------------------|
| STM32F407VET6 | —         | Cortex-M4F, 512 KB flash, 192 KB RAM          |
| W5500       | SPI2 (PB12–PB15) | 10/100 Mbps Ethernet, ioLibrary_Driver    |
| MAX485      | USART3 + DE/RE (PD3/PD2), DMA | RS485 Modbus RTU bus                  |
| Sensors     | Modbus RTU | pH, ORP, EC, DO, Ammonia, Multi-point Ultrasonic, Single-point Ultrasonic |
| Relays      | GPIO       | 10 relays, per-relay pin + NC/NO configurable |
| Debug UART  | USART1     | 115200 baud `printf` bridge                   |

## Features

- **4 FreeRTOS tasks** (CMSIS-OS v2 API): 1 kHz Control Engine, 250 ms Modbus sensor polling (round-robin), HTTP server, and OTA updater.
- **REST API** over W5500 socket 0 on port 80 serving a dark-themed SPA dashboard:
  - `GET  /api/status` — live sensor readings, relay states, uptime, QR payload
  - `POST /api/relay?id=N&state=0|1` — toggle individual relay
  - `POST /api/relay/all?state=0|1` — all relays
  - `POST /api/config/relays` — per-relay pin / NC-NO config
  - `POST /api/config/mqtt` — MQTT broker config
  - `POST /api/config/sensors` — Modbus slave IDs per sensor
  - `POST /api/modbus/scan`, `GET /api/modbus/scan` — auto-discovery scan (IDs 1–247)
  - `POST /api/ota/verify` — OTP validation for firmware unlock
  - `POST /update` — OTA firmware upload
- **Configuration persistence** in EEPROM/flash Sector 11 (relay pin/NC, MQTT, sensor IDs), loaded at boot.
- **QR provisioning**: the dashboard renders a QR code encoding a JSON `qrData` payload — device identity, MAC-based serial, MQTT identity, timestamps, all sensor readings, and relay states.
- **OTA updates**: app runs at `0x08008000` with an independent bootloader at `0x08000000`; firmware is staged and validated (CRC32) before a direct bootloader jump.

## Repository Layout

```
APP/                    Application sources (tasks, HTTP server, OTA, web_assets)
MCAL/STM32F4/           Register-level drivers (GPIO, UART, SPI, Flash, RTC)
HAL/                    High-level HAL (LED, Modbus DMA)
ioLibrary_Driver/       WIZnet Ethernet stack (W5500, sockets, DHCP)
FreeRTOS/               FreeRTOS kernel + ARM_CM4F port
bootloader.ld           Bootloader linker script (flash @ 0x08000000)
app_ota.ld              Application linker script (flash @ 0x08008000)
startup_stm32f407xx.c   Startup / vector table
```

## Build

Requires `arm-none-eabi-gcc` and CMake (xpack toolchain supported).

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=arm_toolchain.cmake
cmake --build build
```

Generated artifacts (in `build/`):

| Target          | Description                                             |
|-----------------|---------------------------------------------------------|
| `Bootloader`    | `Bootloader.bin` — flash @ 0x08000000                    |
| `Kontrx`        | `Kontrx.bin` — legacy no-RTOS W5500 test build           |
| `RS485_Diag`    | Standalone RS485 raw-byte diagnostics (safe to iterate)  |
| `KontrxRTOS`    | `KontrxRTOS.bin`/`.hex` — full RTOS application @ 0x08008000 |

Flash the bootloader once, then any subsequent firmware can be pushed over the network through the dashboard's OTA panel.

## Known-issue notes

- `APP/main_*.c` (besides `main_kontrx.c`) are per-feature test entries; only `main_kontrx.c` builds into the production `KontrxRTOS` target.
- The embedded QR library in `web_assets.h` is the full v1–40 implementation. Three latent bugs in the original port were fixed: `G15_MASK` bit (now `0x5412`), `getLostPoint` LEVEL1–4 rule set, and a dynamic byte-mode length field. Output is byte-identical to `qrcode-generator` 1.4.4 across all EC levels and versions 1–33.

## License

MIT — see [LICENSE](LICENSE).
