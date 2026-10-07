import urllib.request
import json

req = urllib.request.Request("http://192.168.1.200/api/sdcard/read?path=system_event.log&offset=0&limit=50")
with urllib.request.urlopen(req, timeout=5) as resp:
    data = json.loads(resp.read().decode())
    for log in data.get('logs', []):
        if log.get('cat') == 'OTA':
            print(log)
