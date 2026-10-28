"""
Tests verifying bug fixes: config save, QR code, test mode toggle,
config page loading, POST body accumulation, auth gatekeeper.
Run: pytest tests/test_bug_fixes.py -v
"""
import gzip
import json
import os
import re

import pytest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_ASSETS_H = os.path.join(REPO_ROOT, "APP", "web_assets.h")
HTTP_SERVER_C = os.path.join(REPO_ROOT, "APP", "http_server_task.c")
WEB_INDEX = os.path.join(REPO_ROOT, "web", "index.html")


def read_c_file():
    with open(HTTP_SERVER_C, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def read_js():
    with open(WEB_INDEX, "r", encoding="utf-8") as f:
        html = f.read()
    m = re.search(r"<script>(.*?)</script>", html, re.DOTALL)
    return m.group(1) if m else ""


def read_html():
    with open(WEB_INDEX, "r", encoding="utf-8") as f:
        return f.read()


def get_embedded_html():
    with open(WEB_ASSETS_H, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()
    hex_values = re.findall(r"0x([0-9a-fA-F]{2})", content)
    data = bytes(int(h, 16) for h in hex_values)
    return gzip.decompress(data).decode("utf-8")


class TestConfigSaveRxBuf:
    def test_rx_buf_at_least_4096(self):
        m = re.search(r"#define\s+RX_BUF_SIZE\s+(\d+)", read_c_file())
        assert m and int(m.group(1)) >= 4096

    def test_10_gpio_relays_fit(self):
        relays = [{"name": f"Relay{i+1}", "type": 0, "pin": f"PE{i}", "nc": 0} for i in range(10)]
        assert len(json.dumps({"relays": relays})) + 500 < 4096

    def test_16_mixed_actuators_fit(self):
        relays = []
        for i in range(16):
            r = {"name": f"Act{i+1}", "type": i % 8, "nc": 0}
            if i % 8 in (0, 7):
                r["pin"] = f"PD{i}"
            elif i % 8 == 1:
                r.update({"ip": "192.168.1.50", "port": 502, "slave_id": 1, "reg_addr": i})
            elif i % 8 in (3, 4, 5, 6):
                r["channel"] = 1
            relays.append(r)
        assert len(json.dumps({"relays": relays})) + 500 < 4096


class TestPostBodyAccumulation:
    def test_recv_accumulates_post_body(self):
        c = read_c_file()
        assert "content_len" in c, "Missing Content-Length body accumulation"

    def test_accumulation_has_timeout(self):
        c = read_c_file()
        idx = c.find("For POST requests, accumulate")
        if idx < 0:
            idx = c.find("content_len")
        assert idx >= 0
        # The header wait (300 ms) and body wait (2500 ms) loops follow the comment.
        assert "osKernelGetTickCount() - t0) < 2500" in c[idx:idx + 5000], "Accumulation needs timeout"


class TestQRCodeFix:
    def test_nano_qr_library(self):
        assert "nanoQR" in read_js()

    def test_generate_qr_function(self):
        assert "function generateQR" in read_js()

    def test_qr_error_fallback(self):
        js = read_js()
        assert "drawQRFallback" in js

    def test_qr_payload_limit(self):
        js = read_js()
        idx = js.find("nanoQR.generate")
        assert "length" in js[max(0, idx - 500):idx + 200]

    def test_qr_actuator_type_and_pinouts(self):
        js = read_js()
        idx = js.find("function generateQR")
        assert idx >= 0
        qr_code = js[idx:idx + 2500]
        assert ("item.type=" in qr_code or "v.type=" in qr_code)
        assert ("item.pinout=" in qr_code or "v.pinout=" in qr_code)
        assert "copyQRPayload" in js


class TestTestModeToggle:
    def test_header_toggle_exists(self):
        assert "hdr-test-mode-switch" in read_html()

    def test_toggle_handles_boolean(self):
        js = read_js()
        block = js[js.find("function toggleTestMode"):js.find("function toggleTestMode") + 200]
        assert "typeof" in block or "boolean" in block

    def test_update_ui_syncs_header(self):
        js = read_js()
        block = js[js.find("function updateTestModeUI"):js.find("function updateTestModeUI") + 500]
        assert "hdr-test-mode-switch" in block
        assert "hdr-test-mode-badge" in block

    def test_test_mode_endpoint(self):
        assert "POST /api/config/test_mode" in read_c_file()


class TestConfigPageLoading:
    def test_auto_fetch_when_empty(self):
        js = read_js()
        block = js[js.find("function buildRelayConfigGrid"):js.find("function buildRelayConfigGrid") + 300]
        assert "_cfgFetching" in block or "fetchStatus" in block

    def test_relays_from_status(self):
        assert "relays=e.relays" in read_js()

    def test_status_includes_relay_config(self):
        c = read_c_file()
        idx = c.find("static int JSON_StatusResponse")
        assert idx >= 0, "JSON_StatusResponse function not found"
        # Scan the whole function body: it has grown past a fixed window.
        end = c.find("\nstatic ", idx + 1)
        block = c[idx:end if end > 0 else len(c)]
        for field in ["type", "pin", "name"]:
            assert field in block, f"Status response missing '{field}' field"


class TestAuthGatekeeper:
    def _gate(self, c):
        i = c.find("Default-deny API Gatekeeper")
        assert i >= 0, "default-deny gatekeeper missing"
        return c[i:i + 900]

    def _public(self, c):
        i = c.find("static uint8_t Is_Public_Route")
        assert i >= 0, "Is_Public_Route missing"
        return c[i:c.find("\n}", i)]

    def test_config_requires_auth(self):
        c = read_c_file()
        gk = self._gate(c)
        assert 'strncmp(line, "POST ", 5) == 0' in gk and "Is_Session_Valid" in gk
        assert "POST /api/config" not in self._public(c)

    def test_relay_requires_auth(self):
        assert "POST /api/relay" not in self._public(read_c_file())

    def test_bearer_and_cookie_validation(self):
        c = read_c_file()
        assert "Bearer " in c
        assert "kx_session=" in c

    def test_provision_requires_session(self):
        """Provisioning rewrites the MQTT broker, so it is no longer exempt."""
        pub = self._public(read_c_file())
        assert "provision" not in pub and "rules" not in pub


class TestEmbeddedHTMLSync:
    def test_toggle_fix_in_embedded(self):
        assert "typeof" in get_embedded_html() or "boolean" in get_embedded_html()

    def test_config_fetch_in_embedded(self):
        assert "_cfgFetching" in get_embedded_html()

    def test_header_badge_sync_in_embedded(self):
        assert "hdr-test-mode-badge" in get_embedded_html()


class TestMultiSocketSafety:
    def test_socket_ids_no_conflict(self):
        c = read_c_file()
        m = re.search(r"s_http_socks\[.*?\]\s*=\s*\{([^}]+)\}", c)
        if m:
            sock_ids = [int(x.strip()) for x in m.group(1).split(",")]
            reserved = {1, 2, 4}  # MQTT=1, Modbus TCP Server=2, Modbus Client=4
            for sid in sock_ids:
                assert sid not in reserved, f"HTTP socket {sid} conflicts"

    def test_rx_buf_is_static(self):
        assert "static uint8_t rx_buf[RX_BUF_SIZE]" in read_c_file()


class TestConfigPayloadSizes:
    @pytest.mark.parametrize("count", [1, 5, 10, 16])
    def test_gpio_relays_fit(self, count):
        relays = [{"name": f"R{i+1}", "type": 0, "pin": f"PE{i%16}", "nc": 0} for i in range(count)]
        assert len(json.dumps({"relays": relays})) + 500 < 4096

    @pytest.mark.parametrize("count", [1, 5, 10])
    def test_modbus_relays_fit(self, count):
        relays = [{"name": f"MB{i+1}", "type": 1, "ip": "192.168.1.50",
                    "port": 502, "slave_id": 1, "reg_addr": i, "nc": 0} for i in range(count)]
        assert len(json.dumps({"relays": relays})) + 500 < 4096
