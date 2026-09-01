# Kontrx System Architecture & Technical Manual

The **Kontrx Edge Gateway** is an industrial-grade embedded gateway and distributed automation controller designed for real-time water quality monitoring, edge automation, and cloud SCADA integration.

---

## 1. Hardware Architecture

```mermaid
graph TD
    MCU["STM32F407VET6 MCU<br/>(ARM Cortex-M4F @ 168MHz, 512KB Flash, 192KB RAM)"]

    ETH["WIZnet W5500<br/>SPI Hardwired TCP/IP Ethernet"]
    RS485["MAX485 Transceiver<br/>Modbus RTU Master"]
    NOR["Winbond W25Q16<br/>2MB SPI NOR Flash"]
    RELAYS["10x Relay Outputs<br/>(GPIO Push-Pull)"]
    DEBUG["USART1 Debug Console<br/>(115200 Baud)"]
    SENSORS["RS485 Sensor Bus<br/>(pH, ORP, EC, DO, Ammonia, Ultrasonic)"]
    PLC["Remote Industrial PLCs<br/>(Modbus TCP / OPC UA)"]

    MCU <==>|SPI2 @ 21MHz<br/>PB12-PB15| ETH
    MCU <==>|USART3 + DE/RE<br/>PB10/PB11 + PD2/PD3| RS485
    MCU <==>|SPI2 Shared Bus| NOR
    MCU ==>|PE2,PE4,PE6,PC0,PC2,PA0,PA2,PA4,PC4,PD8| RELAYS
    MCU ==>|USART1 PA9/PA10| DEBUG
    RS485 <==>|Half-Duplex A/B Bus| SENSORS
    ETH <==>|RJ45 10/100 Mbps| PLC
```

---

## 2. Flash Memory Architecture & Partitioning

### 2.1 Internal STM32F407 Flash Layout (512 KB Total)

| Sector | Base Address | Size | Description |
|---|---|---|---|
| **Sector 0** | `0x08000000` | 16 KB | Stage-1 Bootloader (`Bootloader.bin`) |
| **Sector 1** | `0x08004000` | 16 KB | OTA Metadata (`OTA_Meta_t`, Magic: `0xC01D0001`) |
| **Sectors 2–5** | `0x08008000` | 224 KB | Active Production Application (`KontrxRTOS.bin`) |
| **Sectors 6–7** | `0x08040000` | 256 KB | Firmware Staging Buffer (Receives OTA uploads) |
| **Sector 7 Tail** | `0x0807C000` | 16 KB | Internal Flash Configuration Backup (`CONFIG_FLASH_ADDR`) |

---

### 2.2 External Winbond W25Q16 SPI Flash Layout (2 MB Total)

```
0x00000000 ┌────────────────────────────────────────────────────────┐
           │ Partition 0: Web Assets (512 KB)                       │
           │ (Sectors 0–127: Embedded HTML5 / CSS3 / JS Dashboard)   │
0x00080000 ├────────────────────────────────────────────────────────┤
           │ Partition 1: Redundant System Configuration (16 KB)    │
           │   - Sector 128 (0x080000): Primary Config              │
           │   - Sector 129 (0x081000): Backup Config               │
           │   - Sector 130 (0x082000): Primary Rules (v1.0.x)      │
           │   - Sector 131 (0x083000): Backup Rules (Stable)       │
0x00084000 ├────────────────────────────────────────────────────────┤
           │ Partition 2: Offline Telemetry Queue (512 KB)          │
           │ (Sectors 132–259: 32,768 Slots × 16-byte FIFO Ring)    │
0x00104000 ├────────────────────────────────────────────────────────┤
           │ Partition 3: Persistent Audit Logs (1008 KB)           │
           │ (Sectors 260–511: Wrap-around Circular Event Logger)   │
0x00200000 └────────────────────────────────────────────────────────┘
```

---

## 3. FreeRTOS Task Architecture & IPC

The gateway runs 5 specialized tasks mapped to CMSIS-RTOS2 API priorities:

```mermaid
graph TD
    subgraph Tasks["FreeRTOS Tasks"]
        CE["1. Task_ControlEngine<br/>Priority: Realtime (5)<br/>Stack: 6 KB<br/>Period: 10ms / 100Hz"]
        MQTT["2. Task_MQTTClient<br/>Priority: AboveNormal (4)<br/>Stack: 8 KB<br/>Period: Event / Interval"]
        MB["3. Task_ModbusSensorPoll<br/>Priority: Normal (3)<br/>Stack: 4 KB<br/>Period: Continuous"]
        HTTP["4. Task_HTTPServer<br/>Priority: Normal (2)<br/>Stack: 12 KB<br/>Period: Socket Event"]
        OTA["5. Task_OTAUpdate<br/>Priority: Low (1)<br/>Stack: 4 KB<br/>Period: Background"]
    end

    subgraph Synchronization["Mutexes & Semaphores"]
        SM["sensorMutex<br/>Guards Modbus_SensorData_t"]
        CM["configMutex<br/>Guards Gateway_Config_t"]
        RM["rulesMutex<br/>Guards RuleConfig_t"]
        SPIM["spiMutex<br/>Guards SPI2 Hardware Bus"]
        S_START["sem_ota_start<br/>HTTP -> OTA Trigger"]
        S_DONE["sem_ota_done<br/>OTA -> HTTP Completion"]
    end

    MB ==>|Writes Sensor Readings| SM
    CE ==>|Reads Non-Blocking Snapshot| SM
    HTTP ==>|Reads Live Status| SM
    MQTT ==>|Consumes Averaged Batch| SM

    CE ==>|Evaluates Rules| RM
    HTTP ==>|Uploads Rules| RM

    HTTP ==>|Locks SPI2 for W5500| SPIM
    MQTT ==>|Locks SPI2 for W5500| SPIM

    HTTP ==>|Signals Upload Done| S_START
    S_START ==>|Wakes Up| OTA
    OTA ==>|Signals Verification Done| S_DONE
    S_DONE ==>|Replies 200 OK| HTTP
```

---

## 4. RS485 Modbus RTU Sensor Subsystem

### 4.1 Supported Sensors & Mathematical Decoding

| Type ID | Sensor Name | Modbus Function | Start Register | Count | Raw Bytes / Format | Output Formula | Default Timeout |
|---|---|---|---|---|---|---|---|
| **1** | pH Sensor | `0x03` (Read Holding) | `0x0000` | 2 | 2x 16-bit Int | `value = reg[0]/100.0`, `temp = reg[1]/100.0` | 40 ms |
| **2** | ORP Sensor | `0x03` (Read Holding) | `0x0000` | 2 | 2x 16-bit Int | `value = (float)reg[0]`, `temp = reg[1]/100.0` | 40 ms |
| **3** | EC Sensor | `0x03` (Read Holding) | `0x0000` | 2 | 2x 16-bit Int | `value = reg[0]/10.0`, `temp = reg[1]/100.0` | 40 ms |
| **4** | DO (KWS-630) | `0x03` (Read Holding) | `0x2600` | 6 | IEEE-754 DCBA Float | `temp = DecodeFloat_DCBA(reg[0..1])`<br/>`value = DecodeFloat_DCBA(reg[4..5])` | 150 ms |
| **5** | Ammonia Sensor | `0x03` (Read Holding) | `0x0000` | 2 | 2x 16-bit Int | `value = (float)reg[0]`, `temp = reg[1]/100.0` | 40 ms |
| **6** | Single Ultrasonic | `0x03` (Read Holding) | `0x0000` | 10 | 10x 16-bit Int | `temp = reg[8]/10.0`, `value = reg[9]/10.0` | 40 ms |
| **7** | Multi-Point US (8 Ch) | `0x03` / `0x04` | `ch*0x10` | 3 | 3x 16-bit Int | `dist[ch] = (float)reg[0]` (2 channels/cycle) | 40 ms |

---

### 4.2 IEEE-754 Little-Endian (DCBA) Float Decoding

The **KWS-630 Dissolved Oxygen Sensor** encodes floating point values in **DCBA** byte order across two 16-bit Modbus registers:
```c
static float Decode_Float_DCBA(uint16_t r0, uint16_t r1) {
    union { float f; uint8_t b[4]; } u;
    u.b[0] = (uint8_t)(r0 >> 8);    /* D = LSB */
    u.b[1] = (uint8_t)(r0 & 0xFF);  /* C */
    u.b[2] = (uint8_t)(r1 >> 8);    /* B */
    u.b[3] = (uint8_t)(r1 & 0xFF);  /* A = MSB */
    return u.f;
}
```

---

## 5. Telemetry & Sparkplug B / MQTT Engine

```mermaid
sequenceDiagram
    participant Poll as Modbus Polling Task
    participant Batch as Modbus_DMA_ConsumeBatch()
    participant Enc as sparkplug_b_enc.c
    participant MQTT as MQTT Client (Socket 1)
    participant Broker as Cloud MQTT Broker
    participant Queue as W25Q16 Offline Queue

    Poll->>Poll: Accumulate sum_value & sample_count
    loop Every mqtt_interval (e.g. 5s)
        MQTT->>Batch: Read and reset sample accumulators
        Batch-->>MQTT: TelemetryBatch_t (avg_value, avg_temp)
        alt Broker Connected
            MQTT->>Enc: sparkplug_encode_ddata()
            Enc-->>MQTT: Protobuf Binary Payload
            MQTT->>Broker: MQTTPublish(QOS1, Retain=1)
            Broker-->>MQTT: PUBACK
        else Broker Disconnected
            MQTT->>Queue: Partition_Queue_Push(OfflineRecord_t)
        end
    end
    Note over MQTT,Broker: When Broker Reconnects: Flush Queue first!
    loop While Queue Count > 0
        MQTT->>Queue: Partition_Queue_Pop()
        Queue-->>MQTT: OfflineRecord_t
        MQTT->>Broker: Publish Cached Telemetry with Original Timestamps
    end
```

---

## 6. Over-The-Air (OTA) Dual-Stage Bootloader Flow

```mermaid
sequenceDiagram
    participant User as Web Dashboard / Engineer
    participant HTTP as HTTPServer Task
    participant Flash as Internal Staging Flash (0x08040000)
    participant OTA as Task_OTAUpdate
    participant Boot as Stage-1 Bootloader (0x08000000)
    participant App as App Flash (0x08008000)

    User->>HTTP: POST /api/ota/verify {"otp":"KontrxOTA2026"}
    HTTP-->>User: 200 OK (Unlocked)
    User->>HTTP: POST /update (Binary Stream)
    HTTP->>Flash: Write chunks to 0x08040000
    HTTP->>OTA: Release sem_ota_start
    OTA->>Flash: Compute CRC32 & Validate Stack Pointer
    OTA->>Flash: Write OTA_Meta_t to Sector 1 (0x08004000)
    OTA->>HTTP: Release sem_ota_done
    HTTP-->>User: 200 OK (Rebooting)
    OTA->>OTA: Trigger System Reset (SCB_AIRCR)
    
    Note over Boot: Microcontroller Boots @ 0x08000000
    Boot->>Flash: Read OTA_Meta_t from Sector 1
    alt Status == PENDING && Magic == 0xC01D0001
        Boot->>App: Erase Sectors 2, 3, 4, 5
        Boot->>App: Copy Binary from Staging (0x08040000) to App (0x08008000)
        Boot->>App: Verify Flash Word-by-Word
        Boot->>Flash: Mark Status = OK
    end
    Boot->>App: Relocate VTOR = 0x08008000 and JumpToApp()
```
