# Feature: Dual-Bank Staging OTA Bootloader & Safe Firmware Update

## 1. Description & Purpose (بتعمل ايه وليه اتعملت)
Provides brick-proof Over-The-Air (OTA) firmware update capabilities across the local network without physical ST-Link or JTAG programmer cables.
* **Why it was built**: Industrial controllers are often installed inside sealed NEMA electrical enclosures, atop water towers, or in remote agricultural fields. Requiring a technician to connect an SWD debugger to flash updates is expensive, slow, and dangerous. The OTA subsystem allows remote updates with guaranteed rollback if a corrupted image or network interruption occurs during the transfer.

---

## 2. Technical Implementation & How It Works (بتشتغل ازاي)
* **Code Location**: `APP/bootloader.c`, `APP/ota_task.c`, `APP/ota_task.h`, `APP/http_server_task.c`, `MCAL/flash_stm32.c`.
* **Internal STM32F407 Flash Sector Partitioning (512 KB)**:
  * **Sector 0** (`0x08000000`, 16 KB): Dedicated bare-metal Bootloader.
  * **Sector 1** (`0x08004000`, 16 KB): `OTA_Meta_t` metadata (Magic `0x4F544131`, Status, Image Size, CRC32).
  * **Sectors 2–5** (`0x08008000`–`0x0803FFFF`, 224 KB): Active Running Application (`APP_START_ADDR`).
  * **Sectors 6–7** (`0x08040000`–`0x0807FFFF`, 256 KB): Inactive Staging Area (`STAGING_ADDR`).
  * **Sector 7 Tail** (`0x0807C000`, 16 KB): Non-volatile Config EEPROM emulation.
* **Firmware Update Sequence**:
  1. **HTTP Streaming**: Browser uploads `.bin` file via `POST /api/ota/upload`. The HTTP task streams incoming chunks directly into the Staging Area (`0x08040000`).
  2. **Staging Verification**: `Task_OTAUpdate` wakes up via `sem_ota_start`, computes full IEEE-802.3 CRC32 across the staged bytes, and validates the initial MSP stack pointer against RAM boundary (`0x20000000`).
  3. **Metadata Commit**: If CRC matches, writes `OTA_Meta_t` to Sector 1 with status `OTA_STATUS_PENDING`.
  4. **Controlled Reset**: Triggers software system reset via `SCB_AIRCR`.
  5. **Bootloader Handoff**:
     * Hardware boots into Sector 0 (`bootloader.c`).
     * Bootloader inspects Sector 1. If `status == OTA_STATUS_PENDING`, it verifies staging integrity.
     * Erases Sectors 2–5 and copies new firmware from Staging to Application memory.
     * Updates status to `OTA_STATUS_OK`, resets W5500 SPI PHY, points `SCB_VTOR` to `0x08008000`, sets Main Stack Pointer (`__set_MSP`), and jumps to application `Reset_Handler`.
     * If validation fails at any point, staging is rejected and the bootloader directly launches the previous stable application.

---

## 3. Tests Performed & Verification (ايه الـ Tests اللي اتعملت عليها وازاي)
* **OTA Scripting & API Verification**:
  * Skill: `.agents/skills/kontrx-ota-upload/SKILL.md`
  * Execution:
    ```bash
    python .agents/skills/kontrx-ota-upload/scripts/ota_upload.py --ip 192.168.1.50 --bin build/KontrxRTOS.bin
    ```
  * What it verifies: HTTP multipart upload, OTP challenge-response, CRC32 verification handoff, reboot trigger, and post-flash reconnect.
* **Interrupted Upload Stress Test**:
  * Deliberately aborted HTTP upload mid-stream at 45% completion; verified that Sector 1 metadata was not marked pending and the MCU rebooted safely into the existing application without corruption.
* **Corrupt Image Rejection**:
  * Uploaded a modified binary with corrupted vector table addresses; verified that the bootloader rejected the stack pointer validation and refused to overwrite active flash.

---

## 4. System Impact & Inter-Dependencies (بتاثر علي ايه ومرتبطه بايه)
* **Interactions**:
  * `APP/http_server_task.c` manages upload receiving and config backup.
  * FreeRTOS tasks pause during final commit handoff.
  * Preserves user calibration, relay maps, and sensor configurations in Sector 7 tail.
* **Hardware Drivers**:
  * Low-level flash driver `MCAL/flash_stm32.c` and NVIC register controls.

---

## 5. Development Status & Blockers (حالتها والمعوقات)
* **Status**: ✅ **COMPLETED & FIELD PROVEN (مكتملة ومجربة ميدانياً بنجاح)**.
* **Blockers / Known Gaps**: None. Fully automated via `kontrx-ota-upload` toolchain.
