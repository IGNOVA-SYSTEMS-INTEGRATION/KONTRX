# Host-Side Unit Tests Execution Guide

## 1. Overview
Unit tests in KONTRX validate core algorithms, binary serialization, protocol framing, and data structures on the host machine without needing physical microcontrollers or debug probes attached.

---

## 2. Test Suites & Commands

### 2.1 C Automation Engine & Logic Unit Tests
Tests condition trees, compare operations, deadband hysteresis, timer delay logic, and rule execution sequences in pure C.
* **Source File**: `tests/test_config_logic.c`
* **Compilation & Execution**:
  ```bash
  # Compile with host GCC
  gcc -Wall -Wextra -O2 -IAPP -IHAL -o test_config tests/test_config_logic.c
  
  # Run executable
  ./test_config
  ```
* **Expected Output**:
  ```text
  [PASS] test_rule_eval_greater_than
  [PASS] test_rule_eval_hysteresis
  [PASS] test_rule_eval_and_logic
  [PASS] test_rule_eval_or_logic
  [PASS] test_rule_eval_timer_on
  All 18 unit tests PASSED.
  ```

---

### 2.2 Modbus RTU Protocol & Frame Unit Tests
Validates standard CRC16 Kermit/Modbus polynomial calculation, float32 DCBA byte swap routines, 16-bit register unpacking, and timeout detection.
* **Source File**: `tests/test_modbus_protocol.py`
* **Execution**:
  ```bash
  pytest tests/test_modbus_protocol.py -v
  ```
* **Key Assertions**:
  * Correctness of CRC16 lookup table vs bit-shift calculation.
  * Correct IEEE-754 conversion from hex registers (`0x4148`, `0x0000` -> `12.50`).
  * Rejection of short frames (< 5 bytes) and parity errors.

---

### 2.3 Web Asset Gzip Pipeline Unit Tests
Validates the build pipeline that embeds `web/index.html` into `APP/web_assets.h`.
* **Source File**: `tests/test_web_assets.py`
* **Execution**:
  ```bash
  pytest tests/test_web_assets.py -v
  ```
* **Key Assertions**:
  * Gzip header magic bytes (`0x1F`, `0x8B`).
  * `KONTRX_HTML_LEN` constant matches raw byte array count.
  * Decompressed string contains valid HTML5 doctype, title, and closed tags.
  * Strict flash budget enforcement (`len(data) < 65536`).

---

### 2.4 QR Code Provisioning Payload Tests
Validates WiFi / Ethernet onboarding string serialization formatted for the mobile application scanner.
* **Source File**: `tests/test_qr_payload.py`
* **Execution**:
  ```bash
  pytest tests/test_qr_payload.py -v
  ```

---

## 3. Running All Host Unit Tests in One Shot
```bash
pytest tests/test_modbus_protocol.py tests/test_web_assets.py tests/test_qr_payload.py -v
```
