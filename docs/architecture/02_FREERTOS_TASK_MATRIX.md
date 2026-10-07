# 02 - FreeRTOS Multi-Tasking Architecture & Synchronization Matrix

## 1. Operating System Overview
KONTRX runs **FreeRTOS v10** with pre-emptive priority-based scheduling using the CMSIS-RTOS2 abstraction layer. 
The system operates 5 concurrent tasks designed with deterministic execution slices to guarantee hard real-time actuator response while concurrently servicing network requests.

---

## 2. Task Allocation & Priority Matrix

| Task Name | Priority | Stack Size (Bytes) | Execution Period | Responsibilities & Deadlines |
|---|---|---|---|---|
| **`Task_ControlEngine`** | Realtime (5) | 6,144 B | 10 ms (100 Hz) | Evaluates hierarchical condition trees, executes Action Sequences, monitors limit switches, updates PTO/PWM/Relay outputs. Must complete in < 2 ms. |
| **`Task_MQTTClient`** | High (4) | 8,192 B | Periodic / CoV | Maintains MQTT connection, encodes Sparkplug B protobuf payloads, performs offline queue batch drains, self-healing network guard. |
| **`Task_ModbusSensorPoll`** | Normal (3) | 4,096 B | ~275 ms | Round-robin RS485 polling of up to 16 sensors, DMA transfer handling, DSP outlier rejection, moving averages. |
| **`Task_HTTPServer`** | Normal (2) | 12,288 B | Event-Driven | Manages 3 concurrent sockets (Sockets 0, 3, 6), streams gzipped SPA, processes REST API commands, manages session tokens and login lockouts. |
| **`Task_OTAUpdate`** | Low (1) | 4,096 B | Event-Driven | Validates staged firmware image (CRC32, Cortex-M4 stack pointer check), coordinates worker task suspension, writes bootloader metadata. |

---

## 3. Thread Synchronization & Shared Resources

| Mutex Name | Type | Guarded Resource | Acquisition Policy & Timeout |
|---|---|---|---|
| **`spiMutex`** | Recursive Mutex | SPI2 Bus (W5500 Ethernet Controller) | Required by HTTP Server, MQTT Client, and Modbus TCP Server. Prevents interleaved SPI frames. |
| **`sensorMutex`** | Standard Mutex | `Modbus_SensorData_t` Global Cache | Modbus Poller acquires to write new averages. Control Engine acquires with zero timeout (`osMutexAcquire(0)`) to avoid priority inversion. |
| **`configMutex`** | Standard Mutex | `Gateway_Config_t` Active Configuration | Protects network, MQTT, and actuator configuration parameters during HTTP writes. |
| **`rulesMutex`** | Standard Mutex | `RuleConfig_t` Active & Probationary Rules | Prevents rule evaluation during flash commit or REST API compilation. |
| **`flashMutex`** | Standard Mutex | SPI1 Bus (Winbond W25Q16 Flash Chip) | Serializes partition writes, rule commits, canvas layout storage, and audit logs. |
| **`sdMutex`** | Standard Mutex | SPI3 Bus (MicroSD Card Socket) | Serializes FATFS file writes, log rotations, and streaming downloads. |

---

## 4. Watchdog & Task Health Supervision
1. **Hardware Independent Watchdog (IWDG)**: Clocked by internal 32 kHz LSI with a 3.2-second timeout window.
2. **Cooperative Health Flags**:
   * `Task_ControlEngine` asserts `ctrl_alive = 1` each 10 ms cycle.
   * `Task_ModbusSensorPoll` asserts `modbus_alive = 1` upon completing each sensor cycle.
   * A diagnostic monitor checks both flags before executing `IWDG_ReloadCounter()`. If any real-time loop hangs or starves, the hardware resets the controller automatically.
