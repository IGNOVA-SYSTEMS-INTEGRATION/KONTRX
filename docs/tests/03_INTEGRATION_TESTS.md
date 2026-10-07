# Network & System Integration Testing Guide

## 1. Overview
Integration tests verify end-to-end communication between external client software (web browsers, Python test harnesses, mobile apps, MQTT brokers) and the KONTRX controller operating over its W5500 SPI Ethernet stack.

---

## 2. Test Execution & Configuration

Integration tests accept a `--device` IP parameter pointing to the target controller (default is `http://192.168.1.50`).

```bash
# Run against live hardware controller on local subnet
pytest tests/test_http_api.py --device=192.168.1.50 -v
```

---

## 3. Test Suites

### 3.1 HTTP REST API & Security Verification
* **Source File**: `tests/test_http_api.py`
* **Test Cases Covered**:
  * `test_health_and_status`: Queries `GET /api/status`, asserts JSON contains sensor array, relay states, system uptime, and heap metrics.
  * `test_auth_flow`: Tests `POST /api/auth/login` with default credentials (`admin` / `admin`), validates session cookie generation, verifies authenticated calls to `/api/config/*`, and asserts 401 Unauthorized upon logout.
  * `test_relay_toggle`: Toggles individual relay via `POST /api/relay?id=1&state=1`, verifies state updates in status response.
  * `test_relay_all_toggle`: Commands all relays simultaneously to 0 and 1.
  * `test_interfaces_discovery`: Queries `GET /api/interfaces`, verifies 12 hardware interfaces with proper status enums and channel metrics.
  * `test_rules_export_import`: Uploads a 5-rule configuration JSON to `POST /api/rules`, then exports via `GET /api/rules` and asserts structural identity.
  * `test_canvas_layout_roundtrip`: Uploads visual canvas node positioning data to `/api/canvas/layout`, verifies persistence across simulated reboot.

---

### 3.2 Actuator Multi-Domain Control Integration
* **Source File**: `tests/test_actuators.py`
* **Test Cases Covered**:
  * Actuator type registration (Relay, PWM, PTO, 0–10V, 4–20mA).
  * Range clamps: PWM duty clamped to `[0.0, 100.0]%`, 4–20mA clamped to `[4.0, 20.0] mA`, 0–10V clamped to `[0.0, 10.0] V`.
  * Frequency constraints: PWM/PTO within timer prescaler bounds (`10 Hz` to `100 kHz`).
  * Safety pin reservation: Asserts that attempting to assign actuators to SPI/Ethernet pins raises configuration validation errors.

---

### 3.3 Sparkplug B & MQTT Broker Connectivity
* **Source File**: `tests/test_mqtt_and_ui.py`
* **Test Cases Covered**:
  * Connection handshake with local Mosquitto / EMQX broker.
  * Verification of `NBIRTH` payload containing all sensor and actuator metrics encoded in Protobuf wire format.
  * Periodic `DDATA` publication rate and sequence number incrementation (`seq = (seq + 1) % 256`).
  * Simulating ungraceful TCP termination and observing broker publication of pre-registered `NDEATH` Last Will and Testament.

---

### 3.4 Regression & Bug Fix Integration
* **Source File**: `tests/test_bug_fixes.py`
* **Test Cases Covered**:
  * Verifies fix for historical memory leak in `cJSON_Delete` during rapid rule uploads.
  * Verifies flash partition sector alignment and dual-sector fallback when primary sector magic is erased.
