"""
IoT 100-issue audit — static regression checks (no device needed).

Each check maps to an item ID in docs/tests/05_IOT_100_AUDIT.md.
  * Plain tests      -> controls that exist today (must stay green).
  * xfail(strict)    -> open findings. When a finding is fixed the test
                        XPASSes, the suite fails, and the marker must be
                        removed so the fix stays guarded.
Run: pytest tests/test_iot_audit.py -v
"""
import os
import re

import pytest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def src(rel):
    with open(os.path.join(REPO_ROOT, rel), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def firmware_sources():
    out = []
    for d in ("APP", "HAL", "MCAL"):
        for root, _, files in os.walk(os.path.join(REPO_ROOT, d)):
            for n in files:
                if n.endswith((".c", ".h")) and n not in ("cJSON.c", "cJSON.h", "web_assets.h"):
                    out.append(os.path.join(root, n))
    return out


def all_fw():
    text = []
    for p in firmware_sources():
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            text.append(f.read())
    return "\n".join(text)


def func_body(text, signature):
    i = text.find(signature)
    assert i >= 0, f"{signature} not found"
    end = text.find("\n}", i)
    return text[i:end]


HTTP = src("APP/http_server_task.c")
TASKS = src("APP/freertos_tasks.c")
SDCARD = src("HAL/sdcard.c")
BOOT = src("APP/bootloader.c")
WEB = src("web/index.html")


def open_finding(reason):
    return pytest.mark.xfail(strict=True, reason=reason)


# ---------------------------------------------------------------- Security
class TestSecurity:
    @open_finding("SEC-02: SD unlock accepts hardcoded adminkontrx/KontrxMaster2026 even after password change")
    def test_sec02_no_backdoor_passwords_in_sd_unlock(self):
        body = func_body(SDCARD, "uint8_t SDCard_UnlockWithPassword")
        assert '"KontrxMaster2026"' not in body and 'strcmp(pwd, "adminkontrx")' not in body

    @open_finding("SEC-02: OTA OTP is a fixed compile-time string")
    def test_sec02_ota_otp_not_hardcoded(self):
        assert not re.search(r'#define\s+OTA_OTP_SECRET\s+"', HTTP)

    @open_finding("SEC-05: session token from LCG of tick/DWT, hardware RNG unused")
    def test_sec05_session_token_uses_hw_rng(self):
        body = func_body(HTTP, "static void Generate_Session_Token")
        assert "RNG" in body

    @open_finding("SEC-05: session has no expiry / idle timeout")
    def test_sec05_session_expiry(self):
        assert re.search(r"SESSION_(TIMEOUT|EXPIRY|TTL)", HTTP)

    def test_sec06_login_lockout_exists(self):
        assert "LOGIN_MAX_ATTEMPTS" in HTTP and "s_login_lockout_until" in HTTP

    def test_sec06_session_cookie_httponly_samesite(self):
        assert "HttpOnly" in HTTP and "SameSite=Strict" in HTTP

    @open_finding("SEC-04: CORS '*' and Origin reflected with Allow-Credentials")
    def test_sec04_no_wildcard_cors(self):
        assert "Access-Control-Allow-Origin: *" not in HTTP

    @open_finding("SEC-09: POST /api/rules* auth gate disabled (configurator TODO)")
    def test_sec09_rules_write_requires_auth(self):
        assert "TEMPORARILY DISABLED" not in HTTP

    @open_finding("SEC-09: POST /api/provision is unauthenticated and rewrites MQTT broker")
    def test_sec09_provision_requires_auth(self):
        i = HTTP.find('strncmp(line, "POST /api/provision", 19) == 0')
        if i < 0:
            i = HTTP.find("POST /api/provision ")
        block = HTTP[i:i + 600]
        assert "Is_Session_Valid" in block

    @open_finding("SEC-08: web UI has no HTML-escape helper before innerHTML")
    def test_sec08_web_has_escape_helper(self):
        assert re.search(r"function\s+(esc|escapeHtml|escHtml)\s*\(", WEB)

    @open_finding("SEC-11: no X-Frame-Options / CSP / nosniff headers")
    def test_sec11_security_headers(self):
        assert "X-Frame-Options" in HTTP or "Content-Security-Policy" in HTTP

    @open_finding("SEC-10: Modbus TCP writes gated only by test_mode, no client allow-list")
    def test_sec10_modbus_tcp_write_acl(self):
        mb = src("HAL/modbus_tcp_server.c")
        assert re.search(r"allow|whitelist|acl", mb, re.I)


# ---------------------------------------------------------------- OTA / boot
class TestOtaBoot:
    @open_finding("OTA-01: firmware image has no cryptographic signature")
    def test_ota01_image_signature(self):
        assert re.search(r"ecdsa|ed25519|sha256|signature", all_fw(), re.I)

    @open_finding("OTA-02: bootloader never checks meta->crc32 before copying/jumping")
    def test_ota02_bootloader_verifies_crc(self):
        assert re.search(r"crc", BOOT, re.I)

    @open_finding("OTA-03: no trial boot / rollback to previous image")
    def test_ota03_rollback(self):
        assert re.search(r"rollback|trial|confirm", BOOT, re.I)

    def test_ota06_image_size_bounded(self):
        assert "content_length > 262144" in HTTP

    @open_finding("OTA-06: 256 KB limit overlaps config at 0x0807C000 inside staging sector 7")
    def test_ota06_size_limit_excludes_config_area(self):
        assert "content_length > 262144" not in HTTP

    def test_ota04_meta_cleared_only_after_verify(self):
        v = BOOT.find("Verification successful")
        e = BOOT.find("FLASH_EraseSector(1)")
        assert 0 <= v < e, "OTA flag must be cleared only after copy verify (power-loss safe)"


# ---------------------------------------------------------------- Reliability
class TestReliability:
    @open_finding("REL-01: no independent watchdog (IWDG); hang or halt = dead until power cycle")
    def test_rel01_iwdg(self):
        assert "IWDG" in all_fw()

    def test_rel04_stack_overflow_check_level2(self):
        assert re.search(r"configCHECK_FOR_STACK_OVERFLOW\s+2", src("FreeRTOSConfig.h"))

    def test_rel05_malloc_failed_hook(self):
        assert "vApplicationMallocFailedHook" in src("APP/main_kontrx.c")

    @open_finding("REL-07: RCC_CSR reset flags read but never cleared (RMVF), cause accumulates")
    def test_rel07_reset_flags_cleared(self):
        assert "RMVF" in all_fw() or "(1U << 24)" in SDCARD

    def test_rel03_fault_supervisor_records_faults(self):
        assert "g_fault_record" in src("APP/fault_supervisor.c")

    @open_finding("REL-02: no per-task heartbeat; a task stuck in a loop is never detected")
    def test_rel02_task_heartbeat(self):
        assert re.search(r"heartbeat|checkin|kick", src("APP/fault_supervisor.c"), re.I)


# ---------------------------------------------------------------- Networking
class TestNetworking:
    @open_finding("NET-11: MAC 00:08:DC:11:22:33 hardcoded; every unit has the same MAC")
    def test_net11_unique_mac(self):
        assert "0x22, 0x33}" not in src("APP/main_kontrx.c")

    @open_finding("NET-02: IP fixed at 192.168.1.200, self-heal re-applies hardcoded IP")
    def test_net02_ip_not_hardcoded_in_self_heal(self):
        body = func_body(TASKS, "void Ensure_W5500_Network_Alive")
        assert "{192, 168, 1, 200}" not in body

    def test_net07_mqtt_keepalive_and_lwt(self):
        assert "keepAliveInterval" in TASKS and "willFlag = 1" in TASKS

    @open_finding("NET-08: fixed 5 s retry, no exponential backoff + jitter")
    def test_net08_backoff_jitter(self):
        assert re.search(r"backoff\s*\*=|jitter", TASKS, re.I)

    def test_net05_http_rejects_oversized_body(self):
        assert "413 Payload Too Large" in HTTP


# ---------------------------------------------------------------- Telemetry
class TestTelemetry:
    def test_tel06_offline_queue_overwrites_oldest(self):
        assert "advance read pointer" in src("HAL/flash_partition.c")

    @open_finding("TEL-07: Sparkplug bdSeq always 0 in NBIRTH/NDEATH")
    def test_tel07_bdseq_increments(self):
        assert not re.search(r'"bdSeq",\s*timestamp_ms,\s*0\)', src("APP/sparkplug_b_enc.c"))

    def test_tel03_unsynced_time_flagged(self):
        assert "TIME_UPTIME_FLAG" in src("APP/timekeeper.h")


# ---------------------------------------------------------------- Modbus RTU
class TestModbus:
    def test_mb02_reply_checks_slave_fc_crc(self):
        mb = src("HAL/modbus_dma.c")
        assert "rx[0] != slave" in mb and "rx[1] != fc" in mb and "Modbus_CRC16(rx, received-2)" in mb

    @open_finding("MB-06: float registers decoded with no NaN/Inf/range check")
    def test_mb06_float_nan_guard(self):
        assert re.search(r"isnan|isfinite", src("HAL/modbus_dma.c"))

    def test_mb10_float_word_order_handled(self):
        assert "Decode_Float_DCBA" in src("HAL/modbus_dma.c")

    def test_mb05_stale_readings_excluded_from_rules(self):
        assert "!sd->readings[j].stale" in TASKS

    @open_finding("SEC-10: unsupported function codes get no exception reply (client times out)")
    def test_mbtcp_exception_for_unknown_fc(self):
        mb = src("HAL/modbus_tcp_server.c")
        assert re.search(r"0x80|exception", mb, re.I)


# ---------------------------------------------------------------- Actuation
class TestActuation:
    @open_finding("ACT-02: Relay_Init calls Relay_SetState(i,0) which is a no-op (state already 0),"
                  " so active-low (NC) relay pins stay LOW = energised after boot")
    def test_act02_relay_init_drives_safe_level(self):
        body = func_body(TASKS, "void Relay_Init(void)")
        assert "BSRR" in body or "ODR" in body

    def test_act08_analog_outputs_clamped(self):
        assert "voltage > 10.0f" in src("HAL/analog_010v.c")
        assert "mA > 20.5f" in src("HAL/dac_420ma.c")

    def test_act07_pto_limit_switch_estop(self):
        assert "PTO_EmergencyStop" in src("HAL/pto_motion.h")

    @open_finding("ACT-01: no per-actuator fail-safe state on comms/sensor loss or supervisor halt")
    def test_act01_failsafe_state(self):
        assert re.search(r"fail_?safe", TASKS + src("HAL/modbus_dma.h"), re.I)

    @open_finding("ACT-05: MQTT relay_N target parses one digit only (relay_10 -> relay 1)")
    def test_act_mqtt_multi_digit_relay_index(self):
        assert "relay_idx = target_str[6] - '0';" not in TASKS


# ---------------------------------------------------------------- Storage
class TestStorage:
    def test_sto03_dual_sector_config_with_crc(self):
        fp = src("HAL/flash_partition.c")
        assert "CONFIG_BACKUP_ADDR" in fp and "Compute_CRC32" in fp

    @open_finding("STO-02: config with bad CRC is still loaded if magic matches (CRC-skip)")
    def test_sto02_no_crc_skip_fallback(self):
        assert "CRC-skip migration" not in src("HAL/flash_partition.c")

    @open_finding("STO-08: admin/master/MQTT passwords stored in plaintext in config")
    def test_sto_passwords_hashed(self):
        assert re.search(r"pbkdf2|sha256|hash", src("HAL/flash_partition.c"), re.I)
