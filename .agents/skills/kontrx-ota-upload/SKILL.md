---
name: kontrx-ota-upload
description: >-
  Use this skill whenever the user asks to compile the firmware and/or upload/flash the updated code to the Kontrx Controller via OTA (Over-The-Air).
  Triggers include requests like "compile and upload", "flash to controller", "upload firmware OTA", "ارفع للكنترولر", "اعمل compile وارفع".
---

# Kontrx Controller OTA Build & Flash Skill

This skill automates compiling the Kontrx RTOS firmware for the STM32F407VET6 hardware controller and uploading the compiled `KontrxRTOS.bin` binary via HTTP OTA.

## Execution Procedure

Whenever a code modification is made to the controller or when the user requests a firmware compile and OTA flash:

1. **Run the Automated OTA Build & Flash Tool**:
   Execute the dedicated Python helper script:
   ```bash
   python tools/ota_upload.py
   ```

2. **Custom Target IP (Optional)**:
   If a specific IP is provided by the user (e.g., `192.168.1.100`), pass it as an argument:
   ```bash
   python tools/ota_upload.py 192.168.1.100
   ```

3. **What the Automation Tool Handles**:
   - Compiles `KontrxRTOS.bin` using `cmake --build build` (or initializes CMake with `arm_toolchain.cmake` & `Ninja`).
   - Verifies that the compiled binary size does not exceed the **224 KB** staging sector boundary.
   - Detects the active Kontrx Controller IP address (default `192.168.1.200`).
   - Authenticates with admin credentials (`admin` / `adminkontrx`) to get session token `X-Auth-Token`.
   - Validates the OTA OTP secret (`KontrxOTA2026`) via `POST /api/ota/verify`.
   - Prepares and erases flash staging sectors (Sectors 6 & 7) via `POST /api/ota/prepare`.
   - Streams the raw binary `KontrxRTOS.bin` via `POST /update`.
   - Confirms HTTP 200 reboot response from controller.

4. **Verification**:
   After flashing, wait ~5 seconds for controller reboot and verify online status:
   ```bash
   python -c "import requests; print(requests.get('http://192.168.1.200/api/status', timeout=3).json())"
   ```
