# 04 - Industrial Communication Stack & Networking Subsystems

## 1. Overview
KONTRX operates a multi-protocol communication stack designed to integrate field instruments, SCADA systems, web browsers, and enterprise cloud brokers concurrently.

---

## 2. Hardwired Industrial Ethernet (WIZnet W5500)
* **Architecture**: Hardwired TCP/IP chip with embedded PHY, MAC, and 32 KB internal buffer memory.
* **Interface**: SPI2 operating at 21 MHz (PB12 CS, PB13 SCK, PB14 MISO, PB15 MOSI).
* **Socket Allocation**:
  * **Socket 0, 3, 6**: Dedicated to concurrent HTTP Server requests.
  * **Socket 1**: MQTT / Sparkplug B client session.
  * **Socket 2**: Modbus TCP Server listening on Port 502.
  * **Socket 4**: Modbus TCP Client for remote PLC polling.
  * **Socket 5, 7**: Reserved for OTA binary streaming and NTP time sync.
* **Self-Healing Guard**:
  * `Ensure_W5500_Network_Alive` executes every 5 seconds.
  * Detects physical cable unplug/replug, SPI register corruption, or socket stalls and re-initializes network parameters automatically.

---

## 3. RS485 Modbus RTU Subsystem
* **Transceiver**: MAX485 half-duplex driver connected to USART3 (PB10 TX, PB11 RX).
* **Hardware Direction Control**: Dedicated GPIOs `PD3` (DE - Driver Enable) and `PD2` (RE# - Receiver Enable).
* **Baud Rate**: 9600 baud, 8 data bits, no parity, 1 stop bit (8N1).
* **DSP Signal Conditioning**:
  * Automatic DCBA byte-swap float decoding for industrial transmitters.
  * Windowed moving-average filter and median spike rejection in `dsp_filter.c`.

---

## 4. Modbus TCP Server (Port 502)
* Listens on standard industrial port 502 for connections from SCADA packages (Ignition, Wonderware, Node-RED).
* **Supported Function Codes**:
  * FC3: Read Holding Registers (reads live sensor telemetry).
  * FC5: Write Single Coil (commands relay outputs).
  * FC6: Write Single Register (commands analog output setpoints).
  * FC16: Write Multiple Registers.
* **Test Mode Interlock**: All external write commands are rejected unless `test_mode == 1` in configuration, preventing external SCADA overrides from interfering with critical local automation rules.

---

## 5. Sparkplug B / MQTT Client Engine
* Connects to enterprise brokers (Mosquitto, EMQX, HiveMQ) using standard MQTT 3.1.1.
* Encodes payloads according to the **Eclipse Sparkplug B** specification in binary protobuf.
* Manages device lifecycle states: `NBIRTH`, `NDEATH`, `DDATA`, and `DCMD`.
