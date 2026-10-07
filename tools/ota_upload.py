#!/usr/bin/env python3
"""
Kontrx Firmware Compile & OTA Flash Tool
Automates building KontrxRTOS.bin and flashing it over HTTP OTA to the Kontrx Controller.
Uses direct raw TCP sockets for 100% reliable compatibility across old and new firmware.
"""

import sys
import os
import time
import socket
import subprocess
import json

DEFAULT_IP = "192.168.1.200"
CANDIDATE_IPS = ["192.168.1.200", "192.168.1.100", "192.168.1.224"]
OTP_SECRET = "KontrxOTA2026"
MAX_FLASH_SIZE_BYTES = 224 * 1024  # 224 KB sector limit


def raw_http_post(ip, path, headers=None, body=b"", timeout=5):
    s = socket.socket()
    s.settimeout(timeout)
    s.connect((ip, 80))
    if isinstance(body, str):
        body = body.encode('utf-8')
    hdr = f"POST {path} HTTP/1.1\r\nHost: {ip}\r\nContent-Length: {len(body)}\r\nConnection: close\r\n"
    if headers:
        for k, v in headers.items():
            if k.lower() not in ("host", "content-length", "connection"):
                hdr += f"{k}: {v}\r\n"
    hdr += "\r\n"
    s.sendall(hdr.encode('utf-8') + body)

    chunks = []
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
        except socket.timeout:
            break
        except Exception:
            break
    s.close()

    raw = b"".join(chunks).decode('utf-8', errors='replace')
    lines = raw.split("\r\n")
    status_code = 0
    if lines and len(lines[0].split()) >= 2:
        try:
            status_code = int(lines[0].split()[1])
        except ValueError:
            pass
    body_part = raw.split("\r\n\r\n", 1)[1] if "\r\n\r\n" in raw else ""
    return status_code, body_part


def find_active_controller(requested_ip=None):
    if requested_ip:
        candidates = [requested_ip] + [ip for ip in CANDIDATE_IPS if ip != requested_ip]
    else:
        candidates = CANDIDATE_IPS

    for ip in candidates:
        try:
            s = socket.socket()
            s.settimeout(1.0)
            s.connect((ip, 80))
            s.close()
            print(f"[+] Found active Kontrx Controller at {ip}")
            return ip
        except Exception:
            pass
    return requested_ip or DEFAULT_IP


def compile_firmware(build_dir="build"):
    print("[*] Compiling Kontrx firmware...")
    if not os.path.exists(build_dir):
        print(f"[*] Initializing CMake build directory '{build_dir}'...")
        cmd_config = [
            "cmake",
            "-B", build_dir,
            "-DCMAKE_TOOLCHAIN_FILE=arm_toolchain.cmake",
            "-G", "Ninja"
        ]
        res_cfg = subprocess.run(cmd_config, capture_output=True, text=True)
        if res_cfg.returncode != 0:
            print(f"[-] CMake configuration failed:\n{res_cfg.stderr}")
            return False

    cmd_build = ["cmake", "--build", build_dir]
    res_build = subprocess.run(cmd_build, capture_output=True, text=True)
    if res_build.returncode != 0:
        print(f"[-] Compilation failed:\n{res_build.stderr}")
        return False

    print("[+] Firmware compiled successfully.")
    return True


def flash_ota(ip, bin_path="build/KontrxRTOS.bin"):
    if not os.path.exists(bin_path):
        print(f"[-] Binary file not found: {bin_path}")
        return False

    bin_size = os.path.getsize(bin_path)
    print(f"[*] Firmware size: {bin_size} bytes ({bin_size / 1024:.2f} KB)")
    if bin_size > MAX_FLASH_SIZE_BYTES:
        print(f"[-] ERROR: Binary exceeds 224 KB staging sector limit ({bin_size} > {MAX_FLASH_SIZE_BYTES})!")
        return False

    with open(bin_path, "rb") as f:
        firmware_bytes = f.read()

    print(f"[*] Connecting to Kontrx Controller at http://{ip}...")

    # Step 0: Authenticate session
    session_token = ""
    try:
        code, body = raw_http_post(
            ip,
            "/api/auth/login",
            headers={"Content-Type": "application/json"},
            body=json.dumps({"username": "admin", "password": "adminkontrx"}),
            timeout=4
        )
        if code == 200:
            data = json.loads(body)
            session_token = data.get("token", "")
            print(f"[+] Authenticated with controller (session: {session_token[:8]}...)")
    except Exception as e:
        print(f"[!] Authentication warning: {e}")

    auth_headers = {"Cookie": f"kx_session={session_token}"} if session_token else {}

    # Step 1: Verify OTP
    otp_ok = False
    for attempt in range(3):
        time.sleep(0.3)
        try:
            v_headers = {"Content-Type": "application/json"}
            if session_token:
                v_headers["Cookie"] = f"kx_session={session_token}"
            code, body = raw_http_post(
                ip,
                "/api/ota/verify",
                headers=v_headers,
                body=json.dumps({"otp": OTP_SECRET}),
                timeout=3
            )
            if code == 200 and '"ok":true' in body:
                otp_ok = True
                print("[+] OTA OTP verified successfully.")
                break
            else:
                print(f"[!] OTP attempt {attempt+1} returned HTTP {code}: {body}")
        except Exception as e:
            print(f"[!] OTP attempt {attempt+1} error: {e}")
            time.sleep(1.0)

    if not otp_ok:
        print("[-] OTP verification failed after retries.")
        return False

    time.sleep(0.5)

    # Step 2: Prepare flash (erases staging sectors 6 & 7)
    try:
        code, body = raw_http_post(ip, "/api/ota/prepare", headers=auth_headers, timeout=5)
        print(f"[+] Flash prepare request sent (HTTP {code}). Erasing staging flash...")
    except Exception as e:
        print(f"[!] Prepare request warning: {e}")

    # Wait for physical MCU flash sector erase
    time.sleep(3.5)

    # Step 3: Stream update binary
    print(f"[*] Uploading firmware binary ({bin_size} bytes) over OTA...")
    for attempt in range(3):
        try:
            s = socket.socket()
            s.settimeout(30.0)
            s.connect((ip, 80))

            hdr_str = (
                f"POST /update HTTP/1.1\r\n"
                f"Host: {ip}\r\n"
                f"Content-Type: application/octet-stream\r\n"
                f"Content-Length: {len(firmware_bytes)}\r\n"
            )
            if session_token:
                hdr_str += f"Cookie: kx_session={session_token}\r\n"
            hdr_str += "Connection: close\r\n\r\n"

            s.sendall(hdr_str.encode('utf-8'))

            sent = 0
            chunk_size = 1024
            while sent < len(firmware_bytes):
                chunk = firmware_bytes[sent:sent + chunk_size]
                s.sendall(chunk)
                sent += len(chunk)
                pct = (sent / len(firmware_bytes)) * 100
                print(f"\r[*] Progress: {sent}/{len(firmware_bytes)} bytes ({pct:.1f}%)", end="", flush=True)
                time.sleep(0.005)

            print("\n[*] Upload stream finished. Waiting for reboot acknowledgment...")

            chunks = []
            while True:
                try:
                    c = s.recv(4096)
                    if not c:
                        break
                    chunks.append(c)
                except Exception:
                    break
            s.close()

            raw_resp = b"".join(chunks).decode('utf-8', errors='replace')
            if "200 OK" in raw_resp or "OTA OK" in raw_resp or "Rebooting" in raw_resp:
                print(f"[SUCCESS] OTA Flash Complete! Response: {raw_resp.strip()}")
                return True
            else:
                print(f"[-] OTA Flash response: {raw_resp}")
                if "CRC" in raw_resp:
                    return False
        except Exception as e:
            print(f"\n[!] Upload attempt {attempt+1} error: {e}")
            time.sleep(2.0)

    print("[-] Firmware update failed after retries.")
    return False


def main():
    target_ip = sys.argv[1] if (len(sys.argv) > 1 and not sys.argv[1].startswith("--")) else None
    skip_compile = "--no-compile" in sys.argv

    if not skip_compile:
        if not compile_firmware():
            sys.exit(1)

    active_ip = find_active_controller(target_ip)
    print(f"[*] Target Controller IP: {active_ip}")

    success = flash_ota(active_ip)
    if success:
        print("[+] Firmware update finished successfully. Controller is rebooting into new firmware...")
        time.sleep(6.0)
        # Verify controller came back online
        try:
            s = socket.socket()
            s.settimeout(3.0)
            s.connect((active_ip, 80))
            s.close()
            print("[+] Controller is ONLINE on port 80!")
        except Exception as e:
            print(f"[*] Controller still rebooting... ({e})")
        sys.exit(0)
    else:
        print("[-] Firmware update failed.")
        sys.exit(1)


if __name__ == "__main__":
    main()
