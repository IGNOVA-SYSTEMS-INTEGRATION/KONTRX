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
    assert "switchQRTab" in html
    assert "boardPayloadObj" in html
    assert "rulesPayloadObj" in html
    assert "window.lastBoardQRPayload" in html
    assert "window.lastRulesQRPayload" in html

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

def test_qr_payload_clean_structure():
    """Verify QR code logic produces clean board and rules objects without live volatile values."""
    with open(INDEX_HTML, "r", encoding="utf-8") as f:
        html = f.read()

    idx = html.find("function generateQR")
    assert idx >= 0
    end_idx = html.find("function fileSelected", idx)
    qr_fn = html[idx:end_idx]

    # No volatile sensor readings
    assert "l.v=" not in qr_fn
    assert "l.temp=" not in qr_fn
    assert "l.dist=" not in qr_fn
    assert "l.avg=" not in qr_fn

    # No volatile actuator states or live measurements
    assert "v.s=" not in qr_fn
    assert "v.duty=" not in qr_fn
    assert "v.pos=" not in qr_fn
    assert "v.steps=" not in qr_fn
    assert "v.current_ma=" not in qr_fn
    assert "v.voltage_v=" not in qr_fn

    # Has separate board and rules generation
    assert "boardPayloadObj" in qr_fn
    assert "rulesPayloadObj" in qr_fn
