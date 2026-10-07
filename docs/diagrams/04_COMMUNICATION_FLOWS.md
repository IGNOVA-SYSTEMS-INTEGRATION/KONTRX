# System Communication Sequence Flows

## 1. End-to-End Telemetry Acquisition & Sparkplug B Publication Flow

```mermaid
sequenceDiagram
    autonumber
    participant Sensor as Modbus Sensor (RS485)
    participant TaskModbus as Task_ModbusSensorPoll
    participant DSP as DSP Filter Engine
    participant Cache as Thread-Safe Sensor Cache
    participant TaskMQTT as Task_MQTTClient
    participant Sparkplug as Sparkplug B Micro-Encoder
    participant W5500 as W5500 SPI Ethernet
    participant Broker as MQTT Broker (Ignition/EMQX)

    loop Every 275 ms (Round Robin)
        TaskModbus->>Sensor: Transmit Modbus Frame via USART3 DMA (DE High)
        Sensor-->>TaskModbus: DMA RX Response (Float/Integer registers)
        TaskModbus->>DSP: Submit raw sample (Moving Average + Median)
        DSP-->>TaskModbus: Filtered stabilized float
        TaskModbus->>Cache: Acquire sensorMutex & update channel record
        TaskModbus->>Cache: Release sensorMutex
    end

    loop Every 1000 ms (Publish Cycle)
        TaskMQTT->>Cache: Acquire sensorMutex & snapshot batch
        TaskMQTT->>Cache: Release sensorMutex
        TaskMQTT->>Sparkplug: sparkplug_encode_ddata(&buf, batch, relays)
        Sparkplug-->>TaskMQTT: Protobuf binary payload (~210 bytes)
        TaskMQTT->>W5500: Write payload to Socket 1 TX buffer
        W5500->>Broker: TCP MQTT PUBLISH: spBv1.0/KONTRX/DDATA/EDGE_01
        Broker-->>W5500: MQTT PUBACK
    end
```

---

## 2. Real-Time Automation Closed-Loop Control Sequence

```mermaid
sequenceDiagram
    autonumber
    participant Engine as Task_ControlEngine (100ms)
    participant Cache as Sensor Cache
    participant Rules as Active Rule Table (W25Q16)
    participant Relays as Relay Hardware Driver
    participant PWM as TIM4 PWM Hardware
    participant PTO as TIM1 PTO Stepper Motion
    participant Logger as Event Logger (SD/Flash)

    loop Deterministic Evaluation (100 ms)
        Engine->>Cache: Read latest sensor metrics
        Engine->>Rules: Iterate rules in ranked priority order
        Note over Engine,Rules: Evaluate Condition Nodes (TON delay, Range, Hysteresis)
        
        alt Threshold Breached & Hysteresis Cleared
            Engine->>Relays: Set GPIO BSRR (Turn Relay ON/OFF)
            Engine->>PWM: Set TIM4->CCR (Modulate Duty Cycle %)
            Engine->>PTO: Assert Step/Dir Pulse Train
            Engine->>Logger: Append state transition event with Epoch timestamp
        else Within Deadband
            Note over Engine: Hold current actuator state (No Chattering)
        end
    end
```

---

## 3. Embedded Web Dashboard HTTP Request Lifecycle

```mermaid
sequenceDiagram
    autonumber
    participant Browser as Web Browser Client
    participant W5500 as W5500 Socket 0 (Port 80)
    participant TaskHTTP as Task_HTTPServer
    participant Auth as Session Manager
    participant CCM as CCMRAM DMA TX Buffer
    participant Cache as Telemetry & Relay Cache

    Browser->>W5500: GET /api/status HTTP/1.1 (Cookie: session_id=...)
    W5500->>TaskHTTP: Sn_IR_RECV Interrupt posted
    TaskHTTP->>Auth: Validate session token
    Auth-->>TaskHTTP: Authenticated (Admin)
    TaskHTTP->>Cache: Acquire sensorMutex & actuatorMutex
    Cache-->>TaskHTTP: Snapshot system variables
    TaskHTTP->>CCM: Serialize JSON payload in CCMRAM (Zero Heap Allocation)
    TaskHTTP->>W5500: Burst stream 12 KB buffer via SPI2 DMA
    W5500-->>Browser: HTTP/1.1 200 OK (application/json)
```
