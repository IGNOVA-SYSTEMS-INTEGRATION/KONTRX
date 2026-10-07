# Hardware-in-the-Loop (HIL) & Environmental Testing

## 1. Overview
Hardware-in-the-Loop (HIL) testing validates KONTRX firmware running on physical target hardware connected to actual industrial transceivers, electrical loads, and sensor equipment.

---

## 2. Test Bench Setup & Topology

```
   [ Workstation / Test Rig ]
       | Ethernet (192.168.1.50)
       v
+-----------------------------------------------------------------+
|                  KONTRX CONTROLLER (STM32F407)                  |
|                                                                 |
| [MAX485 USART3]       [Relay Outputs]      [TIM1 PTO / TIM4 PWM]|
+--------|---------------------|----------------------|-----------+
         | RS485 Bus           | 24V Contactors       | Stepper Driver
         v                     v                      v
   +---------------+     +---------------+     +---------------+
   | Modbus Sensors|     | 3-Phase Pump  |     | NEMA 23 Motor |
   | pH / EC / DO  |     | Dummy Load    |     | Encoder Return|
   +---------------+     +---------------+     +---------------+
```

---

## 3. Test Scenarios & Procedures

### 3.1 Long-Distance RS485 Bus Noise Resilience
* **Equipment**: 50 meters of unshielded Cat5e twisted pair running alongside a 2.2 kW Variable Frequency Drive (VFD) cable.
* **Procedure**:
  1. Poll 4 Modbus RTU sensors at 9600 baud continuously while cycling the VFD from 0 to 50 Hz.
  2. Measure packet error rate (PER) and CRC discard counts over 24 hours.
* **Pass Criterion**: Zero task lockups; CRC failures logged without disrupting healthy polling cycles; DSP moving average filter maintains stable readings within ±1.5% variance.

---

### 3.2 Power Brownout & Sector Rollback Test
* **Equipment**: Programmable DC bench power supply with arbitrary voltage slew rates.
* **Procedure**:
  1. Trigger a configuration commit via HTTP `POST /api/rules`.
  2. Drop controller DC input from 24V to 0V precisely 2 ms into SPI flash write cycle.
  3. Re-apply 24V and inspect boot logs over debug UART (USART1 @ 115200 baud).
* **Pass Criterion**: MCU reboots cleanly; `Partition_LoadRules()` detects partial write; automatically rolls back to backup sector (`RULES_BACKUP_ADDR`); operational rules intact.

---

### 3.3 High-Current Relay Inductive Kickback Stress
* **Equipment**: 8 AC inductive contactor coils (24V AC, 15 VA) connected across Relay outputs 1–8 with snubbers.
* **Procedure**:
  1. Run chaser routine toggling all 8 relays in rapid sequence at 5 Hz for 100,000 cycles.
  2. Observe microcontroller reset lines and 3.3V power rails on digital storage oscilloscope.
* **Pass Criterion**: Zero unexpected resets; no watchdog triggers; zero corruption of internal SRAM or peripheral registers.

---

### 3.4 MicroSD Card Hot-Swap & File Recovery
* **Equipment**: Kingston Industrial MicroSD 32GB (FAT32).
* **Procedure**:
  1. Disconnect Ethernet to force telemetry into offline queue mode.
  2. Mechanically extract SD card while write cycle is active.
  3. Verify FreeRTOS log output.
  4. Re-insert SD card 30 seconds later.
* **Pass Criterion**: Firmware detects card removal without kernel fault; diverts pending queue items to onboard W25Q16 flash; mounts re-inserted card upon next check; syncs queue successfully.
