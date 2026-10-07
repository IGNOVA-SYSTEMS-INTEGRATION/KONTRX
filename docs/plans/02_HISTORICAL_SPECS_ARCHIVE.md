# Architecture Decision Records (ADRs) & Historical Specs

## 1. Context & Overview
This document records key architectural decisions made across the lifecycle of the KONTRX firmware, documenting the context, alternatives considered, chosen solution, and long-term consequences.

---

## 2. Architecture Decision Records

### ADR-01: Hardware TCP/IP (WIZnet W5500) vs Software LwIP Stack
* **Status**: ACCEPTED
* **Context**: The STM32F407 has an internal Ethernet MAC, which typically runs with an external PHY (e.g. LAN8720) and the open-source LwIP software stack.
* **Problem**: Software TCP/IP stacks running on MCU cores are susceptible to memory starvation, Denial-of-Service buffer exhaustion, and non-deterministic interrupt latency during network storms. This could starve the 100 ms closed-loop control engine.
* **Decision**: Adopt the WIZnet W5500 SPI Ethernet controller with hardwired TCP/IP stack and 8 independent hardware socket buffers (32 KB total on-chip buffer).
* **Consequences**:
  * Offloads all ARP, IP, ICMP, TCP, and UDP processing from the Cortex-M4 core.
  * Conserves ~30 KB of MCU Flash and ~20 KB of RAM.
  * Zero risk of network traffic crashing real-time FreeRTOS control tasks.

---

### ADR-02: Fixed Deterministic Flash Sectors vs Embedded File System (LittleFS)
* **Status**: ACCEPTED
* **Context**: The system needs to persist runtime configurations, rule sets, canvas graphs, and offline telemetry records to external SPI Flash (Winbond W25Q16).
* **Problem**: Embedded file systems like LittleFS or SPIFFS introduce dynamic metadata overhead, wear-leveling tree traversal latency, and dynamic heap allocation that could fail during low-memory conditions.
* **Decision**: Use a fixed-offset sector partition map (`flash_partition.c`) with dual-sector ping-pong redundancy and CRC32 verification for critical configurations.
* **Consequences**:
  * Zero dynamic heap allocation during flash reads/writes.
  * Guaranteed constant-time lookups (O(1)).
  * Fail-safe atomic rollback: updates write to primary with status under test; fallback sector remains pristine.

---

### ADR-03: Zero-Allocation Micro-Protobuf Encoder for Sparkplug B
* **Status**: ACCEPTED
* **Context**: Transmitting telemetry according to Eclipse Sparkplug B specification requires Google Protobuf serialization.
* **Problem**: Standard Nanopb requires code generation from `.proto` definitions and dynamic or complex structure mappings that add binary bloat and maintenance overhead.
* **Decision**: Implement a purpose-built, zero-allocation streaming micro-encoder (`sparkplug_b_enc.c`) that directly writes Varints, Keys, and IEEE-754 floats into a caller-supplied byte buffer.
* **Consequences**:
  * Minimal binary footprint (< 16 KB code size).
  * Sub-millisecond serialization time with zero heap allocations.
  * Perfect compliance with Eclipse Sparkplug B Specification.

---

### ADR-04: CCMRAM Placement for HTTP TX Buffer
* **Status**: ACCEPTED
* **Context**: `Task_HTTPServer` serves large JSON state responses (`GET /api/status`) and file listings that can exceed 8 KB in size.
* **Problem**: Allocating a 12 KB buffer on the FreeRTOS heap or in main SRAM1 reduces available memory for tasks and increases risk of fragmentation.
* **Decision**: Map `tx_buf[12288]` directly into the STM32F407's 64 KB Core Coupled RAM (`.ccmram`) section via `__attribute__((section(".ccmram")))`.
* **Consequences**:
  * Frees 12 KB of contiguous SRAM1 for FreeRTOS task stacks.
  * CCMRAM is connected directly to the Cortex-M4 D-bus with zero wait states.

---

### ADR-05: Gzip-Compressed SPA Embedded in Internal MCU Flash
* **Status**: ACCEPTED
* **Context**: Providing an onboard web user interface for field technicians without an internet connection.
* **Problem**: Storing dozens of separate HTML, CSS, JS, and image files requires an external web server or heavy file system.
* **Decision**: Bundle the entire dashboard into a single-file SPA (`web/index.html`), gzip compress it, and embed it as a static C byte array in flash (`APP/web_assets.h`).
* **Consequences**:
  * Served with `Content-Encoding: gzip` directly to modern web browsers.
  * Consumes under 40 KB of Flash and 0 bytes of RAM at rest.
  * Browser decompresses and caches the client UI in memory.
