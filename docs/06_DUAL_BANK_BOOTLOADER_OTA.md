# 06 - KONTRX Dual-Bank Bootloader, HTTP OTA & Safe Flash Recovery

## 1. Zero-Bricking OTA Requirement
In remote agricultural and offshore aquaculture deployments, a failed firmware update that leaves the microcontroller in an unbootable state necessitates an expensive on-site technician visit. 
KONTRX eliminates this risk through a fail-safe Dual-Bank Bootloader architecture.

---

## 2. Flash Memory Map & Boot Stages

```text
0x08000000 ┌────────────────────────────────────────┐
           │ Bootloader (32 KB, Sector 0)          │
0x08008000 ├────────────────────────────────────────┤
           │ Application Bank A (480 KB)           │
0x08080000 ├────────────────────────────────────────┤
           │ Application Bank B / Staging (480 KB) │
0x080F8000 ├────────────────────────────────────────┤
           │ Bootloader Flags & Metadata (32 KB)   │
0x08100000 └────────────────────────────────────────┘
```

### Update Procedure:
1. **HTTP Streaming Upload**:
   * The new firmware binary (`Kontrx.bin`) is streamed directly over HTTP POST `/api/ota/upload`.
   * Written page-by-page into the staging flash partition.
2. **Pre-Flash Integrity Verification**:
   * Complete image CRC32 is calculated and validated against the payload header.
   * Magic bytes and vector table initial stack pointer (`_estack`) are verified to point to valid SRAM address ranges (`0x20000000`–`0x20020000`).
3. **Bootloader Handoff**:
   * The boot flag is written to external flash: `BOOT_FLAG_PENDING_UPGRADE`.
   * The system triggers a clean software reset via `NVIC_SystemReset()`.
4. **Bootloader Execution**:
   * The bootloader verifies the new image.
   * If valid, it copies the new firmware to the active execution sector and resets into the new app.
   * If the new application fails to feed the watchdog or initialize within 30 seconds, the bootloader automatically reverts to the previous working golden image.
