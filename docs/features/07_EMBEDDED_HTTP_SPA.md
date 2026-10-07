# Feature: Embedded HTTP Server & Single-Page Application (SPA)

## 1. Description & Purpose (بتعمل ايه وليه اتعملت)
Hosts a zero-dependency, modern responsive web dashboard and REST API server directly inside the STM32F4 microcontroller over Ethernet.
* **Why it was built**: Technicians and field engineers commissioning industrial installations do not always have an internet connection or cloud access. By typing the controller's local IP address (e.g. `http://192.168.1.50`) into any smartphone or laptop browser, they immediately gain full control: viewing sensor graphs, toggling relays, re-tuning PWM/PTO/analog parameters, managing rules, browsing SD card logs, and triggering firmware updates.

---

## 2. Technical Implementation & How It Works (بتشتغل ازاي)
* **Code Location**: `APP/http_server_task.c`, `APP/http_server_task.h`, `APP/web_assets.h`, `web/index.html`.
* **Zero-RAM Asset Compression & Streaming**:
  * The frontend application (`web/index.html`) is compressed using `gzip` and embedded directly as a constant byte array in internal MCU flash (`APP/web_assets.h`).
  * When a browser requests `GET /`, the HTTP task serves the gzipped payload directly with header `Content-Encoding: gzip`.
  * The entire UI fits within a strict **< 64 KB flash budget** (typically ~35 KB gzipped), consuming 0 bytes of RAM at rest.
* **REST API Endpoints**:
  * **Telemetry & State**: `GET /api/status` (live sensor values, relay statuses, uptime, heap).
  * **Actuator Control**: `POST /api/relay?id=N&state=0|1`, `POST /api/relay/all`, `/api/pwm`, `/api/pto`, `/api/420ma`, `/api/010v`.
  * **Configuration**: `POST /api/config/relays`, `POST /api/config/mqtt`, `POST /api/config/sensors`.
  * **Rules & Canvas**: `GET /api/rules`, `POST /api/rules`, `GET /api/canvas/layout`, `POST /api/canvas/layout`.
  * **Storage & Logs**: `GET /api/sdcard/status`, `GET /api/logs/paged`, `GET /api/sdcard/list`, `GET /api/sdcard/download`.
  * **Security & Auth**: `POST /api/auth/login`, `POST /api/auth/logout`, `GET /api/auth/me`, `POST /api/auth/change_password`.
  * **OTA Firmware Update**: `POST /api/ota/upload` (stream-written in chunks to flash staging).
* **Buffer Architecture**:
  * Response payloads are assembled into a dedicated 12 KB DMA buffer located in **CCMRAM** (`.ccmram`), completely bypassing main SRAM1 and freeing heap memory.

---

## 3. Tests Performed & Verification (ايه الـ Tests اللي اتعملت عليها وازاي)
* **Automated Asset & API Test Suites**:
  * Files: `tests/test_web_assets.py` & `tests/test_http_api.py`
  * Execution:
    ```bash
    pytest tests/test_web_assets.py tests/test_http_api.py -v
    ```
  * What it verifies: Gzip magic byte validation (`0x1F 0x8B`), decompression into valid HTML5, DOM tag integrity, API routing, authentication token validation, and query parameter parsing.
* **Concurrency & Browser Stress Test**:
  * Hammered the embedded server with 50 concurrent HTTP requests from curl / ApacheBench (`ab -n 500 -c 10`); verified that W5500 socket management kept memory leak-free and FreeRTOS task never locked.
* **Mobile & Desktop Browser Verification**:
  * Tested on Chrome, Safari Mobile (iOS), Firefox, and Edge. Verified reactive dashboard graphs and toggle switches work seamlessly without internet connectivity.

---

## 4. System Impact & Inter-Dependencies (بتاثر علي ايه ومرتبطه بايه)
* **Dependencies**:
  * Operates on W5500 Socket 0 (`PORT_HTTP = 80`).
  * Interacts with `sensorMutex`, `actuatorMutex`, and `flashMutex`.
* **Cross-Task Impact**:
  * Allows operators to manually override `Task_ControlEngine` automations.
  * Triggers OTA reboot handoff to `Task_OTA`.

---

## 5. Development Status & Blockers (حالتها والمعوقات)
* **Status**: ✅ **COMPLETED & PRODUCTION STABLE (مكتملة ومستقرة تماماً)**.
* **Blockers / Known Gaps**: None. Fully verified with live responsive web dashboard.
