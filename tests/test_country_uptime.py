"""
Automated validation for Country selection, Timezone Date-Time formatting,
and Uptime display in MQTT and Logs.
"""
import gzip
import re
import os

def test_web_html_has_country_settings():
    with open("web/index.html", "r", encoding="utf-8") as f:
        html = f.read()

    assert "id='page-settings'" in html
    assert "Regional &amp; Time Settings" in html or "Regional & Time Settings" in html
    assert "id='set-country'" in html
    assert "country-live-clock" in html
    assert "country-tz-badge" in html
    assert "COUNTRIES = {" in html
    assert '"EG":' in html
    assert '"SA":' in html
    assert '"AE":' in html
    assert "fmtRealTime(epoch)" in html

def test_web_html_mqtt_uptime_column():
    with open("web/index.html", "r", encoding="utf-8") as f:
        html = f.read()

    assert "<th>Uptime</th>" in html or "<th style='white-space:nowrap;'>Uptime</th>" in html
    assert "insertPlaceholder(\"{{uptime}}\")" in html
    assert "renderMqttLogTable" in html
    assert "badge badge-gray" in html

def test_web_html_logs_uptime():
    with open("web/index.html", "r", encoding="utf-8") as f:
        html = f.read()

    assert "filterAndRenderLogs" in html
    assert "fmtUptime(upSec)" in html

def test_web_assets_gzip_in_sync():
    with open("web/index.html", "rb") as f:
        html_raw = f.read()
    gzipped = gzip.compress(html_raw, compresslevel=9)
    gz_len = len(gzipped)

    with open("APP/web_assets.h", "r", encoding="utf-8") as f:
        header = f.read()

    m = re.search(r"#define KONTRX_HTML_LEN (\d+)", header)
    assert m is not None, "KONTRX_HTML_LEN macro not found"
    header_len = int(m.group(1))
    assert header_len == gz_len, f"web_assets.h len {header_len} does not match gzipped index.html {gz_len}"

def test_freertos_tasks_flat_telemetry_has_uptime():
    with open("APP/freertos_tasks.c", "r", encoding="utf-8") as f:
        content = f.read()

    idx_start = content.find("static void Build_Flat_Telemetry_JSON")
    idx_end = content.find("static void Build_Structured_Telemetry_JSON")
    assert idx_start >= 0 and idx_end > idx_start
    func_body = content[idx_start:idx_end]
    assert "uptime_s" in func_body, "Build_Flat_Telemetry_JSON must output uptime_s"

def test_flash_partition_log_stamp_uptime():
    with open("HAL/flash_partition.c", "r", encoding="utf-8") as f:
        content = f.read()

    idx = content.find("void Log_FormatStamp")
    assert idx >= 0
    func_body = content[idx:idx + 1000]
    assert "*up_s = stamp - boot" in func_body, "Log_FormatStamp must calculate up_s for epoch stamps"

def test_http_server_mqtt_log_has_up_s():
    with open("APP/http_server_task.c", "r", encoding="utf-8") as f:
        content = f.read()

    idx1 = content.find("JSON_MqttLogResponse")
    assert idx1 >= 0
    block1 = content[idx1:idx1 + 2500]
    assert 'up_s' in block1

    idx2 = content.find("HTTP_SSE_PushMqttLog")
    assert idx2 >= 0
    block2 = content[idx2:idx2 + 800]
    assert 'up_s' in block2
