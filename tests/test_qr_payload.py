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

def test_qr_payload_does_not_contain_live_sensor_or_actuator_telemetry():
    """Verify QR code logic excludes volatile values (value, temp, status, relay state, duty, pos, ma, v)."""
    with open(INDEX_HTML, "r", encoding="utf-8") as f:
        html = f.read()

    idx = html.find("function generateQR")
    assert idx >= 0
    end_idx = html.find("function fileSelected", idx)
    qr_fn = html[idx:end_idx]

    # No sensor readings or live status in payload
    assert "l.v=" not in qr_fn
    assert "l.temp=" not in qr_fn
    assert "l.dist=" not in qr_fn
    assert "l.avg=" not in qr_fn
    assert "status:d&&d.valid" not in qr_fn

    # No actuator states or motion telemetry in payload
    assert "v.s=" not in qr_fn
    assert "v.duty=" not in qr_fn
    assert "v.pos=" not in qr_fn
    assert "v.steps=" not in qr_fn
    assert "v.location=" not in qr_fn
    assert "v.moving=" not in qr_fn
    assert "v.current_ma=" not in qr_fn
    assert "v.voltage_v=" not in qr_fn

    # Has early return when payload matches last payload
    assert "if(y===window.lastQRPayload&&window.lastGeneratedQR)return;" in qr_fn
