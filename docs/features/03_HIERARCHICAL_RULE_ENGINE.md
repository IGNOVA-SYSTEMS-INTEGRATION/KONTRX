# Feature: Hierarchical Rule Engine & Automation Logic Subsystem

## 1. Description & Purpose (بتعمل ايه وليه اتعملت)
Provides autonomous, edge-side closed-loop control without requiring constant cloud or central server connectivity.
* **Why it was built**: Industrial automation and aquaculture processes cannot afford latency or communication dropouts when critical thresholds are breached (e.g., sudden drop in dissolved oxygen killing a fish harvest, or pH spikes destroying a hydroponic cycle). The edge controller must make instantaneous decisions (within 100 ms) and actuate physical equipment locally.

---

## 2. Technical Implementation & How It Works (بتشتغل ازاي)
* **Code Location**: `APP/freertos_tasks.c` (`Task_ControlEngine`), `APP/main_kontrx.c`, `APP/cJSON.c`.
* **Architecture & Flow**:
  1. **Thread Execution**: Runs deterministically inside `Task_ControlEngine` with a cycle period of 100 ms and a stack size of 1024 words (`configMINIMAL_STACK_SIZE * 8`).
  2. **Rule Schema**:
     * **Triggers**: Sensor value thresholds (`<`, `<=`, `>`, `>=`, `==`, `!=`), digital input states, timer intervals, or time-of-day schedules.
     * **Conditions**: Multi-condition logic gates (AND / OR) combining environmental inputs, sensor health flags, and manual override modes.
     * **Hysteresis & Debounce**: Configurable dead-band window to prevent rapid chattering of high-power contactors and relays.
     * **Actions**: Single or multiple actuator actions (Relay ON/OFF, PWM duty cycle change, PTO motor position move, 4–20mA output ramp).
     * **Priority & Hierarchies**: Rules are evaluated in ranked order. Safety interlocks (e.g., emergency stop or tank low-level cutoff) supersede normal production cycles.
  3. **Thread Safety**: Interlocks access sensor cache via `sensorMutex` and actuator registers via `actuatorMutex` to prevent race conditions with HTTP or MQTT tasks.

---

## 3. Tests Performed & Verification (ايه الـ Tests اللي اتعملت عليها وازاي)
* **Host-side Automation Logic Test**:
  * File: `tests/test_config_logic.c` & `tests/test_actuators.py`
  * Execution:
    ```bash
    pytest tests/test_actuators.py -k "rule or logic" -v
    ```
  * What it verifies: Threshold crossing triggers, logic evaluation (AND/OR), hysteresis band stability, actuator state assertion, and priority preemption.
* **Rapid Cycle Stress Test**:
  * Injected synthetic fluctuating sensor streams across threshold boundaries at 10 Hz; verified that debounce timers and hysteresis properly restrained output transitions.
* **Hardware Interlock Verification**:
  * Simulating low-water switch trip simultaneously with high-temperature alert; verified that safety interlock shuts off heating elements within < 20 ms.

---

## 4. System Impact & Inter-Dependencies (بتاثر علي ايه ومرتبطه بايه)
* **Inputs**:
  * Consumes processed data from `Task_ModbusSensorPoll` (via `sensorMutex`).
  * Consumes manual override commands from `Task_HTTPServer` and `Task_MQTTClient`.
* **Outputs**:
  * Dispatches control signals to `HAL/plc_control.c`, `HAL/pwm_controller.c`, `HAL/pto_motion.c`, `HAL/dac_420ma.c`.
* **State Emission**:
  * Writes state transitions to the event log queue for telemetry broadcast.

---

## 5. Development Status & Blockers (حالتها والمعوقات)
* **Status**: ✅ **COMPLETED & OPERATIONAL (مكتملة وتعمل في الإنتاج)**.
* **Blockers / Known Gaps**: None. Complex node-based canvas graphs serialize directly into this engine.
