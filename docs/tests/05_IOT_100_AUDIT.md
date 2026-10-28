# IoT 100-Issue Audit — Kontrx v2.2.2

| | |
|---|---|
| **Date** | 2026-10-28 |
| **Branch** | `docs/iot-100-audit` (base `dev` @ `e064fb9`) |
| **Board under test** | 192.168.1.200, fw `2.2.2`, uptime ~19 min, `test_mode=1` |
| **Scope** | Phase 1 = audit + report only. No firmware code was changed. |
| **Regression guard** | [`tests/test_iot_audit.py`](../../tests/test_iot_audit.py) — one check per verifiable item. Open findings are `xfail(strict=True)` so a fix flips them and forces the marker's removal. |

## 1. Summary

| Status | Count |
|---|---|
| ✅ PASS | 37 |
| ⚠️ PARTIAL | 23 |
| ❌ FAIL | 33 |
| 🔍 NOT VERIFIED (needs HIL / destructive test) | 6 |
| N/A | 1 |
| **Total** | **100** |

| Severity of FAIL + PARTIAL | Count |
|---|---|
| Critical | 6 |
| High | 14 |
| Medium | 22 |
| Low | 14 |

### Top 10 to fix first

| # | ID | Finding | Why it matters |
|---|---|---|---|
| 1 | SEC-09 | ~15 state-changing endpoints have no auth (`/api/rules/*`, `/api/provision`, `/api/layout`, `/api/interfaces/enable`, `/api/sensor/mode`, log/queue clear) | Anyone on the LAN (or a web page via CSRF) can rewrite control rules or redirect MQTT to their broker |
| 2 | SEC-02 | Backdoor passwords: SD unlock always accepts `adminkontrx` / `KontrxMaster2026`; OTA OTP is the fixed string `KontrxOTA2026` | Changing the admin password does not close these doors; OTP is in the repo |
| 3 | OTA-01/02 | OTA image has no signature, and the CRC is self-computed (nothing to compare against) | Any reachable client with the OTP flashes arbitrary firmware |
| 4 | REL-01 | No IWDG anywhere | A hang, or the supervisor's "unrecoverable → halt", leaves the board dead with outputs frozen until power cycle |
| 5 | ACT-02 | `Relay_Init()` → `Relay_SetState(i,0)` is a no-op (state already 0), so pins are never driven; active-low (NC) relays sit LOW = **energised** after every boot | Safety: a relay reports OFF while the load is ON. Latent today (all live relays are `nc=0`) |
| 6 | NET-11 | MAC `00:08:DC:11:22:33` hardcoded | Two boards on one LAN = ARP/switch chaos |
| 7 | NET-02 | IP `192.168.1.200` hardcoded at boot and re-applied by W5500 self-heal; no DHCP | Not deployable to another subnet; second board conflicts |
| 8 | OTA-03/10 | No rollback; bootloader halts `while(1)` on bad image / bad SP | Bad OTA = brick until SWD reflash |
| 9 | SEC-04/07 | `Access-Control-Allow-Origin: *`, preflight reflects any Origin with `Allow-Credentials: true` (live) | Enables cross-site requests to all unauthenticated write endpoints |
| 10 | ACT-01 | No per-actuator fail-safe state on sensor/comms loss or supervisor halt | Outputs hold last state indefinitely |

## 2. Test evidence

| Tier | What ran | Result |
|---|---|---|
| T0 Build | `cmake --build build` (arm-none-eabi-gcc 10.3) | ✅ 0 errors. text 217,580 B, bss 186,112 B |
| T0 Warnings | Extra build with `-Wextra -Wformat=2 -Wshadow` | 58 warnings, 52 in `ioLibrary_Driver`/FreeRTOS. App code: 2× `-Wsign-compare`, 2× `-Wunused-but-set-variable`, 1× `-Wshadow`, 1× `-Wformat-nonliteral` |
| T0 cppcheck 2.x | `--enable=warning,performance,portability` on APP/HAL/MCAL | 2 findings: `nullPointerRedundantCheck` [sparkplug_b_enc.c:197](../../APP/sparkplug_b_enc.c), `constStatement` [freertos_tasks.c:1183](../../APP/freertos_tasks.c) |
| T0 Stack | `-fstack-usage` without LTO | Top frames: `Dispatch_Request` 5,120 B, `Task_MQTTClient` 3,976 B, `Task_HTTPServer` 1,960 B, `Task_ControlEngine` 1,576 B, `Send_Action_Response` 1,248 B |
| T1 pytest (host) | `pytest tests --ignore=tests/test_http_api.py` | ✅ 252 passed (2 stale source-grep tests fixed, see §5) |
| T1 audit checks | `pytest tests/test_iot_audit.py` | 17 passed, 28 xfailed (= open findings, all fail on the real assertion) |
| T1 C host tests | MinGW-w64 gcc 16.2 (WinLibs UCRT), `-Wall -Wextra`: `test_config_logic.c`, `test_safe_fmt.c`, `test_rules_parser.c` | ✅ 12/12, 8/8, 21/21 + 20,000-iteration parser fuzz (0 mismatches). 0 compiler warnings. ASan/UBSan not available on MinGW |
| T2 live, read-only | `test_http_api.py` GET + auth-gate subset (22 tests) | ✅ 22 passed |
| T2 live probe | Unauth GETs, headers, CORS preflight, malformed requests, Modbus TCP | See items SEC-04, SEC-11, MEM-02, SEC-10. Board stayed up after 3 malformed requests |
| T2 live health | `/api/status` → `health` | heap 5,736 B free of 45,056 (87 % used, min = current); no faults, no restarts; lowest stack margin IOWorker 832 B |
| T3 HIL | Power-cut during OTA/config write, cable pulls, SD removal, socket flood | **Not run** — needs your hands / explicit OK for destructive tests |

## 3. The 100 items

Legend: ✅ PASS · ⚠️ PARTIAL · ❌ FAIL · 🔍 NOT VERIFIED · N/A. Sev = C/H/M/L. "Test" = guard in `test_iot_audit.py` (`xf` = open-finding xfail).

### 3.1 Security (SEC)

| ID | Issue | Status | Sev | Evidence | Fix | Test |
|---|---|---|---|---|---|---|
| SEC-01 | Default credentials, no forced change | ❌ | H | `admin`/`adminkontrx` restored when empty: [flash_partition.c:229](../../HAL/flash_partition.c), [http_server_task.c:1719](../../APP/http_server_task.c); no first-login change | Per-device initial password (printed label / derived from UID), force change on first login | — |
| SEC-02 | Hardcoded secrets / backdoors | ❌ | C | [sdcard.c:365](../../HAL/sdcard.c) always accepts `adminkontrx`, `KontrxMaster2026`; [http_server_task.c:77](../../APP/http_server_task.c) `OTA_OTP_SECRET "KontrxOTA2026"` | Remove literal fallbacks; OTP = per-boot random or TOTP shown on authenticated UI | xf ×2 |
| SEC-03 | No TLS (HTTP + MQTT plaintext) | ❌ | H | No mbedTLS/wolfSSL in tree; W5500 has no TLS | Short term: broker on LAN/VPN + MQTT creds. Long term: mbedTLS on M4F (RAM budget is the blocker, see REL-05) or TLS gateway | — |
| SEC-04 | CORS wildcard / reflected Origin + credentials | ❌ | H | `Access-Control-Allow-Origin: *` [:545](../../APP/http_server_task.c), [:1412](../../APP/http_server_task.c). Live: preflight from `http://evil.example` → 200, ACAO echoed, `Allow-Credentials: true` | Same-origin only; drop CORS or allow-list configured origins | xf |
| SEC-05 | Weak session token, no expiry, single global session | ❌ | H | LCG of tick ⊕ DWT CYCCNT [:171](../../APP/http_server_task.c); no TTL; one `s_session_token` for all clients | STM32 hardware RNG (RNG peripheral), 128-bit token, idle + absolute TTL | xf ×2 |
| SEC-06 | Brute-force protection | ⚠️ | M | Login: 5 tries / 30 s **global** lockout (attacker can lock admin out). `/api/ota/verify` has **no** limit | Per-IP backoff; rate-limit OTP | ✅ lockout exists |
| SEC-07 | CSRF | ❌ | H | Unauthenticated POSTs (SEC-09) are reachable cross-site; cookie `SameSite=Strict` only protects the gated routes | Auth on every write + Origin check | — |
| SEC-08 | XSS in web UI | ❌ | M | 53 `innerHTML` sinks, no escape helper; actuator/sensor names and SD file names concatenated raw; token also kept in `localStorage` (`kx_token`) defeating `HttpOnly` | `escHtml()` for all dynamic text / `textContent`; drop token from localStorage | xf |
| SEC-09 | Missing auth on state-changing endpoints | ❌ | C | Gate list [:1866-1890](../../APP/http_server_task.c) omits: `/api/rules*` (update/accept/reject/toggle/bypass/clear/rollback — commented out "TEMPORARILY"), `/api/provision` (rewrites MQTT broker+creds), `/api/layout`, `/api/interfaces/enable`, `/api/sensor/mode`, `/api/sensor/multi/mode`, `/api/logs/clear`, `/api/sdcard/queue/clear`, `/api/sdcard/logs/clear` | Default-deny: every non-GET needs session; provisioning via one-time pairing token | xf ×2 |
| SEC-10 | Modbus TCP :502 unauthenticated | ❌ | H | Writes gated only by `test_mode` (live board has `test_mode=1`). FC05/06 echo success even when blocked; unsupported FC (live: 0x2B) gets no exception → client timeout | Client IP allow-list, real exception replies (0x01/0x02), optional read-only mode | xf ×2 |
| SEC-11 | Info disclosure / missing security headers | ❌ | M | Unauth GET of `/api/logs`, `/api/sdcard/read|download|browse`, `/api/rules`, `/api/config/*`. Live: no `X-Frame-Options`, CSP, `nosniff`. (MQTT password is **not** returned — good) | Gate reads behind session; add headers | xf |
| SEC-12 | Flash readout protection (RDP) / SWD | 🔍 | H | No code sets option bytes (`OPTCR` only defined in regs header) | Check RDP level on production units; set RDP1 in production flow | — |

### 3.2 OTA & Boot (OTA)

| ID | Issue | Status | Sev | Evidence | Fix | Test |
|---|---|---|---|---|---|---|
| OTA-01 | No image signature | ❌ | C | No sha256/ECDSA/Ed25519 anywhere | Ed25519 or ECDSA-P256 signature over image, public key in bootloader | xf |
| OTA-02 | Meaningless CRC | ❌ | H | [ota_task.c:87](../../APP/ota_task.c) computes CRC of staged bytes and stores it; nobody supplies an expected value. Bootloader never checks `meta->crc32` | Client sends expected CRC/hash in header; bootloader re-verifies before copy | xf |
| OTA-03 | No rollback / trial boot | ❌ | H | Staging becomes the new app; old app erased; no confirm flag | A/B or "pending-confirm": app must confirm within N s else bootloader restores | xf |
| OTA-04 | Power loss during update | ✅ | — | Upload only touches staging; bootloader clears meta (sector 1) only after word-by-word verify ([bootloader.c:113](../../APP/bootloader.c)) → re-copies on next boot | — | ✅ |
| OTA-05 | Downgrade / anti-rollback | ❌ | M | No version field in `OTA_Meta_t` | Monotonic version in signed header | — |
| OTA-06 | Size bound overlaps config | ❌ | M | Limit 262,144 B, but `CONFIG_FLASH_ADDR 0x0807C000` lives inside staging sector 7 (offset 240 KB). Image now 219 KB → 21 KB margin | Limit = 0x3C000 or move config out of staging | ✅ bound / xf overlap |
| OTA-07 | Partial upload | ✅ | — | `total_written < content_length` → 500 `OTA INCOMPLETE`, meta not written | — | — |
| OTA-08 | OTA while actuating | ⚠️ | M | `g_ota_in_progress` pauses tasks, but 2× 128 KB sector erase stalls the CPU (~2–4 s, code runs from flash) with outputs frozen | Drive outputs to safe state before erase; warn in UI | — |
| OTA-09 | OTP not bound to client | ❌ | H | `ota_otp_validated` is one global flag: after anyone verifies, **any** client can `/api/ota/prepare` + `/update` | Bind to session + socket, expire after 60 s | — |
| OTA-10 | Bootloader dead-ends | ❌ | H | Copy mismatch or bad SP → `while(1)` halt ([bootloader.c:115,132](../../APP/bootloader.c)); no IWDG | Retry copy, fall back to recovery/DFU mode, IWDG | — |

### 3.3 Reliability (REL)

| ID | Issue | Status | Sev | Evidence | Fix | Test |
|---|---|---|---|---|---|---|
| REL-01 | No hardware watchdog | ❌ | C | No IWDG/WWDG code. Supervisor halts CPU on unrecoverable faults ([fault_supervisor.h](../../APP/fault_supervisor.h)) | IWDG ~2–4 s, fed only when all task heartbeats are fresh | xf |
| REL-02 | Task hang detection | ❌ | H | Supervisor catches faults, not livelocks / blocked tasks | Per-task heartbeat counter checked by supervisor | xf |
| REL-03 | Fault handling | ⚠️ | H | Good: HardFault/MemManage/Bus/Usage isolated, task restarted, record survives reset. Gap: unrecoverable path halts forever with outputs frozen | Halt → safe outputs → reset (via IWDG) | ✅ |
| REL-04 | Stack overflow | ✅ | — | `configCHECK_FOR_STACK_OVERFLOW 2`; live min free: ControlEng 2,336 / Modbus 1,864 / HTTP 3,120 / MQTT 2,720 / **IOWorker 832** / LogWriter 1,208 B | Watch IOWorker; OTA task uses only 164 B of 4 KB → 3 KB can be reclaimed | ✅ |
| REL-05 | Heap headroom | ⚠️ | M | 44 KB heap, live free **5,736 B** (= min ever). `malloc_fail=0` | Reclaim OTA stack, consider static task allocation | ✅ hook |
| REL-06 | Brown-out (BOR) level | 🔍 | M | No BOR option-byte config in code | Read option bytes on board; set BOR level 3 for 3.3 V | — |
| REL-07 | Reset-reason logging | ⚠️ | L | RCC_CSR decoded in boot audit ([sdcard.c:83](../../HAL/sdcard.c)) but flags never cleared (`RMVF`) → cause accumulates across resets | Latch + clear with `RMVF` at boot | xf |
| REL-08 | Priority inversion | ✅ | — | Mutexes via `xSemaphoreCreateMutex` / `osMutexNew` → priority inheritance | — | — |
| REL-09 | Deadlock risk | ⚠️ | M | 17 of 34 `osMutexAcquire` calls use `osWaitForever` | Bounded timeouts + log on timeout | — |
| REL-10 | Busy-wait in tasks | ⚠️ | L | Modbus TX polls `USART3->SR` up to 500k iterations ([modbus_dma.c:224](../../HAL/modbus_dma.c)) | TC interrupt / DMA | — |

### 3.4 Memory safety (MEM)

| ID | Issue | Status | Sev | Evidence | Fix | Test |
|---|---|---|---|---|---|---|
| MEM-01 | Unsafe string functions | ✅ | — | Only `strcpy` of literals into fixed buffers; rest `snprintf`/`strncpy` | — | — |
| MEM-02 | RX buffer bounds | ✅ | — | `recv` clamped to `sizeof(rx_buf)-1`; >12 KB body → 413. Live: 3 KB URL, binary junk, bad `%` escapes → clean 404/200, board stays up | — | ✅ 413 |
| MEM-03 | JSON input parsing | ✅ | L | `JSON_ReadStr` bounded; does not handle `\"` (truncates) | Escape-aware reader | — |
| MEM-04 | Content-Length parsing | ⚠️ | M | `atoi()` ([:1618](../../APP/http_server_task.c)) — negative / > INT_MAX values make `need` negative and skip the wait | Parse unsigned with overflow check, reject < 0 | — |
| MEM-05 | `/update` Content-Length | ⚠️ | L | Hand-rolled digit loop wraps on > 10 digits | Same helper as MEM-04 | — |
| MEM-06 | JSON output injection | ❌ | M | `username` echoed into JSON via `%s` unescaped (`/api/auth/me`, login) | JSON-escape helper in `safe_fmt` | — |
| MEM-07 | Large stack frames | ✅ | — | See §2 stack table; fits current task stacks | Keep `-fstack-usage` check in CI | — |
| MEM-08 | Static analysis findings | ⚠️ | L | cppcheck: 2 (see §2) | Fix redundant NULL check | — |
| MEM-09 | Compiler warnings | ⚠️ | L | 6 in app code with extra flags | Fix, then `-Werror` for APP/HAL | — |
| MEM-10 | DMA from CCMRAM | ✅ | — | `rx_buf`/`tx_buf`/queues in CCM; SPI/USART used without DMA | Never enable DMA on CCM buffers | — |

### 3.5 Networking (NET)

| ID | Issue | Status | Sev | Evidence | Fix | Test |
|---|---|---|---|---|---|---|
| NET-01 | Link up/down detection | ❌ | M | No PHY link polling in `KontrxRTOS` (only in legacy `main.c`) | Poll `PHYCFGR`, log + drive MQTT state | — |
| NET-02 | Hardcoded IP, no DHCP | ❌ | H | `192.168.1.200` in [main_kontrx.c:325](../../APP/main_kontrx.c) and self-heal [freertos_tasks.c:1712](../../APP/freertos_tasks.c) | IP/mask/gw/DHCP in config; self-heal re-applies config | xf |
| NET-03 | DNS failure | ✅ | — | Logged, offline queueing, 5 s retry | — | — |
| NET-04 | Socket exhaustion | ✅ | — | Adaptive idle timeout (PR #22); live test run fine | Add flood test to HIL | — |
| NET-05 | Slow-client (slowloris) | ⚠️ | M | One HTTP task; a partial POST blocks it for up to 300 ms + 2.5 s, stalling every socket | Per-socket state machine instead of blocking loops | ✅ 413 |
| NET-06 | Half-open TCP | ⚠️ | L | No W5500 TCP keep-alive (`Sn_KPALVTR`) on server sockets; relies on idle timeout | Set `Sn_KPALVTR` | — |
| NET-07 | MQTT keep-alive + LWT | ✅ | — | `keepAliveInterval=60`, NDEATH LWT retained QoS1 | — | ✅ |
| NET-08 | Reconnect backoff | ❌ | L | Fixed 5 s; no exponential backoff or jitter | Exp. backoff 5→300 s + jitter | xf |
| NET-09 | Broker unavailable | ✅ | — | Offline queue to W25Q16 | — | — |
| NET-10 | W5500 lock-up | ✅ | — | VERSIONR/IP check every 5 s + reinit | Uses hardcoded IP (NET-02) | — |
| NET-11 | Duplicate MAC | ❌ | C | `00:08:DC:11:22:33` on every unit (live board reports it) | Derive from 96-bit UID (0x1FFF7A10) or provisioned | xf |
| NET-12 | Oversized payloads | ✅ | — | 413 above 12 KB | — | ✅ |

### 3.6 Telemetry (TEL)

| ID | Issue | Status | Sev | Evidence | Fix | Test |
|---|---|---|---|---|---|---|
| TEL-01 | Store-and-forward | ✅ | — | 496 KB ring on W25Q16 | — | — |
| TEL-02 | Duplicates / ordering on replay | 🔍 | M | QoS1 + persistent session → duplicates possible; needs broker-side test | Message id / seq in payload | — |
| TEL-03 | Timestamps before time sync | ✅ | — | `TIME_UPTIME_FLAG` marks unsynced stamps | — | ✅ |
| TEL-04 | NTP failure | ✅ | — | SNTP local→internet, browser fallback | — | — |
| TEL-05 | RTC persistence | ⚠️ | L | HW RTC initialised but clock is software-only; time lost on reboot until SNTP | Use RTC + VBAT as source | — |
| TEL-06 | Queue overflow policy | ✅ | — | Flash ring overwrites oldest; RAM queues drop + count | — | ✅ |
| TEL-07 | Sparkplug `bdSeq` | ❌ | M | Always `0` in NBIRTH/NDEATH ([sparkplug_b_enc.c:194,403](../../APP/sparkplug_b_enc.c)) — host apps can't pair births with deaths | Persist + increment per connect | xf |
| TEL-08 | NDEATH | ✅ | — | Via LWT | — | — |
| TEL-09 | Scaling / overflow | ⚠️ | L | Modbus TCP exposes `(uint16_t)(value*100)` ([modbus_tcp_server.c:70](../../HAL/modbus_tcp_server.c)): negative → UB, > 655.35 wraps | Clamp, or 32-bit float register pairs | — |
| TEL-10 | Stale data flag | ✅ | — | `stale` per reading | — | ✅ |

### 3.7 Modbus RTU / sensors (MB)

| ID | Issue | Status | Sev | Evidence | Fix | Test |
|---|---|---|---|---|---|---|
| MB-01 | Response timeouts | ✅ | — | Per-sensor window (`RS485_ReplyTimeout`) | — | — |
| MB-02 | Reply validation | ✅ | L | Slave, FC, CRC checked ([modbus_dma.c:285](../../HAL/modbus_dma.c)); byte-count `rx[2]` not checked | Check `rx[2] == 2*num` | ✅ |
| MB-03 | RS485 DE turnaround | ✅ | — | Waits for `TC` before RX | — | — |
| MB-04 | Bus contention | 🔍 | M | Needs HIL with two masters / noise | — | — |
| MB-05 | Sensor disconnect | ✅ | — | Stale readings excluded from rules | — | ✅ |
| MB-06 | NaN / Inf / range | ❌ | M | `Decode_Float_DCBA` returns raw float, no `isnan`/range check → NaN silently fails rule compares and prints `nan` (invalid JSON) | `isfinite` + per-type range → mark stale | xf |
| MB-07 | Filter lag | ✅ | — | Moving-avg / median by design | Measure on HIL | — |
| MB-08 | Address conflicts | ⚠️ | L | Scan exists; duplicate-address detection not seen | Flag duplicates in scan | — |
| MB-09 | Retry storms | 🔍 | L | Needs HIL with dead slaves | — | — |
| MB-10 | Word order | ✅ | — | DCBA handled per sensor | — | ✅ |

### 3.8 Actuation & safety (ACT)

| ID | Issue | Status | Sev | Evidence | Fix | Test |
|---|---|---|---|---|---|---|
| ACT-01 | Fail-safe state | ❌ | H | No per-actuator safe state for comms/sensor loss or halt | `safe_state` per actuator, applied on stale inputs / halt / OTA | xf |
| ACT-02 | Relay state at boot | ❌ | C | [freertos_tasks.c:445](../../APP/freertos_tasks.c) calls `Relay_SetState(i,0)`, which returns early at [:509](../../APP/freertos_tasks.c) because `relayStates[i]` is already 0 → pin left at ODR reset (LOW). For `nc=1` relays LOW = energised. Also MODER set before ODR | Write BSRR to the de-energised level **before** switching MODER to output | xf |
| ACT-03 | Conflicting rules | ✅ | — | Output claims with priority arbitration | — | — |
| ACT-04 | Chattering | ⚠️ | M | Hysteresis node exists; no min on/off time | Min dwell per actuator | — |
| ACT-05 | Manual vs auto | ⚠️ | L | `test_mode` gate on HTTP/MQTT/Modbus. MQTT `relay_N` parses one digit (`relay_10` → relay 1) | Parse full index | xf |
| ACT-06 | Interlocks | ❌ | M | No interlock concept between outputs | Interlock groups in rules | — |
| ACT-07 | PTO limits | ✅ | — | Limit-switch ISR → `PTO_EmergencyStop` | — | ✅ |
| ACT-08 | Analog limits | ✅ | L | 0–10 V clamp, 0–20.5 mA clamp; 4–20 mA allows < 4 mA | Floor 4 mA unless "fault" | ✅ |
| ACT-09 | Global e-stop | ⚠️ | M | SD removal freezes writes; no e-stop input that de-energises all | E-stop DI → all safe states | — |
| ACT-10 | Pin conflict validation | ❌ | M | Live config: `4202`, `REERE`, `OPCUA1` all on `PA4` | Reject duplicate pins on save | — |

### 3.9 Storage (STO)

| ID | Issue | Status | Sev | Evidence | Fix | Test |
|---|---|---|---|---|---|---|
| STO-01 | Flash wear | ⚠️ | L | Rings spread wear; config save = 2 sector erases each time | Write-on-change only (already), wear counter | — |
| STO-02 | Corrupt config accepted | ❌ | M | "CRC-skip migration" loads a config whose CRC fails if magic matches ([flash_partition.c:285](../../HAL/flash_partition.c)) | Fall back to defaults instead | xf |
| STO-03 | Dual-sector config | ✅ | — | Primary + backup with CRC32 | — | ✅ |
| STO-04 | SD removal | ✅ | — | SD is a presence key; removal → critical lock (data lives on W25Q16) | — | — |
| STO-05 | Filesystem corruption | N/A | — | No FAT filesystem in firmware | — | — |
| STO-06 | Log rotation | ✅ | — | Circular 1 MB log | — | — |
| STO-07 | Erase blocking RTOS | ✅ | — | W25Q erase under SD lock; internal erase only during OTA | — | — |
| STO-08 | Secrets at rest | ❌ | M | Admin, master, MQTT passwords stored plaintext in config | Salted hash for admin/master | xf |

### 3.10 Web / API (WEB)

| ID | Issue | Status | Sev | Evidence | Fix | Test |
|---|---|---|---|---|---|---|
| WEB-01 | Input validation | ⚠️ | M | IP parse + reserved-pin checks exist; duplicates not (ACT-10) | Central validator per save | — |
| WEB-02 | Error format | ⚠️ | L | `HTTP_400` body without `Content-Type`; 404 plain text | Uniform JSON errors | — |
| WEB-03 | API versioning | ❌ | L | No `/api/v1` | Version prefix before external integrations | — |
| WEB-04 | Concurrent config saves | ✅ | — | Single HTTP task serialises writes | — | — |
| WEB-05 | SSE reconnect / offline UI | ✅ | — | `EventSource.onerror` handlers | — | — |
| WEB-06 | Stale assets after OTA | ✅ | — | `Cache-Control: no-cache, no-store` | — | — |
| WEB-07 | Long-run DOM leaks | 🔍 | L | Needs 24 h browser profiling | — | — |
| WEB-08 | Config schema migration | ⚠️ | M | One `CONFIG_MAGIC_CURRENT`; struct change → defaults (or CRC-skip, STO-02) | Versioned config with field migration | — |

## 4. Proposed fix roadmap (Phase 2 — needs approval)

Each line = one PR to `dev`. Order by risk.

1. **fix(security): default-deny auth** — SEC-09, SEC-07, SEC-04, OTA-09, SEC-11 headers.
2. **fix(security): remove backdoors + hardware RNG tokens** — SEC-02, SEC-05, SEC-06.
3. **fix(safety): relay boot level + fail-safe states** — ACT-02, ACT-01, ACT-10.
4. **feat(reliability): IWDG + task heartbeats** — REL-01, REL-02, REL-03, REL-07.
5. **fix(net): unique MAC + configurable IP/DHCP** — NET-11, NET-02, NET-01, NET-08.
6. **feat(ota): signed images + bootloader verify + rollback** — OTA-01/02/03/05/06/10 (bootloader change → needs SWD recovery plan before flashing).
7. **fix(data): NaN guards, bdSeq, config CRC, scaling** — MB-06, TEL-07, STO-02, TEL-09, MEM-04/06.
8. Remaining Medium/Low items.

## 5. Notes

- Two stale tests fixed in `tests/test_bug_fixes.py`: both grepped a fixed-size window that the code outgrew. The behaviour exists (2.5 s body timeout; `name` field present live in `/api/status`).
- T3 HIL / destructive tests (power-cut during OTA and config save, cable pulls, socket flood, login lockout) were **not run** and need explicit approval.
- Live tests that log in were skipped on purpose: login regenerates the single global session token and would log out your open browser session.
