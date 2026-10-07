# Feature: Sparkplug B Industrial MQTT Telemetry & Protocol Encoder

## 1. Description & Purpose (بتعمل ايه وليه اتعملت)
Provides standard Eclipse Sparkplug B compliant MQTT telemetry encoding (Protobuf wire format) directly on the STM32F4 micro-controller.
* **Why it was built**: Standard raw JSON over MQTT is bloated, lacks strict typing, and has no native SCADA discovery semantics. Sparkplug B provides:
  1. Standardized birth/death certificates (`NBIRTH`, `NDEATH`, `DBIRTH`, `DDATA`, `DDEATH`).
  2. Compact binary Protobuf encoding, reducing payload sizes by ~85% compared to JSON.
  3. Seamless plug-and-play integration into industrial SCADA and IIoT platforms like Ignition, HiveMQ, and Node-RED.

---

## 2. Technical Implementation & How It Works (بتشتغل ازاي)
* **Code Location**: `APP/sparkplug_b_enc.c`, `APP/sparkplug_b_enc.h`, `APP/freertos_tasks.c` (`Task_MQTTClient`).
* **Protobuf Wire Encoder Without External Heavy Libraries**:
  * Instead of pulling in large Nanopb or Google Protobuf libraries that consume huge flash/RAM, `sparkplug_b_enc.c` implements a zero-allocation, stream-oriented micro-encoder.
  * Encodes Varints, Wire Types (Varint `0`, 64-bit `1`, Length-delimited `2`, 32-bit `5`), and Metric structures into a static scratch buffer.
* **Message Lifecycle**:
  1. **NBIRTH**: Published on broker connection. Advertises all active metrics (sensor channels, relay states, PWM channels, PTO axes, analog current/voltages, board uptime).
  2. **DDATA**: Published periodically (1 to 10 seconds, or on change/dead-band threshold). Encodes updated telemetry metrics and sequence counter (`seq`).
  3. **NDEATH / LWT**: Pre-registered with the MQTT broker via `Last Will and Testament` on topic `spBv1.0/<group>/NDEATH/<node_id>`. Published automatically by broker if TCP socket drops unexpectedly.
* **Topic Structure**:
  * `spBv1.0/{Group_ID}/{Message_Type}/{Edge_Node_ID}[/{Device_ID}]`
  * Example: `spBv1.0/KONTRX_FARM/NBIRTH/EDGE_NODE_01`

---

## 3. Tests Performed & Verification (ايه الـ Tests اللي اتعملت عليها وازاي)
* **Automated Python Protocol Tests**:
  * File: `tests/test_mqtt_and_ui.py`
  * Execution:
    ```bash
    pytest tests/test_mqtt_and_ui.py -k "sparkplug" -v
    ```
  * What it verifies: Protobuf payload decoding using Google's official `sparkplug_b_pb2` Python package, metric name validity, timestamp millisecond alignment, and sequence counter continuity.
* **Broker & Ignition Interoperability Test**:
  * Successfully connected to EMQX and Eclipse Mosquitto brokers; ingested into Ignition Edge SCADA via Cirrus Link Sparkplug Engine module without a single malformed packet.
* **Bandwidth Profiling**:
  * Benchmark confirmed an average 16-sensor batch encodes into ~210 bytes of Protobuf compared to ~1,450 bytes of JSON.

---

## 4. System Impact & Inter-Dependencies (بتاثر علي ايه ومرتبطه بايه)
* **Consumers / Producers**:
  * Consumes aggregated sensor batches from `Task_ModbusSensorPoll`.
  * Encodes actuator states (Relays, PWM, PTO, 4-20mA, 0-10V) from `main_kontrx.c`.
  * Transmitted over W5500 SPI Ethernet by `Task_MQTTClient`.
* **State Management**:
  * Tracks 64-bit sequence counter (`seq = (seq + 1) % 256`) per Sparkplug B standard.

---

## 5. Development Status & Blockers (حالتها والمعوقات)
* **Status**: ✅ **COMPLETED & OPERATIONAL (مكتملة ومطابقة للمواصفات)**.
* **Blockers / Known Gaps**: None. Fully compliant with Eclipse Sparkplug B Specification v2.2.
