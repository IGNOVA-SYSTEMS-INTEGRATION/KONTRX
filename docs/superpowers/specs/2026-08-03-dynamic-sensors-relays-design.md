# Dynamic Sensors & Relays — Design

Date: 2026-08-03
Branch: `feature/dynamic-sensors-relays`

## Problem

Today the gateway supports exactly 7 fixed sensor slots (`sensor_ids[8]`) and 10 fixed
relays (`MAX_RELAYS 10`). Users cannot add/remove sensors or relays from the dashboard,
cannot have more than one sensor of the same type, and the Auto-Scan feature only displays
detected devices — it never writes the IDs back into config.

Goal: a fully dynamic, user-editable sensor and relay inventory, driven from the dashboard
UI, with Auto-Scan → confirm → apply.

## Requirements (agreed)

- Sensor list is dynamic: up to **16 sensors**, any mix of the 7 known types
  (1=pH, 2=ORP, 3=EC, 4=DO, 5=Ammonia, 6=Ultrasonic, 7=Multi-US). Multiple sensors of the
  same type allowed.
- Relay list is dynamic: up to **16 relays**, each with editable `name`, `pin`, `nc`, live
  `state`.
- Dashboard UI:
  - Sensors tab: dynamic table (type dropdown + Modbus ID + live value), Add Sensor, Delete.
  - Auto-Scan: button → progress → detected devices (from existing `POST/GET /api/modbus/scan`)
    → user selects which to apply (checkbox) → confirm → adds rows.
  - Relays tab: dynamic table (name + pin + NC/NO + toggle), Add Relay, Delete, with
    reserved-pin validation (existing `RESERVED_PINS` logic reused).
  - Value cards and the QR payload are built dynamically from the returned JSON.
- Persist new config to flash Sector 11, bump `magic` so stale persisted config is ignored.

## Architecture (Approach A — full dynamic restructure)

### Data model (`HAL/modbus_dma.h`)

```c
#define MAX_SENSORS 16
#define MAX_RELAYS  16
#define MAX_MULTI_US 4

/* sensor type: 1=pH 2=ORP 3=EC 4=DO 5=Ammonia 6=Ultrasonic 7=Multi-US */
typedef struct { uint8_t type; uint8_t id; } SensorEntry_t;
typedef struct { uint8_t count; SensorEntry_t entries[MAX_SENSORS]; } SensorList_t;

typedef struct {
    uint8_t port_id;   /* 0=A..4=E */
    uint8_t pin_num;   /* 0..15    */
    uint8_t is_nc;
    char     name[20]; /* editable label */
    uint8_t  state;    /* runtime */
} Relay_Config_t;

/* dynamic per-sensor reading */
typedef struct {
    uint8_t type;
    uint8_t id;
    float   value;
    float   temp;
    uint8_t valid;
} SensorReading_t;

/* multi-US board holds 8 distances + avg/comp/temp */
typedef struct {
    float dist[8];
    float avg, comp, temp;
    uint8_t id;
} MultiUS_t;

typedef struct {
    SensorReading_t readings[MAX_SENSORS];
    uint8_t readings_count;
    MultiUS_t multi_us[MAX_MULTI_US];
    uint8_t multi_us_count;
    uint32_t last_update_time;
} Modbus_SensorData_t;
```

`Gateway_Config_t` gains:
```c
    SensorList_t sensors;
    Relay_Config_t relays[MAX_RELAYS];
    uint8_t relay_count;
    /* magic bumped to 0xC01D0002 */
```
Legacy fields (`sensor_ids[8]`) removed. `MAX_RELAYS` changed 10 → 16.

### Polling (`HAL/modbus_dma.c`)

`Modbus_DMA_PollSensors()` rewritten:
- Snapshot config (`Get_Shared_Config`).
- Loop `cfg.sensors.entries[i]` for `i < cfg.sensors.count`, `switch(type)`:
  - pH: `0x0000,2` → value=rs0/100, temp=rs1/100
  - EC: `0x0000,2` → value=rs0/10, temp=rs1/100
  - ORP: `0x0000,2` → value=rs0, temp=rs1/100
  - Ammonia: `0x0000,2` → value=rs0, temp=rs1/100
  - DO: `0x2600,6` → temp=DecodeFloat_DCBA(rs0,rs1), value=DecodeFloat_DCBA(rs4,rs5)
  - Ultrasonic: `0x0000,10` → temp=rs8/10, dist=rs9/10
  - Multi-US: 8×`i*0x10,3` dists + `0x0080,3` temp + `0x0000,3` fc04 avg/comp
- Write into `readings[]` (and `multi_us[]` for type 7). Same mutex guards.
- 50ms delay between probes (existing pattern).

### HTTP API (`APP/http_server_task.c`)

- `GET /api/status`:
  - `"sensors":[{"type":"ph","id":2,"value":7.1,"temp":25.0,"valid":1}, ...]` (dynamic)
  - `"relays":[{"name":"Pump1","pin":"PE2","nc":0,"state":0}, ...]` (dynamic, `relay_count` items)
  - plus existing `serial`, `ip`, `fw`, `uptime_s`, `mac`.
- `POST /api/config/sensors` → **full replace**: `{"sensors":[{"type":"ph","id":2}, ...]}`
  - validates: 1..16 entries, id 1..247, unique id within list.
- `POST /api/config/relays` → **full replace**: `{"relays":[{"name":"Pump1","pin":"PE2","nc":0}, ...]}`
  - validates: 1..16 entries, pin format, unique pin, not reserved.
- `POST /api/relay?id=N&state=S` and `POST /api/relay/all` — unchanged (toggle by index).
- `POST/GET /api/modbus/scan` — unchanged (UI drives apply locally).

### Dashboard (`APP/web_assets.h`)

- Sensors page: dynamic rows built from `d.sensors` (each row: type `<select>`, id `<input>`,
  live value). `addSensorRow()`, `deleteSensorRow(i)`, `saveSensors()` → full-replace POST.
- Auto-Scan section: existing scan progress + results; each result gets a checkbox;
  "Apply Selected" appends rows for checked devices.
- Relays page: dynamic rows (name input, pin input, NC/NO select, toggle). `addRelayRow()`,
  `deleteRelayRow(i)`, `saveRelays()` → full-replace POST. Reserved-pin validation reused.
- Value cards: generated in `fetchStatus` from `d.sensors` dynamically.
- `generateQR`: sensors built from `d.sensors` (id+type), relays from `d.relays`
  (name+pin+nc). No timestamp. QR stays stable unless config changes.

### Migration & defaults

- Config loader: if `magic != 0xC01D0002` or checksum mismatch → init defaults:
  sensors = [] (empty), relays = default 10 (PE2,PE4,PE6,PC0,PC2,PA0,PA2,PA4,PC4,PD8).
  Empty sensor list means no Modbus polling until user adds sensors or runs Auto-Scan.
- User is warned: existing saved config is discarded on first boot after this update.

## Error handling

- Invalid POST bodies → HTTP 400; field-level validation messages shown in UI.
- Scan while polling: existing `g_scan_status.is_scanning` guard (poll task enters scan mode).

## Testing

- Rebuild with `arm-none-eabi-gcc` (CMake) — all targets compile.
- Extract embedded JS and syntax-check; verify JSON shape produced by handlers.
- Manual: dashboard add/edit/delete sensors + relays, Auto-Scan apply, QR reflects list.
