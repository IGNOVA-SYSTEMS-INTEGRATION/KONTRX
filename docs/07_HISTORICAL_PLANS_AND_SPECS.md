# 07 - KONTRX Historical Engineering Plans & Migration Specs

## 1. Evolution Timeline

| Milestone | Version | Date | Key Architectural Additions |
|---|---|---|---|
| **Genesis** | v1.0 | 2025-Q4 | Bare-metal firmware, fixed 4 Modbus sensors, static 4 GPIO relays. |
| **RTOS Transition** | v2.0 | 2026-Q1 | FreeRTOS 10 kernel adoption, W5500 SPI driver, basic HTTP web config server. |
| **Actuator Engine** | v2.2 | 2026-Q3 | Multi-type actuator engine (PWM, PTO stepper motion, 0-10V, 4-20mA), CCMRAM buffer placement. |
| **Advanced Rules & History**| v2.3 | 2026-Q4 | Hierarchical condition trees (Logic gates, TON/TP timers, SR-latches), action sequences, canvas layout persistence, and 12-slot flash rules history rollback. |

---

## 2. Key Architecture Decision Records (ADRs)

### ADR-01: W5500 vs STM32 Internal MAC + PHY
* **Decision**: Adopt WIZnet W5500 SPI Ethernet controller.
* **Rationale**: Offloads the entire TCP/IP stack from the STM32F4 core into hardware. Prevents denial-of-service packet floods from crashing real-time industrial actuator tasks and saves ~30 KB of microcontroller flash/RAM otherwise needed by LwIP.

### ADR-02: NOR Flash Partitioning vs Unified File System
* **Decision**: Fixed deterministic NOR flash sectors for configuration and circular buffers instead of LittleFS.
* **Rationale**: Eliminates dynamic heap allocation during flash writes and guarantees predictable write wear and zero corruption on abrupt power loss.

### ADR-03: Sparkplug B Compliance
* **Decision**: Adopt Sparkplug B payload format over plain proprietary JSON.
* **Rationale**: Seamless plug-and-play interoperability with Ignition SCADA, AXIRA Cloud, and enterprise historians without custom payload decoders.
