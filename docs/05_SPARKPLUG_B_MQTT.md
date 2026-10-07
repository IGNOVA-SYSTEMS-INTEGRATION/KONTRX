# 05 - KONTRX Sparkplug B Edge MQTT Client & Node Identity

## 1. Overview
Sparkplug B is an open-source software specification from the Eclipse Foundation designed specifically to provide MQTT brokers with the context, state awareness, and topic namespace required for mission-critical Industrial IoT (IIoT) and SCADA applications.

---

## 2. Dynamic Node Identity Architecture
Every KONTRX edge device is assigned unique hardware-bound identifiers upon first factory boot:
* **Edge Node ID**: Formatted as `KX-%07lu` using the STM32 factory serial number (e.g. `KX-0001042`).
* **Device ID**: Formatted as `KX-%07lu-dev` (e.g. `KX-0001042-dev`).
* **Persistent Migration**: If a device upgrades from older firmware where these fields were blank, `Modbus_DMA_Init` automatically provisions these values into Flash memory on startup without requiring manual re-configuration.

---

## 3. Sparkplug B Topic Structure & Lifecycle

```text
spBv1.0/{group_id}/{message_type}/{edge_node_id}/{device_id}
```

### Lifecycle Messages:
1. **NBIRTH (Node Birth)**:
   * Published when KONTRX establishes its broker session.
   * Announces device metadata (Hardware revision, firmware version, network IP, MAC address, uptime).
2. **NDEATH (Node Death)**:
   * Registered as the MQTT Last Will and Testament (LWT) payload.
   * If network drops unexpectedly, the broker immediately alerts AXIRA Cloud that the node is offline.
3. **DDATA (Device Data)**:
   * Published periodically (or on Change-of-Value / CoV).
   * Contains real-time sensor metrics and actuator operational states.
4. **DCMD (Device Command)**:
   * Consumed by KONTRX when AXIRA Cloud commands an actuator change (e.g., turn relay ON, set PWM duty cycle, adjust PTO position).

---

## 4. Dynamic Sensor Metric Encoding
The v2.2+ firmware dynamically iterates configured sensors in `cfg->sensors`:
* Emits metric triples for each channel:
  * `Sensors/{type}_{id}/Value` (Float)
  * `Sensors/{type}_{id}/Temperature` (Float)
  * `Sensors/{type}_{id}/Online` (Boolean quality flag)
* **Offline Sensor Filter (`mqtt_skip_offline`)**:
  * When enabled, faulty or unattached sensors are omitted from the payload to reduce wireless telemetry bandwidth by up to 60%.
