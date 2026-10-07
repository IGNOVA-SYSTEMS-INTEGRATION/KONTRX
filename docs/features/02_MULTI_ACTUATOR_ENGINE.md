# Feature: Multi-Type Actuator Engine & Hardware Control Subsystems

## 1. Description & Purpose (بتعمل ايه وليه اتعملت)
Provides unified software and hardware control over **8 distinct industrial actuator domains**:
1. Local GPIO Relays (0–8)
2. Modbus TCP Remote Coils (over Socket 4)
3. OPC UA Remote Sinks
4. Industrial PWM (variable speed fans, proportional dosing)
5. PTO (Pulse Train Output) 4-axis stepper/servo motion
6. 4–20mA Analog Current Loop Transmitters
7. 0–10V Analog Voltage Transmitters
8. General Digital Outputs
* **Why it was built**: Modern automation sites require more than simple on/off relays. Dosing pumps require 4–20mA, chemical stirrers require PWM speed adjustment, feeding systems require precise stepper rotation (PTO), and VFDs require 0–10V analog voltage setpoints.

---

## 2. Technical Implementation & How It Works (بتشتغل ازاي)
* **Code Location**: `HAL/pto_motion.c`, `HAL/pwm_controller.c`, `HAL/analog_010v.c`, `HAL/dac_420ma.c`, `APP/main_kontrx.c`, `APP/freertos_tasks.c`.
* **Execution Details**:
  * **Relays & Digital Out**: Driven via direct GPIO `BSRR` register writes for jitter-free single-cycle toggling.
  * **PWM Control**: Configured on TIM4 Channels 1 & 2 (`PD12`/`PD13`), generating 0–100% duty cycle with configurable frequencies up to 20 kHz.
  * **PTO Motion**: Utilizes TIM1 Channels 1–4 with dedicated GPIO direction pins (`PE8`/`PE10`/`PE12`/`PE15`) and hardware limit switch EXTI lines (`PE0`, `PE1`, `PE3`, `PE7`). Step count managed deterministically via dedicated NVIC update interrupts (`NVIC_ISER[0]`).
  * **Analog 0–10V & 4–20mA**: Configured through dedicated industrial analog front-end circuitry for proportional valve and VFD speed control.
  * **Factory Default Provisioning**: `main_kontrx.c` auto-provisions 13 default actuator slots (8 Relays + 2 PWM + 1 PTO Axis + 1 0-10V + 1 4-20mA).

---

## 3. Tests Performed & Verification (ايه الـ Tests اللي اتعملت عليها وازاي)
* **Automated Actuator Test Suite**:
  * File: `tests/test_actuators.py`
  * Execution:
    ```bash
    pytest tests/test_actuators.py -v
    ```
  * What it verifies: Actuator type registration, boundary clamps (0–100% for PWM, 4–20mA ranges, 0–10V ranges), PTO step calculation, and invalid pin rejection.
* **Pin Safety Protection Test**:
  * Attempts to register actuators on flash pins (`PB0`, `PB3–PB5`) and Ethernet pins (`PB12–PB15`). Asserts that the system rejects the configuration and logs a security error.
* **Oscilloscope Hardware Verification**:
  * Verified pulse train frequency and duty cycle on TIM1/TIM4 outputs using a 100 MHz digital storage oscilloscope.

---

## 4. System Impact & Inter-Dependencies (بتاثر علي ايه ومرتبطه بايه)
* **Impacts**:
  * `Task_ControlEngine`: Executes output commands triggered by automation rules.
  * `Task_HTTPServer`: Controlled via `/api/relay`, `/api/pwm`, `/api/pto`, `/api/010v`, `/api/420ma`.
  * `Task_MQTTClient`: Emits real-time state metrics (duty %, frequency, position, mA, V) over Sparkplug B.
* **Dependencies**: Relies on STM32F4 timer interrupts, GPIO MCAL, and peripheral safety blacklist masks.

---

## 5. Development Status & Blockers (حالتها والمعوقات)
* **Status**: ✅ **COMPLETED & OPERATIONAL (مكتملة وشغالة بالكامل)**.
* **Blockers / Known Gaps**: None. All 8 types are supported in firmware v2.2+.
