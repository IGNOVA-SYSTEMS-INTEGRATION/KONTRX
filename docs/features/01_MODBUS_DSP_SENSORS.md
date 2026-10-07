# Feature: Modbus RTU Sensor Subsystem & DSP Signal Conditioning

## 1. Description & Purpose (بتعمل ايه وليه اتعملت)
Provides continuous, noise-resilient acquisition of physical water quality and environmental parameters (pH, ORP, Electrical Conductivity, Dissolved Oxygen, Ammonia, Ultrasonic water level) from industrial RS485 transmitters.
* **Why it was built**: Industrial fish farms, hydroponics, and chemical plants generate significant electromagnetic interference (EMI) from pump contactors and aerator VFDs. Raw sensor readings oscillate wildly without hardware direction timing and DSP filtering.

---

## 2. Technical Implementation & How It Works (بتشتغل ازاي)
* **Code Location**: `HAL/modbus_dma.c`, `HAL/modbus_dma.h`, `APP/dsp_filter.c`, `APP/freertos_tasks.c`.
* **State Machine Cycle**:
  1. `Task_ModbusSensorPoll` cycles every ~275 ms round-robin through up to 16 configured sensors.
  2. Drives MAX485 `DE/RE#` lines high via direct GPIO register writes to transmit Modbus request frame with CRC16.
  3. Drives `DE/RE#` low and enables DMA receive on USART3.
  4. Decodes responses based on register profile:
     * Standard 16-bit integer registers (scaled by /10 or /100).
     * Industrial IEEE-754 32-bit floats with DCBA byte swapping (KWS-630 Dissolved Oxygen sensors).
  5. Passes raw values to `dsp_filter.c`:
     * Windowed Moving Average (smooths electrical ripple).
     * Median Filter (rejects transient spikes caused by motor starts).
     * Rate-of-Change Outlier Filter (discards impossible physical jumps).
  6. Updates global thread-safe cache `Modbus_SensorData_t` guarded by `sensorMutex`.

---

## 3. Tests Performed & Verification (ايه الـ Tests اللي اتعملت عليها وازاي)
* **Host-side Automated Protocol Test**:
  * File: `tests/test_modbus_protocol.py`
  * Execution:
    ```bash
    pytest tests/test_modbus_protocol.py -v
    ```
  * What it verifies: CRC16 calculation, DCBA float byte-swap conversion, frame timeout handling, and malformed packet rejection.
* **DSP Filter Bench Test**:
  * Simulates noisy square wave and impulse spike inputs, asserting that median filtering eliminates single-sample spikes and moving average converges within 5 samples.
* **Hardware-in-the-Loop (HIL) Test**:
  * Tested on live RS485 bus with 4 physical sensors (pH, EC, DO, Temperature) connected over 50 meters of twisted pair cable with 120-ohm termination resistors.

---

## 4. System Impact & Inter-Dependencies (بتاثر علي ايه ومرتبطه بايه)
* **Feeds**:
  * `Task_ControlEngine`: Reads sensor values to evaluate automation rules (e.g. oxygen below 4.5 mg/L).
  * `Task_MQTTClient`: Transmits sensor values in Sparkplug B DDATA packets.
  * `Task_HTTPServer`: Displays live telemetry on the web dashboard `/api/status`.
* **Dependencies**: Requires USART3 MCAL driver, MAX485 direction GPIOs (`PD2`/`PD3`), and `sensorMutex`.

---

## 5. Development Status & Blockers (حالتها والمعوقات)
* **Status**: ✅ **COMPLETED & PRODUCTION STABLE (مكتملة ومستقرة تماماً)**.
* **Blockers / Known Gaps**: None. Fully deployed in firmware v2.2+.
