"""
Tests for the web assets build pipeline (extract + rebuild).
Run: pytest tests/test_web_assets.py -v
"""
import gzip
import os
import re
import struct
import tempfile

import pytest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_ASSETS_H = os.path.join(REPO_ROOT, "APP", "web_assets.h")
WEB_INDEX = os.path.join(REPO_ROOT, "web", "index.html")


def read_header_bytes(path: str) -> bytes:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()
    hex_values = re.findall(r"0x([0-9a-fA-F]{2})", content)
    return bytes(int(h, 16) for h in hex_values)


def read_header_len(path: str) -> int:
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    m = re.search(r"KONTRX_HTML_LEN\s+(\d+)", content)
    return int(m.group(1)) if m else -1


class TestWebAssetsHeader:
    def test_header_exists(self):
        assert os.path.isfile(WEB_ASSETS_H)

    def test_gzip_magic_bytes(self):
        data = read_header_bytes(WEB_ASSETS_H)
        assert data[0] == 0x1F and data[1] == 0x8B, "Not a valid gzip stream"

    def test_length_matches_array(self):
        declared = read_header_len(WEB_ASSETS_H)
        data = read_header_bytes(WEB_ASSETS_H)
        assert declared == len(data), f"KONTRX_HTML_LEN={declared} but array has {len(data)} bytes"

    def test_decompresses_to_valid_html(self):
        data = read_header_bytes(WEB_ASSETS_H)
        html = gzip.decompress(data).decode("utf-8")
        assert "<!DOCTYPE html>" in html or "<html" in html
        assert "<title>" in html
        assert "</html>" in html

    def test_contains_kontrx_branding(self):
        data = read_header_bytes(WEB_ASSETS_H)
        html = gzip.decompress(data).decode("utf-8")
        assert "Kontrx" in html

    def test_size_under_flash_budget(self):
        data = read_header_bytes(WEB_ASSETS_H)
        assert len(data) < 65536, f"Gzipped SPA is {len(data)} bytes, exceeds 64KB flash budget"

    def test_has_viewport_meta(self):
        data = read_header_bytes(WEB_ASSETS_H)
        html = gzip.decompress(data).decode("utf-8")
        assert "viewport" in html

    def test_has_dark_theme_support(self):
        data = read_header_bytes(WEB_ASSETS_H)
        html = gzip.decompress(data).decode("utf-8")
        assert "dark-theme" in html or "dark_theme" in html


class TestWebAssetsRoundtrip:
    def test_extract_and_rebuild_matches(self):
        original_data = read_header_bytes(WEB_ASSETS_H)
        html = gzip.decompress(original_data)

        with tempfile.NamedTemporaryFile(suffix=".html", delete=False, mode="wb") as tmp_html:
            tmp_html.write(html)
            tmp_html_path = tmp_html.name

        with tempfile.NamedTemporaryFile(suffix=".h", delete=False, mode="w") as tmp_h:
            tmp_h_path = tmp_h.name

        try:
            import subprocess
            build_script = os.path.join(REPO_ROOT, "tools", "build_web_assets.py")
            subprocess.run(
                ["python", build_script, "build", "--input", tmp_html_path, "--output", tmp_h_path],
                check=True, capture_output=True,
            )
            rebuilt_data = read_header_bytes(tmp_h_path)
            rebuilt_html = gzip.decompress(rebuilt_data)
            assert rebuilt_html == html, "Round-trip HTML content mismatch"
        finally:
            os.unlink(tmp_html_path)
            os.unlink(tmp_h_path)


class TestExtractedHTML:
    @pytest.fixture
    def html_content(self):
        data = read_header_bytes(WEB_ASSETS_H)
        return gzip.decompress(data).decode("utf-8")

    def test_has_login_page(self, html_content):
        assert "page-login" in html_content

    def test_has_dashboard_page(self, html_content):
        assert "page-dashboard" in html_content

    def test_has_responsive_css(self, html_content):
        assert "@media" in html_content

    def test_no_inline_credentials(self, html_content):
        assert "adminkontrx" not in html_content

    def test_has_csrf_protection(self, html_content):
        assert "X-Auth-Token" in html_content or "kx_session" in html_content

    def test_all_pages_present(self, html_content):
        expected_pages = [
            "page-login", "page-dashboard", "page-configs",
            "page-rules", "page-mqtt", "page-logs",
            "page-ota", "page-settings",
        ]
        for page in expected_pages:
            assert page in html_content, f"Missing page: {page}"

    def test_toast_notification_system(self, html_content):
        assert "showToast" in html_content

    def test_pwa_meta_tags(self, html_content):
        assert "theme-color" in html_content
        assert "apple-mobile-web-app" in html_content
