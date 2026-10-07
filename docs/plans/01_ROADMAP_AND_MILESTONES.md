# KONTRX Engineering Roadmap & Release Milestones

## 1. Release Timeline & Milestone Breakdown

| Milestone | Target Version | Status | Architectural Highlights |
|---|---|---|---|
| **M1: Genesis** | v1.0 | ✅ Shipped | Bare-metal polling loop, static 4 Modbus RTU sensors, 4 direct GPIO relay outputs. Basic UART debug CLI. |
| **M2: RTOS Migration** | v2.0 | ✅ Shipped | Migration to FreeRTOS kernel (v10). Integration of WIZnet W5500 SPI Ethernet controller. Basic HTTP dashboard and MQTT JSON telemetry. |
| **M3: Multi-Domain Actuator Engine** | v2.2 | ✅ Shipped | 8 actuator domains: Relays, Modbus TCP Coils, PWM fans/dosing, 4-Axis PTO stepper motion, 4–20mA current loop, 0–10V analog voltage. Allocation of TX buffers to 64 KB CCMRAM. |
| **M4: Industrial Telemetry & Canvas** | v2.3 | ✅ Shipped | Sparkplug B Protobuf micro-encoder (NBIRTH/DDATA/NDEATH), visual canvas JSON layout persistence on SPI NOR Flash, dual-sector rule rollback (`RULES_MAGIC_CURRENT`), SD card offline store-and-forward queue. |
| **M5: Fieldbus Expansion & Security** | v2.5 | 🔄 In Progress | Modbus TCP server (Port 502) optimization, BACnet/IP gateway, session token renewal, hardware ASIC driver hooks. |
| **M6: Enterprise IIoT Suite** | v3.0 | 📅 Planned | Native TLS 1.3 encryption offload, hardware EtherCAT slave module support, cloud mesh synchronization with AXIRA Core. |

---

## 2. Milestone Deliverables & Verification Criteria

### Milestone 4 (Current Stable Baseline - v2.3)
* **Deliverables**:
  * FreeRTOS 6-task deterministic matrix.
  * Hierarchical condition trees (Logic gates, Range, TON/TP timers, SR latches).
  * 16-channel Modbus RTU polling with moving average + median outlier DSP filtering.
  * W25Q16 external NOR Flash partition scheme with dual-sector config and rule redundancy.
  * Embedded gzipped responsive web dashboard in internal flash (< 64 KB).
  * Safe dual-bank OTA bootloader with CRC32 verification and automatic rollback.

### Milestone 5 (In Progress - v2.5)
* **Deliverables**:
  * Full BACnet/IP device profile compliance on UDP Port 47808.
  * Modbus TCP multi-client socket pooling on W5500.
  * Diagnostic event tracing via SD card paged query API.
