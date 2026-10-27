#!/usr/bin/env python3
"""
Test Suite for Multi-Sensor Board Real-time Telemetry Synchronization and State Persistence.
Verifies that SSE pushes full flow and summary_decoded fields, and that the frontend
uses Smart Deep-Merge to prevent flickering or losing sensor metrics.
"""

import gzip
import re
import os
import subprocess
import sys

def test_backend_sse_telemetry():
    c_path = "APP/http_server_task.c"
    assert os.path.exists(c_path), f"{c_path} not found"
    with open(c_path, "r", encoding="utf-8") as f:
        code = f.read()

    # Find Build_Live_Telemetry_JSON definition
    idx_start = code.find("Build_Live_Telemetry_JSON")
    assert idx_start != -1, "Build_Live_Telemetry_JSON not found in http_server_task.c"
    idx_end = code.find("return pos;", idx_start)
    assert idx_end != -1, "return pos not found after Build_Live_Telemetry_JSON"
    fn_body = code[idx_start:idx_end]

    # Verify flow and summary_decoded are serialized in SSE
    assert r'\"flow\":[' in fn_body, "flow array missing from Build_Live_Telemetry_JSON multi_sensor"
    assert r'\"rate\":' in fn_body, "flow rate missing from Build_Live_Telemetry_JSON"
    assert r'\"summary_decoded\":{' in fn_body, "summary_decoded object missing from Build_Live_Telemetry_JSON"
    assert r'\"flow_total\":' in fn_body, "flow_total missing from Build_Live_Telemetry_JSON summary_decoded"
    assert r'\"volume\":' in fn_body, "volume missing from Build_Live_Telemetry_JSON summary_decoded"
    assert r'\"uptime\":' in fn_body, "uptime missing from Build_Live_Telemetry_JSON summary_decoded"
    print("[PASS] test_backend_sse_telemetry")

def test_frontend_smart_deep_merge():
    html_path = "web/index.html"
    assert os.path.exists(html_path), f"{html_path} not found"
    with open(html_path, "r", encoding="utf-8") as f:
        html = f.read()

    # Verify smart deep-merge exists in SSE handler
    assert "existing.flow = newBd.flow" in html, "existing.flow merge logic missing in SSE handler"
    assert "existing.summary_decoded = newBd.summary_decoded" in html, "existing.summary_decoded merge logic missing in SSE handler"
    
    # Verify buildSensorCards() is called after multi_sensor merge
    idx_ms = html.find("d.multi_sensor.forEach")
    idx_cards = html.find("buildSensorCards()", idx_ms)
    assert idx_cards != -1 and idx_cards > idx_ms, "buildSensorCards() must be called after multi_sensor merge"
    print("[PASS] test_frontend_smart_deep_merge")

def test_javascript_syntax_validity():
    html_path = "web/index.html"
    with open(html_path, "r", encoding="utf-8") as f:
        html = f.read()

    scripts = re.findall(r'<script(?:\s+[^>]*)?>(.*?)</script>', html, re.DOTALL)
    for i, s in enumerate(scripts):
        path = f"scratch/test_ms_script_{i}.js"
        with open(path, "w", encoding="utf-8") as sf:
            sf.write(s)
        res = subprocess.run(["node", "--check", path], capture_output=True, text=True)
        if os.path.exists(path):
            os.remove(path)
        assert res.returncode == 0, f"JS Syntax Error in script {i}:\n{res.stderr}"

    print(f"[PASS] test_javascript_syntax_validity ({len(scripts)} scripts verified)")

def test_web_assets_synchronized():
    html_path = "web/index.html"
    assets_path = "APP/web_assets.h"
    with open(html_path, "rb") as f:
        raw_html = f.read()
    gzipped_html = gzip.compress(raw_html, 9)

    with open(assets_path, "r", encoding="utf-8") as f:
        header = f.read()

    m = re.search(r"#define (?:WEB_ASSET_INDEX_GZ_LEN|KONTRX_HTML_LEN)\s+(\d+)", header)
    assert m, "KONTRX_HTML_LEN not found in APP/web_assets.h"
    len_in_h = int(m.group(1))

    assert abs(len(gzipped_html) - len_in_h) < 15, f"Size mismatch: computed {len(gzipped_html)} vs header {len_in_h}"
    print(f"[PASS] test_web_assets_synchronized (gzipped {len_in_h} bytes)")

def main():
    test_backend_sse_telemetry()
    test_frontend_smart_deep_merge()
    test_javascript_syntax_validity()
    test_web_assets_synchronized()
    print("\nALL MULTI-SENSOR TELEMETRY TESTS PASSED (4/4)!")

if __name__ == "__main__":
    main()
