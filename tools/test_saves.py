import requests
import json
import time

BASE_URL = "http://192.168.1.200"

def run_test():
    session = requests.Session()
    session.headers.update({"Connection": "close"})
    
    print("1. Testing GET /api/status...")
    try:
        r = session.get(f"{BASE_URL}/api/status", timeout=5)
        print(f"Status: {r.status_code}, length={len(r.text)}")
    except Exception as e:
        print(f"GET /api/status failed: {e}")
        return

    print("\n2. Testing POST /api/auth/login...")
    try:
        r = session.post(f"{BASE_URL}/api/auth/login", json={"username": "admin", "password": "adminkontrx"}, timeout=5)
        print(f"Login status: {r.status_code}, text={r.text}")
    except Exception as e:
        print(f"Login failed: {e}")
        return

    print("\n3. Testing GET /configs (HTML page)...")
    try:
        r = session.get(f"{BASE_URL}/configs", timeout=5)
        print(f"Configs page status: {r.status_code}, length={len(r.text)}")
    except Exception as e:
        print(f"GET /configs failed: {e}")

    print("\n4. Testing POST /api/config/mqtt...")
    try:
        payload = {
            "broker": "192.168.1.18",
            "port": 1883,
            "client_id": "kontrx-KX-0000002",
            "username": "",
            "password": "",
            "interval": 2,
            "sparkplug_topic": "test/topic/12345",
            "send_mode": 0,
            "payload_shape": 2,
            "enabled": 1,
            "skip_offline": 0
        }
        r = session.post(f"{BASE_URL}/api/config/mqtt", json=payload, timeout=5)
        print(f"POST /api/config/mqtt status: {r.status_code}, text={r.text}")
    except Exception as e:
        print(f"POST /api/config/mqtt failed: {e}")

    print("\n5. Testing GET /configs immediately after save...")
    try:
        r = session.get(f"{BASE_URL}/configs", timeout=5)
        print(f"GET /configs after save status: {r.status_code}, length={len(r.text)}")
    except Exception as e:
        print(f"GET /configs after save failed: {e}")

    print("\n6. Testing POST /api/config/test_mode...")
    try:
        r = session.post(f"{BASE_URL}/api/config/test_mode?enable=0", timeout=5)
        print(f"Test mode status: {r.status_code}, text={r.text}")
    except Exception as e:
        print(f"POST test_mode failed: {e}")

    print("\n7. Testing POST /api/config/serial...")
    try:
        r = session.post(f"{BASE_URL}/api/config/serial", json={"serial": 2}, timeout=5)
        print(f"Serial status: {r.status_code}, text={r.text}")
    except Exception as e:
        print(f"POST serial failed: {e}")

    print("\n8. Testing POST /api/config/relays...")
    try:
        r = session.post(f"{BASE_URL}/api/config/relays", json={"relays": [
            {"name": "Relay1", "type": 0, "pin": "PE2", "nc": 0},
            {"name": "Relay2", "type": 0, "pin": "PE4", "nc": 0}
        ]}, timeout=5)
        print(f"Relays status: {r.status_code}, text={r.text}")
    except Exception as e:
        print(f"POST relays failed: {e}")

    print("\n9. Testing GET /configs immediately after relays save...")
    try:
        r = session.get(f"{BASE_URL}/configs", timeout=5)
        print(f"GET /configs after relays save: {r.status_code}, length={len(r.text)}")
    except Exception as e:
        print(f"GET /configs after relays save failed: {e}")

if __name__ == "__main__":
    run_test()
