"""
Unit tests for Kontrx MQTT, UI elements, OTA authentication, and queue throttling.
Run: pytest tests/test_mqtt_and_ui.py -v
"""
import os
import re
import gzip
import pytest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_INDEX = os.path.join(REPO_ROOT, "web", "index.html")
WEB_ASSETS_H = os.path.join(REPO_ROOT, "APP", "web_assets.h")
HTTP_SERVER_C = os.path.join(REPO_ROOT, "APP", "http_server_task.c")
FREERTOS_TASKS_C = os.path.join(REPO_ROOT, "APP", "freertos_tasks.c")


def read_file(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


class TestUIElementsAndScripts:
    @pytest.fixture(scope="class")
    def html(self):
        return read_file(WEB_INDEX)

    def test_zero_missing_html_event_handlers(self, html):
        """Ensure all functions invoked from HTML event attributes are actually defined."""
        handlers = re.findall(r'on(?:click|change|input|drop|dragstart|dragover|dragend)=\s*[\'"]([^\'"]+)[\'"]', html)
        fn_calls = set()
        for h in handlers:
            for m in re.finditer(r'([a-zA-Z0-9_$]+)\s*\(', h):
                fn_name = m.group(1)
                if fn_name not in ['if', 'for', 'while', 'switch', 'alert', 'confirm',
                                   'parseInt', 'parseFloat', 'event', 'stopPropagation',
                                   'preventDefault', 'Math']:
                    fn_calls.add(fn_name)

        script_match = re.search(r'<script>(.*?)</script>', html, re.DOTALL)
        assert script_match is not None, "web/index.html must have a <script> block"
        script_content = script_match.group(1)

        missing = []
        for fn in sorted(fn_calls):
            pattern = r'(?:function\s+' + re.escape(fn) + r'\b|' + re.escape(fn) + r'\s*[:=]\s*function|window\.' + re.escape(fn) + r'\b)'
            if not re.search(pattern, script_content):
                missing.append(fn)

        assert len(missing) == 0, f"Found missing functions in JS: {missing}"

    def test_mode_toggle_buttons_styled(self, html):
        """Ensure CSS classes for payload mode buttons and palette exist."""
        assert ".mode-toggle-btn" in html
        assert ".mode-toggle-btn.active" in html
        assert ".palette-item" in html
        assert ".field-pill" in html

    def test_mqtt_builder_functions_defined(self, html):
        """Ensure builder functions are implemented."""
        assert "function switchPayloadMode" in html
        assert "function buildMqttPalette" in html
        assert "function insertPlaceholder" in html
        assert "function toggleMqttTx" in html
        assert "function toggleMQTTSkipOffline" in html

    def test_skip_offline_not_empty(self, html):
        """Ensure toggleMQTTSkipOffline actually performs a fetch to the backend."""
        m = re.search(r'function toggleMQTTSkipOffline\(.*?\)\{(.*?)\}', html, re.DOTALL)
        assert m is not None
        body = m.group(1)
        assert "fetch" in body
        assert "/api/config/mqtt/skip_offline" in body


class TestOTAAuthentication:
    @pytest.fixture(scope="class")
    def html(self):
        return read_file(WEB_INDEX)

    @pytest.fixture(scope="class")
    def http_c(self):
        return read_file(HTTP_SERVER_C)

    def test_web_upload_authenticates_otp(self, html):
        """Ensure the web uploadFirmware function validates OTP before flashing."""
        idx = html.find("function uploadFirmware")
        assert idx != -1
        func_block = html[idx:idx + 1800]
        assert "/api/ota/verify" in func_block
        assert "KontrxOTA2026" in func_block
        assert "/api/ota/prepare" in func_block
        assert "/update" in func_block

    def test_http_server_accepts_session_or_otp(self, http_c):
        """Ensure firmware HTTP server allows authenticated admin session or OTP."""
        update_idx = http_c.find('strncmp(line, "POST /update"')
        assert update_idx != -1
        update_block = http_c[update_idx:update_idx + 1000]
        assert "Is_Session_Valid" in update_block

        prep_idx = http_c.find('strncmp(line, "POST /api/ota/prepare"')
        assert prep_idx != -1
        prep_block = http_c[prep_idx:prep_idx + 500]
        assert "Is_Session_Valid" in prep_block


class TestBackendMQTTRoutesAndQueue:
    @pytest.fixture(scope="class")
    def http_c(self):
        return read_file(HTTP_SERVER_C)

    @pytest.fixture(scope="class")
    def tasks_c(self):
        return read_file(FREERTOS_TASKS_C)

    def test_mqtt_toggle_endpoint_exists(self, http_c):
        """Ensure both /tx_switch and /toggle endpoints are handled."""
        assert "POST /api/config/mqtt/tx_switch" in http_c
        assert "POST /api/config/mqtt/toggle" in http_c

    def test_mqtt_skip_offline_endpoint_exists(self, http_c):
        """Ensure skip_offline endpoint is handled and persisted."""
        assert "POST /api/config/mqtt/skip_offline" in http_c

    def test_queue_drain_throttled(self, tasks_c):
        """Ensure offline queue draining is rate-limited and respects mqtt_tx_enabled."""
        idx = tasks_c.find("Partition_Queue_Count() > 0")
        assert idx != -1
        context = tasks_c[max(0, idx - 400):idx + 400]
        assert "drain_interval_ticks" in context or "s_last_queue_drain_tick" in context
        assert "mqtt_tx_enabled" in context

    def test_queue_drain_prevents_duplicate_keys(self, tasks_c):
        """Ensure offline JSON building avoids duplicate sensor keys."""
        assert "emitted_keys" in tasks_c
        assert "duplicate" in tasks_c


class TestWebAssetsSync:
    def test_web_assets_header_matches_html(self):
        """Ensure APP/web_assets.h is synchronized and within flash budget."""
        assert os.path.isfile(WEB_ASSETS_H)
        content = read_file(WEB_ASSETS_H)
        hex_values = re.findall(r"0x([0-9a-fA-F]{2})", content)
        data = bytes(int(h, 16) for h in hex_values)
        assert len(data) < 65536, f"Gzipped SPA is {len(data)} bytes, exceeds 64KB budget"
        decompressed = gzip.decompress(data).decode("utf-8")
        assert ".mode-toggle-btn" in decompressed
        assert "buildMqttPalette" in decompressed
