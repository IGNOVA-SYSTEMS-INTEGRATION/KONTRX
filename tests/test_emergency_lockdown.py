#!/usr/bin/env python3
"""
Test Suite for Emergency Lockdown Route, Redirection, UI Modal Layer, and Real-Time Synchronization.
"""

import gzip
import re
import os
import sys

def test_http_server_route_and_redirect():
    c_path = "APP/http_server_task.c"
    assert os.path.exists(c_path), f"{c_path} not found"
    with open(c_path, "r", encoding="utf-8") as f:
        c_code = f.read()

    # 1. /lockdown and /emergency registered in Is_Page_Route
    assert 'strncmp(line, "GET /lockdown", 13) == 0' in c_code, "GET /lockdown not in Is_Page_Route"
    assert 'strncmp(line, "GET /emergency", 14) == 0' in c_code, "GET /emergency not in Is_Page_Route"

    # 2. Critical lock 302 redirect logic
    assert "if (SDCard_IsCriticalLocked())" in c_code, "SDCard_IsCriticalLocked check missing in Dispatch_Request"
    assert "Location: /lockdown" in c_code, "302 Location: /lockdown missing"
    assert "HTTP/1.1 302 Found" in c_code, "HTTP 302 Found response missing"
    print("[PASS] test_http_server_route_and_redirect")

def test_web_modal_layer_and_z_index():
    html_path = "web/index.html"
    assert os.path.exists(html_path), f"{html_path} not found"
    with open(html_path, "r", encoding="utf-8") as f:
        html = f.read()

    # Find #sd-lockdown-screen z-index
    lockdown_match = re.search(r"id=['\"]sd-lockdown-screen['\"][^>]*style=['\"]([^'\"]*)['\"]", html)
    assert lockdown_match, "#sd-lockdown-screen element with style not found"
    lockdown_style = lockdown_match.group(1)
    z_lockdown_m = re.search(r"z-index:\s*(\d+)", lockdown_style)
    assert z_lockdown_m, "z-index not found on #sd-lockdown-screen"
    z_lockdown = int(z_lockdown_m.group(1))

    # Find #sd-unlock-modal z-index
    unlock_match = re.search(r"id=['\"]sd-unlock-modal['\"][^>]*style=['\"]([^'\"]*)['\"]", html)
    assert unlock_match, "#sd-unlock-modal element with style not found"
    unlock_style = unlock_match.group(1)
    z_unlock_m = re.search(r"z-index:\s*(\d+)", unlock_style)
    assert z_unlock_m, "z-index not found on #sd-unlock-modal"
    z_unlock = int(z_unlock_m.group(1))

    assert z_unlock > z_lockdown, f"Modal z-index ({z_unlock}) must be greater than lockdown screen z-index ({z_lockdown})"
    print(f"[PASS] test_web_modal_layer_and_z_index (modal z-index={z_unlock} > screen z-index={z_lockdown})")

def test_web_realtime_and_routing():
    html_path = "web/index.html"
    with open(html_path, "r", encoding="utf-8") as f:
        html = f.read()

    # 1. checkSdLock updates window.isSystemCriticalLocked and pushes state to /lockdown
    assert "window.isSystemCriticalLocked = true" in html, "window.isSystemCriticalLocked = true missing"
    assert '"/lockdown"' in html, "pushState /lockdown missing"

    # 2. showTab intercepts navigation when locked
    assert "window.isSystemCriticalLocked" in html, "showTab does not check window.isSystemCriticalLocked"
    assert 'checkSdLock(true)' in html, "showTab does not force checkSdLock(true)"

    # 3. showTab does NOT stop SSE on tab switch
    # Verify stopSSE is not in showTab
    idx_show_tab = html.find("function showTab(e,t,a){")
    idx_end_tab = html.find("if(\"mqtt\"===t){refreshMqttLog();}", idx_show_tab)
    tab_hdr = html[idx_show_tab:idx_end_tab]
    assert "stopSSE()" not in tab_hdr, "stopSSE() should not be called in showTab"

    # 4. SSE auto-reconnect logic
    assert "sseReconnectTimer" in html, "sseReconnectTimer missing"
    assert "setTimeout" in html and "startSSE" in html, "SSE auto-reconnect timeout missing"

    # 5. Heartbeat fallback polling
    assert "setInterval" in html and "fetchStatus" in html, "Fallback heartbeat polling missing"

    print("[PASS] test_web_realtime_and_routing")

def test_web_assets_synced():
    html_path = "web/index.html"
    assets_path = "APP/web_assets.h"
    assert os.path.exists(assets_path), f"{assets_path} not found"

    with open(html_path, "rb") as f:
        raw_html = f.read()
    gzipped_html = gzip.compress(raw_html, 9)

    with open(assets_path, "r", encoding="utf-8") as f:
        header = f.read()

    m = re.search(r"#define (?:WEB_ASSET_INDEX_GZ_LEN|KONTRX_HTML_LEN)\s+(\d+)", header)
    assert m, "KONTRX_HTML_LEN not found in APP/web_assets.h"
    len_in_h = int(m.group(1))

    # Allow slight compression variance (+- 10 bytes)
    assert abs(len(gzipped_html) - len_in_h) < 15, f"Size mismatch: computed {len(gzipped_html)} vs header {len_in_h}"
    print(f"[PASS] test_web_assets_synced (gzipped {len_in_h} bytes)")


def test_sd_rearm_and_card_presence_requirement():
    with open("HAL/sdcard.c", "r", encoding="utf-8") as f:
        sd = f.read()

    # Verify physical presence check is mandatory before unlock
    assert "SDCard_Hardware_Check_Present() == 0" in sd, "Physical SD presence check missing in SDCard_UnlockWithPassword"
    assert "return 2;" in sd, "Return code 2 (card missing) not returned"

    # Verify supervisor is re-armed
    assert "s_sd_override_active = 0;" in sd, "s_sd_override_active not reset to 0 to re-arm supervisor"
    # Verify s_sd_override_active does NOT permanently bypass poll lockout
    assert "if (s_sd_override_active) return 0;" not in sd, "Permanent lockout bypass still present in poll function"

    with open("APP/http_server_task.c", "r", encoding="utf-8") as f:
        http = f.read()
    assert "card_missing" in http, "card_missing JSON response missing in http_server_task.c"

    with open("web/index.html", "r", encoding="utf-8") as f:
        html = f.read()
    assert "res.card_missing" in html, "card_missing handling missing in web/index.html submitSdAdminUnlock"
    assert "sd-card-hw-status" in html, "SD card hardware socket status indicator missing in lockdown UI"
    print("[PASS] test_sd_rearm_and_card_presence_requirement")

def test_zero_emojis_and_valid_svg_icons():
    import unicodedata
    with open("web/index.html", "r", encoding="utf-8") as f:
        html = f.read()

    # Check for emojis or unstandardized symbols
    for i, line in enumerate(html.splitlines(), 1):
        for ch in line:
            code = ord(ch)
            if code > 127:
                cat = unicodedata.category(ch)
                if cat.startswith("S") and ch not in ["°", "µ"]:
                    raise AssertionError(f"Symbol {ch} (U+{code:04X}) found at line {i}: {line[:80]}")

    # Check required SVGs are defined
    for sym in ["i-key", "i-alert", "i-flash", "i-qr", "i-sound", "i-first", "i-arrow-r", "i-arrow-l", "i-download"]:
        assert f"id='{sym}'" in html or f'id="{sym}"' in html, f"SVG symbol {sym} not defined in defs"

    print("[PASS] test_zero_emojis_and_valid_svg_icons")

def main():
    test_http_server_route_and_redirect()
    test_web_modal_layer_and_z_index()
    test_web_realtime_and_routing()
    test_web_assets_synced()
    test_sd_rearm_and_card_presence_requirement()
    test_zero_emojis_and_valid_svg_icons()
    print("\nALL EMERGENCY LOCKDOWN TESTS PASSED (6/6)!")

if __name__ == "__main__":
    main()
