"""
Kontrx HTTP API Integration Tests
Run against a live device: pytest test_http_api.py --device=192.168.1.100
"""
import pytest
import requests
import time
import json

DEFAULT_DEVICE = "192.168.1.100"
DEFAULT_USER = "admin"
DEFAULT_PASS = "adminkontrx"




@pytest.fixture(scope="session")
def base_url(request):
    return f"http://{request.config.getoption('--device')}"


@pytest.fixture(scope="session")
def credentials(request):
    return {
        "username": request.config.getoption("--user"),
        "password": request.config.getoption("--password"),
    }


@pytest.fixture(scope="session")
def session_token(base_url, credentials):
    r = requests.post(f"{base_url}/api/auth/login", json=credentials, timeout=5)
    assert r.status_code == 200
    data = r.json()
    assert data["ok"] is True
    return data["token"]


# ── Auth Tests ────────────────────────────────────────────

class TestAuth:
    def test_login_success(self, base_url, credentials):
        r = requests.post(f"{base_url}/api/auth/login", json=credentials, timeout=5)
        assert r.status_code == 200
        data = r.json()
        assert data["ok"] is True
        assert "token" in data
        assert data["token"].startswith("kx")

    def test_session_cookie_httponly(self, base_url, credentials):
        r = requests.post(f"{base_url}/api/auth/login", json=credentials, timeout=5)
        cookie_header = r.headers.get("Set-Cookie", "")
        assert "HttpOnly" in cookie_header

    def test_login_wrong_password(self, base_url, credentials):
        r = requests.post(
            f"{base_url}/api/auth/login",
            json={"username": credentials["username"], "password": "wrong"},
            timeout=5,
        )
        assert r.status_code == 401
        assert r.json()["ok"] is False

    def test_login_empty_body(self, base_url):
        r = requests.post(f"{base_url}/api/auth/login", data="", timeout=5)
        assert r.status_code == 401

    def test_logout(self, base_url, credentials):
        r_login = requests.post(f"{base_url}/api/auth/login", json=credentials, timeout=5)
        token = r_login.json().get("token")
        r = requests.post(
            f"{base_url}/api/auth/logout",
            headers={"X-Auth-Token": token},
            timeout=5,
        )
        assert r.status_code == 200

    def test_protected_endpoint_no_token(self, base_url):
        r = requests.post(f"{base_url}/api/config/sensors", json={}, timeout=5)
        assert r.status_code == 401

    def test_protected_endpoint_bad_token(self, base_url):
        time.sleep(0.1)
        r = requests.post(
            f"{base_url}/api/config/sensors",
            json={},
            headers={"X-Auth-Token": "invalid_token"},
            timeout=5,
        )
        assert r.status_code == 401


# ── Status & Read-Only API Tests ──────────────────────────

class TestStatusAPI:
    def test_get_status(self, base_url):
        r = requests.get(f"{base_url}/api/status", timeout=5)
        assert r.status_code == 200
        data = r.json()
        assert "fw" in data
        assert "uptime_s" in data or "uptime" in data

    def test_get_interfaces(self, base_url):
        r = requests.get(f"{base_url}/api/interfaces", timeout=5)
        assert r.status_code == 200
        data = r.json()
        assert isinstance(data, dict)

    def test_get_config_sensors(self, base_url):
        r = requests.get(f"{base_url}/api/config/sensors", timeout=5)
        assert r.status_code == 200

    def test_get_config_mqtt(self, base_url):
        r = requests.get(f"{base_url}/api/config/mqtt", timeout=5)
        assert r.status_code == 200

    def test_get_config_actuators(self, base_url):
        r = requests.get(f"{base_url}/api/config/actuators", timeout=5)
        assert r.status_code == 200

    def test_get_rules(self, base_url):
        r = requests.get(f"{base_url}/api/rules", timeout=5)
        assert r.status_code == 200


# ── SPA Serving Tests ─────────────────────────────────────

class TestSPA:
    def test_root_serves_html(self, base_url):
        r = requests.get(f"{base_url}/", timeout=5)
        assert r.status_code == 200
        assert "text/html" in r.headers.get("Content-Type", "")

    def test_gzip_encoding(self, base_url):
        r = requests.get(
            f"{base_url}/",
            headers={"Accept-Encoding": "gzip"},
            timeout=5,
        )
        assert r.status_code == 200

    @pytest.mark.parametrize("path", [
        "/dashboard", "/config", "/sensors", "/actuators",
        "/mqtt", "/rules", "/ota", "/logs",
    ])
    def test_spa_routes_serve_html(self, base_url, path):
        r = requests.get(f"{base_url}{path}", timeout=5)
        assert r.status_code == 200
        assert "text/html" in r.headers.get("Content-Type", "")


# ── Config Write Tests (authenticated) ────────────────────

class TestConfigWrite:
    def test_update_mqtt_config(self, base_url, session_token):
        r = requests.get(f"{base_url}/api/config/mqtt", timeout=5)
        original = r.json()

        r = requests.post(
            f"{base_url}/api/config/mqtt",
            json=original,
            headers={"X-Auth-Token": session_token},
            timeout=5,
        )
        assert r.status_code == 200

    def test_update_sensors_config(self, base_url, session_token):
        r = requests.get(f"{base_url}/api/config/sensors", timeout=5)
        original = r.json()

        r = requests.post(
            f"{base_url}/api/config/sensors",
            json=original,
            headers={"X-Auth-Token": session_token},
            timeout=5,
        )
        assert r.status_code == 200


# ── Relay Control Tests ───────────────────────────────────

class TestRelay:
    def test_relay_toggle_requires_auth(self, base_url):
        r = requests.post(
            f"{base_url}/api/relay",
            json={"id": 0, "state": 1},
            timeout=5,
        )
        assert r.status_code == 401

    def test_relay_toggle(self, base_url, session_token):
        r = requests.post(
            f"{base_url}/api/relay",
            json={"id": 0, "state": 1},
            headers={"X-Auth-Token": session_token},
            timeout=5,
        )
        assert r.status_code == 200


# ── OTA Tests ─────────────────────────────────────────────

class TestOTA:
    def test_ota_upload_requires_auth(self, base_url):
        r = requests.post(f"{base_url}/update", data=b"\x00" * 100, timeout=5)
        assert r.status_code == 401

    def test_ota_requires_otp_validation(self, base_url, session_token):
        r = requests.post(
            f"{base_url}/update",
            data=b"\x00" * 100,
            headers={"X-Auth-Token": session_token},
            timeout=5,
        )
        assert r.status_code == 401


# ── Modbus Scan Tests ─────────────────────────────────────

class TestModbusScan:
    def test_scan_status(self, base_url):
        r = requests.get(f"{base_url}/api/modbus/scan", timeout=5)
        assert r.status_code == 200


# ── SD Card Tests ─────────────────────────────────────────

class TestSDCard:
    def test_sdcard_status(self, base_url):
        r = requests.get(f"{base_url}/api/sdcard/status", timeout=5)
        assert r.status_code in [200, 404]


# ── Edge Cases ────────────────────────────────────────────

class TestEdgeCases:
    def test_404_unknown_path(self, base_url):
        r = requests.get(f"{base_url}/api/nonexistent", timeout=5)
        assert r.status_code == 404

    def test_large_request_body(self, base_url, session_token):
        r = requests.post(
            f"{base_url}/api/config/mqtt",
            data="x" * 2000,
            headers={"X-Auth-Token": session_token},
            timeout=5,
        )
        assert r.status_code in [200, 400, 413]

    def test_options_preflight(self, base_url):
        r = requests.options(f"{base_url}/api/status", timeout=5)
        assert r.status_code == 200
        assert "Access-Control-Allow-Methods" in r.headers
