import json
import os
import re
import pytest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INDEX_HTML = os.path.join(REPO_ROOT, "web", "index.html")

def test_qr_payload_generation_logic():
    with open(INDEX_HTML, "r", encoding="utf-8") as f:
        html = f.read()

    assert "function generateQR" in html
    assert "copyQRPayload" in html
    assert "v.type=tName" in html
    assert "v.pinout=pin" in html
    assert "v.pinouts=pin" in html
    assert "window.lastQRPayload=y" in html

def test_multi_us_included_in_sensor_cards():
    with open(INDEX_HTML, "r", encoding="utf-8") as f:
        html = f.read()

    idx = html.find("function buildSensorCards")
    assert idx >= 0
    fn_body = html[idx:idx + 2000]
    # Verify multi_us is no longer excluded
    assert 'if("multi_us"!==n.type)' not in fn_body
    # Verify multi_us handling is present
    assert '"multi_us"===n.type' in fn_body
