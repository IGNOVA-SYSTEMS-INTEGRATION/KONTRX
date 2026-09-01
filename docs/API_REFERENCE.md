# Kontrx REST API Reference Manual

The **Kontrx Edge Gateway** runs an embedded HTTP server on **Port 80** over the W5500 Ethernet controller (Socket 0). It exposes a comprehensive RESTful API for live telemetry monitoring, actuator control, peripheral auto-discovery, rule automation, configuration management, and Over-The-Air (OTA) firmware updates.

All JSON responses include Cross-Origin Resource Sharing (`CORS`) headers:
```http
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type
```

---

## 1. System & Live Telemetry APIs

### `GET /api/status`
Returns a real-time JSON snapshot of the gateway, including all configured Modbus sensors, actuator states, MQTT connection status, system health, and the full QR provisioning payload.

* **Method**: `GET`
* **Response Code**: `200 OK`
* **Example Response**:
```json
{
  "device_id": "KX-0000002",
  "fw": "1.1.0",
  "uptime": 1420,
  "cpu": 12,
  "free_heap": 34816,
  "total_heap": 45056,
  "ip": "192.168.1.200",
  "netmask": "255.255.255.0",
  "gateway": "192.168.1.1",
  "mac": "00:08:DC:11:22:33",
  "mqtt_connected": 1,
  "mqtt_broker": "broker.emqx.io",
  "mqtt_port": 1883,
  "mqtt_topic": "spBv1.0/KontrxGroup/DDATA/kontrx-0000002",
  "sensors": [
    { "type": "ph", "id": 1, "value": 7.42, "temp": 25.10, "valid": 1 },
    { "type": "ec", "id": 3, "value": 1420.00, "temp": 25.10, "valid": 1 },
    { "type": "do", "id": 4, "value": 6.85, "temp": 24.80, "valid": 1 }
  ],
  "relays": [
    { "id": 0, "name": "Pump_Main", "state": 1, "port": "PE", "pin": 2, "nc": 0, "type": 0 },
    { "id": 1, "name": "Valve_Inlet", "state": 0, "port": "PE", "pin": 4, "nc": 0, "type": 0 }
  ],
  "qrData": "{\"id\":\"KX-0000002\",\"mac\":\"00:08:DC:11:22:33\",\"ip\":\"192.168.1.200\",\"sensors\":{\"ph\":7.42,\"ec\":1420.0,\"do\":6.85},\"relays\":[1,0,0,0,0,0,0,0,0,0]}"
}
```

---

### `GET /api/hardware`
Returns available GPIO ports, pin constraints, and reserved pin assignments.

* **Method**: `GET`
* **Response Code**: `200 OK`
* **Example Response**:
```json
{
  "ports": ["PA", "PB", "PC", "PD", "PE"],
  "reserved_pins": [
    { "port": "PA", "pins": [6, 9, 10], "reason": "LED / USART1 Debug" },
    { "port": "PB", "pins": [10, 11, 12, 13, 14, 15], "reason": "Modbus USART3 / W5500 SPI2" },
    { "port": "PD", "pins": [2, 3], "reason": "MAX485 RE/DE Control" }
  ]
}
```

---

### `GET /api/logs`
Streams active system events from the 16 KB circular RAM buffer and W25Q16 flash log partition.

* **Method**: `GET`
* **Response Code**: `200 OK`
* **Example Response**:
```json
{
  "logs": [
    { "uptime": 12, "cat": "SYS", "msg": "System initialized and booted successfully." },
    { "uptime": 14, "cat": "MODBUS", "msg": "Sensor pH (ID 1) connected." },
    { "uptime": 18, "cat": "MQTT", "msg": "TCP connection established." },
    { "uptime": 19, "cat": "MQTT", "msg": "MQTT session connected successfully." }
  ]
}
```

---

### `POST /api/logs/clear`
Clears the circular RAM log buffer.

* **Method**: `POST`
* **Response Code**: `200 OK`
* **Response**: `{"status":"ok"}`

---

## 2. Actuator & Relay Control APIs

### `POST /api/relay?id={0..15}&state={0|1}`
Toggles the state of a specific actuator.

* **Method**: `POST`
* **Query Parameters**:
  - `id`: Actuator index (0 to `MAX_RELAYS - 1`).
  - `state`: `1` for ON (Energized), `0` for OFF (De-energized).
* **Example**:
```bash
curl -X POST "http://192.168.1.200/api/relay?id=0&state=1"
```
* **Response Code**: `200 OK`
* **Response**: `{"status":"ok","id":0,"state":1}`

---

### `POST /api/relay/all?state={0|1}`
Switches all configured actuators simultaneously.

* **Method**: `POST`
* **Query Parameters**:
  - `state`: `1` (all ON) or `0` (all OFF).
* **Example**:
```bash
curl -X POST "http://192.168.1.200/api/relay/all?state=0"
```
* **Response Code**: `200 OK`
* **Response**: `{"status":"ok","state":0}`

---

### `POST /api/config/relays`
Replaces the entire actuator configuration list and persists changes to Flash.

* **Method**: `POST`
* **Payload**:
```json
{
  "actuators": [
    {
      "id": 0,
      "name": "Pump_Main",
      "type": 0,
      "port_or_ip": "PE",
      "pin_or_slave": 2,
      "is_active_low": 0
    },
    {
      "id": 1,
      "name": "PLC_Valve_1",
      "type": 1,
      "port_or_ip": "192.168.1.50",
      "port": 502,
      "pin_or_slave": 1,
      "reg_addr": 100,
      "is_active_low": 0
    }
  ]
}
```
* **Response Code**: `200 OK`
* **Response**: `{"status":"ok","count":2}`

---

## 3. Sensor & Modbus Discovery APIs

### `POST /api/config/sensors`
Configures the active Modbus RTU sensor inventory.

* **Method**: `POST`
* **Payload**:
```json
{
  "sensors": [
    { "type": 1, "id": 1 },
    { "type": 3, "id": 3 },
    { "type": 4, "id": 4 }
  ]
}
```
* **Response Code**: `200 OK`
* **Response**: `{"status":"ok","count":3}`

---

### `POST /api/modbus/scan`
Triggers an asynchronous auto-discovery scan across Modbus slave addresses 1 to 247.

* **Method**: `POST`
* **Response Code**: `200 OK`
* **Response**: `{"status":"scanning_started"}`

---

### `GET /api/modbus/scan`
Polls the progress and results of the active Modbus auto-discovery scan.

* **Method**: `GET`
* **Response Code**: `200 OK`
* **Example Response**:
```json
{
  "scanning": 0,
  "progress": 100,
  "count": 2,
  "devices": [
    { "id": 1, "type": "ph" },
    { "id": 4, "type": "do" }
  ]
}
```

---

## 4. Rule Engine & Automation APIs

### `GET /api/rules`
Fetches the currently active automation rules from RAM.

* **Method**: `GET`
* **Response Code**: `200 OK`
* **Example Response**:
```json
{
  "version_id": "v1.0.4",
  "timestamp": "2026-09-01T12:00:00Z",
  "rules_valid": 1,
  "bypass_validation": 0,
  "rules": [
    {
      "rule_id": "r1",
      "input_id": "ph",
      "operator": "<",
      "threshold": 6.5,
      "output_id": "Pump_Main",
      "action": "ON",
      "active": 1
    }
  ]
}
```

---

### `POST /api/rules`
Uploads a new rule set. Rules enter a 30-second probationary watchdog test.

* **Method**: `POST`
* **Payload**:
```json
{
  "version_id": "v1.0.5",
  "timestamp": "2026-09-01T12:15:00Z",
  "rules": [
    {
      "rule_id": "r1",
      "input_id": "ph",
      "operator": ">",
      "threshold": 8.0,
      "output_id": "Valve_Inlet",
      "action": "ON",
      "active": 1
    }
  ]
}
```
* **Response Code**: `200 OK`
* **Response**: `{"status":"ok","testing":1,"version":"v1.0.5"}`

---

### `POST /api/rules/accept`
Manually validates and confirms the current rules version, committing it to primary flash.

* **Method**: `POST`
* **Response Code**: `200 OK`
* **Response**: `{"status":"ok","rules_valid":1}`

---

### `POST /api/rules/reject`
Rejects the probationary rules and rolls back to the last known stable backup.

* **Method**: `POST`
* **Response Code**: `200 OK`
* **Response**: `{"status":"ok","rolled_back":1}`

---

## 5. MQTT & Cloud Telemetry APIs

### `POST /api/config/mqtt`
Configures MQTT broker connection parameters and telemetry schema mappings.

* **Method**: `POST`
* **Payload**:
```json
{
  "broker": "broker.emqx.io",
  "port": 1883,
  "client_id": "kontrx-gw-01",
  "username": "",
  "password": "",
  "topic": "spBv1.0/KontrxGroup/DDATA/kontrx-0000002",
  "interval": 5,
  "send_mode": 0,
  "mappings": [
    { "source_type": 0, "source_id": 1, "json_key": "ph", "enabled": 1 },
    { "source_type": 0, "source_id": 3, "json_key": "ec", "enabled": 1 },
    { "source_type": 1, "source_id": 0, "json_key": "pump_state", "enabled": 1 }
  ]
}
```
* **Response Code**: `200 OK`
* **Response**: `{"status":"ok"}`

---

## 6. Over-The-Air (OTA) Firmware Update APIs

### `POST /api/ota/verify`
Authenticates the user using One-Time Password (`OTP`) before unlocking firmware staging flash.

* **Method**: `POST`
* **Payload**: `{"otp":"KontrxOTA2026"}`
* **Response Code**: `200 OK`
* **Response**: `{"status":"unlocked"}`

---

### `POST /api/ota/prepare`
Erases internal flash staging sectors (Sectors 6 & 7) to prepare for binary stream.

* **Method**: `POST`
* **Payload**: `{"fw_size": 184320}`
* **Response Code**: `200 OK`
* **Response**: `{"status":"ready"}`

---

### `POST /update`
Streams raw binary firmware (`KontrxRTOS.bin`) to staging flash.

* **Method**: `POST`
* **Headers**: `Content-Type: application/octet-stream`
* **Payload**: Raw binary data.
* **Response Code**: `200 OK`
* **Response**: `{"status":"success","action":"rebooting"}`
