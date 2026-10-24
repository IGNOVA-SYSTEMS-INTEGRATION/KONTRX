# Kontrx Controller Hardening Plan (October 2026)

Reference plan agreed with the product owner on 2026-10-24. It covers firmware
logic, memory, speed, security and UI. Each phase is its own branch from `dev`:
build, review the diff, OTA (`.claude/skills/kontrx-ota`), test on the board,
then commit and merge `--no-ff`.

## Targets

| Area | Target |
|---|---|
| MQTT message after any action (actuator change, rule, UI) | on the wire within **20 ms**, ideally a few ms |
| Actuator operations (on/off, value change) | under **20 ms** |
| UI actions | under **500 ms** |
| Sensor reading | each sensor at most **100 ms** apart, without slowing the others |
| Resets | never, except the reset that applies an OTA |

## Decisions

| Topic | Decision |
|---|---|
| Watchdog | No software watchdog. A power-supply hardware watchdog exists but is treated as absent. |
| Faults | Degraded mode instead of reset: suspend only the faulty task, outputs keep their state, Control Engine and rules keep running, every event logged. |
| Sensor offline | Offline after **10 consecutive failed reads**. Without bypass, rules using it are skipped (no action, no auto-reverse); outputs hold. With bypass, values read as 0 and rules continue (UI shows a bypass badge, event logged). |
| Sequences on sensor loss | Follow the sequence's own `on_false` (finish cycle / hold / abort). HOLD must truly hold. |
| Test mode | Behaviour unchanged. When test mode turns OFF, rule timers and sequences reset. |
| MQTT commands | Removed completely (no subscriptions). Telemetry publishing unchanged (on-change / interval). |
| Sparkplug | NBIRTH always sent on connect (seq 0, with bdSeq). No Rebirth handling (own Mosquitto/EMQX host). |
| Device identity | Default IDs derived from the MCU 96-bit UID (no hard-coded UUIDs). |
| Restart | Every actuator restores its last state (state journal on the W25Q). |
| Pins | One owner per pin. Firmware rejects conflicts; UI lists only free pins and suggests a free one. Peripheral pins (SPI3 4-20 mA DAC, PWM, PTO, 0-10 V) are protected. Relay pins PE2, PE4, PE6, PC0, PC2, PA0, PA2, PA4, PC4, PD8 stay as they are. The 4-20 mA output is the external MCP4922 on SPI3 (PC10-12, CS PC1/3/5/13), not PA4. |
| Firmware backup | Only the **last stable** firmware is kept (W25Q). "Boot OK" is automatic; "Stable" is set manually by the owner after testing ("Mark as stable"). Failed boot of a new image rolls back on the next reset; "Rollback to stable" available in the UI. |
| Time | SNTP: local network server first, then internet, then browser time at login. |
| Debug UART | 115200 baud. |
| Sensor comms | Per-sensor settings in the config: baud, parity, stop bits, timeout, poll interval, filter, enabled. Controller switches baud per device on the shared bus. Real interval per sensor shown in the UI with a warning if a setting cannot be met. |

## Sensor facts (Kacise manuals)

| Sensor | Baud options | Default | Note |
|---|---|---|---|
| pH KWS-790, ORP KWS-590, EC KWS-390, Ammonia KWS-290 | 1200-57600 | 9600 8N1 | baud/address changed via function 6 (FC10, R12-R14) |
| DO KWS-630 | 9600 only | 9600 8N1 | physical response time 60 s; default poll 1000 ms |

RS-485 supports about 100 kbps over 1200 m with twisted pair, 120 ohm
termination at both ends and correct biasing.

## Phases (in order)

### 0. FC04 multi-sensor map (FW 2.2.2)
Decode FC04 input registers into `summary_decoded`, show total flow / volume /
uptime on the card.

### 3c. Memory and fault handling (first: reset risks)
- Rules upload parser that uses no heap (today cJSON needs 15-45 KB, heap free is ~5.7 KB, and the malloc-failed hook resets).
- Malloc-failed and stack-overflow hooks: no reset, no hang; degraded mode plus log.
- Heap and per-task stack high-water marks in `/api/status`.
- Merge duplicate RAM copies (7 rules copies, 9 config copies; ~25 KB).
- Shrink `g_mqtt_status` (17 KB).
- HTTP: 413 for bodies over the buffer; case-insensitive `Content-Length`.
- Restore auth on `POST /api/rules`.
- Bootloader: last-stable backup on W25Q, boot-OK confirmation, CRC check before erasing, rollback, no `while(1)` halts.
- SNTP + clock source (HSE/HSI) in status.
- Debug UART 115200; fix log ordering after wrap.
- `docs/SAFETY_BEHAVIOR.md`: every failure case and what outputs, rules and logs do.

### 3a. Rules correctness
- Outputs written only when their value changes (fixes PTO moving forever, PWM/DAC re-programmed every cycle).
- Offline detection (10 failures) and skip-on-offline; no auto-reverse on sensor loss; bypass badge.
- Sequences per `on_false`; real HOLD.
- Timers inside AND/OR evaluated every cycle (no short-circuit skipping).
- Validate rules on upload: no cycles, bounded depth, names resolved to fixed IDs (no "first digit" fallback), sane hysteresis, no float `==`.
- Remove MQTT command path.
- Reset rule state when test mode turns OFF; `ControlEngine_ResetStates` race.

### 3b. Pins and state
- Complete pin ownership table in firmware, exposed via API; UI uses it.
- Conflict handling on save and at boot (critical function keeps the pin, the other actuator is disabled and flagged).
- Last-state restore journal; NC relays driven to the right level at boot.
- Runtime state stored apart from the config (no lost updates from UI saves).
- Correct 4-20 mA labels; all 8 channels.

### 3d. Sensor speed
- Remove the 350 ms idle; FC04 summary every cycle, detail blocks in rotation.
- Per-sensor comms settings with a config migration (no config loss on OTA).
- Per-device baud switching; 3.5-character gaps per baud.
- Backoff for unreachable sensors (up to 60 s).
- Filter per sensor: none / fast (median-3) / smooth; fix mm units of the jump limit.
- Measure Kacise reply times; test the Blue Pill slave at 19200 and 38400.
- Scan across baud rates.

### 6. Speed
- Control Engine every 5 ms, woken at once by events.
- MQTT: task notified on every action; small event message; QoS1 without blocking on PUBACK; measured with DWT and a broker-side subscriber.
- Sparkplug compliance: seq 0-255 with NBIRTH = 0 and reset on reconnect, bdSeq, DBIRTH, protobuf NDEATH; UID-based IDs.
- W5500 SPI with DMA at 21 MHz.
- UI: prioritised request queue, no loader for actions, `/api/live` (~500 B) polled about every 500 ms, partial card updates, versioned page cache.
- Log index (log page is ~735 ms today and blocks every other HTTP request).

### 5. Security
- Session tokens from the hardware RNG, several sessions with expiry.
- Auth on `POST /api/provision` and Modbus TCP writes.
- Per-device OTA secret instead of the fixed OTP; forced change of the default password; auth on GET endpoints.

### 7. UI
- One outline SVG icon set; no emojis or symbols.
- Replace native `alert`/`confirm`.
- Page-by-page form review.

### 4. MISRA C:2012
Required rules first (17.3 implicit declarations, 17.2 recursion), then the rest.

### Tests
Host unit tests, fuzzing, Sparkplug checks, a write-feature test script run by
the owner, and a 72-hour soak test.

## Baseline (FW 2.2.1, before phases 1-2) and after phase 2

| Measurement | 2.2.1 | After phase 2 |
|---|---|---|
| Modbus TCP FC03 p50 / max | 17.8 / 58.7 ms | 5.7 / 15.7 ms |
| HTTP `/api/status` p50 | 67.8 ms | 48.8 ms |
| heap_free | 6496 B | 5736 B |
