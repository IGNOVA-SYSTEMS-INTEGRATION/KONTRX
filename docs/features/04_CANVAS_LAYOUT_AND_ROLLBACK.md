# Feature: Canvas Layout Persistence & Dual-Sector Rollback

## 1. Description & Purpose (بتعمل ايه وليه اتعملت)
Provides non-volatile, fail-safe storage for both visual node-graph layouts and compiled automation rules on external SPI NOR Flash (Winbond W25Q16).
* **Why it was built**:
  1. Desktop Configurators (e.g. Electron/React visual canvas) create visual graphs (node positions, connection wires, visual tags) that cannot be represented in flat MCU execution structs (`Rule_t`). Without canvas blob storage, re-opening a configuration from the controller loses visual topology.
  2. Industrial installations risk bricking or unstable logic updates (e.g., power loss during flash erase or misconfigured rules causing runaway oscillations). A robust dual-sector rollback mechanism ensures the controller never boots with corrupted or invalid configurations.

---

## 2. Technical Implementation & How It Works (بتشتغل ازاي)
* **Code Location**: `HAL/flash_partition.c`, `HAL/flash_partition.h`, `HAL/w25q16.c`, `APP/freertos_tasks.c`.
* **Flash Partition Scheme on W25Q16 (2 MB)**:
  * `PARTITION_CONFIG_ADDR` (`0x00080000`, 16 KB):
    * Primary Config (`0x00080000`, 4 KB, Sector 128)
    * Backup Config (`0x00081000`, 4 KB, Sector 129)
    * Primary Rules (`0x00082000`, 4 KB, Sector 130)
    * Backup Rules (`0x00083000`, 4 KB, Sector 131)
  * `LAYOUT_PARTITION_ADDR` (`0x00084000`, 16 KB, Sectors 132 to 135):
    * Stores raw visual canvas JSON (up to 16,372 bytes) prefixed with magic `0x4C41594FU` ("LAYO"), 32-bit payload length, and CRC32.
* **Dual-Sector Atomic Rollback Sequence**:
  1. **Atomic Write**: When new rules/configs arrive via HTTP or MQTT, `Partition_BackupCurrentRules()` copies current operational rules to `RULES_BACKUP_ADDR`.
  2. **Staged Validation**: The new rule set is written to `RULES_PRIMARY_ADDR` with `rules_valid = 0` (Testing mode).
  3. **Stability Window**: A hardware/software health supervisor observes system stability across 5 minutes. If no crash, watchdog reset, or sensor panic occurs, `rules_valid` is flipped to `1`.
  4. **Automatic Fallback**: Upon boot, `Partition_LoadRules()` verifies `magic == RULES_MAGIC_CURRENT` and CRC32. If corrupt or validation failed, the system automatically falls back to `RULES_BACKUP_ADDR`.

---

## 3. Tests Performed & Verification (ايه الـ Tests اللي اتعملت عليها وازاي)
* **Automated Python Integration Suite**:
  * File: `tests/test_bug_fixes.py` & `tests/test_http_api.py`
  * Execution:
    ```bash
    pytest tests/test_bug_fixes.py -k "layout or rollback or partition" -v
    ```
  * What it verifies: Canvas JSON export and import fidelity, round-trip node position preservation, CRC32 mismatch detection, and sector boundary overflow prevention.
* **Power-Cut Simulation (Sudden Brownout)**:
  * Simulated sudden brownout interrupt during sector erase/write cycle on SPI Flash; verified that upon reboot, the system safely reverted to the backup sector without hanging.
* **Corrupt Sector Fallback Test**:
  * Programmatically injected invalid checksums into `RULES_PRIMARY_ADDR`; confirmed MCU detected corruption and loaded `RULES_BACKUP_ADDR` seamlessly with syslog warning.

---

## 4. System Impact & Inter-Dependencies (بتاثر علي ايه ومرتبطه بايه)
* **Connected Subsystems**:
  * `Task_HTTPServer`: Serves `/api/rules/export`, `/api/rules/import`, `/api/canvas/layout`.
  * `Task_ControlEngine`: Consumes loaded `RuleConfig_t` during runtime execution.
  * `W25Q16 HAL`: Direct dependency on SPI1 flash low-level driver.
* **Safety Lockout**:
  * Uses flash write mutex `flashMutex` to ensure concurrent log writes or telemetry queuing never interrupt configuration persistence.

---

## 5. Development Status & Blockers (حالتها والمعوقات)
* **Status**: ✅ **COMPLETED & PRODUCTION STABLE (مكتملة ومستقرة تماماً)**.
* **Blockers / Known Gaps**: None. Supports full round-trip canvas persistence with the Electron desktop app.
