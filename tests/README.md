# Kontrx Test Suite

## Host-side Unit Tests (no device needed)

### Config Logic (C)
```bash
gcc -o test_config tests/test_config_logic.c && ./test_config
```

### Modbus Protocol (Python)
```bash
pip install pytest
pytest tests/test_modbus_protocol.py -v
```

## Integration Tests (requires live device)

### HTTP API
```bash
pytest tests/test_http_api.py --device=192.168.1.100 -v
```

## Web Assets Build Pipeline

### Extract HTML from firmware header
```bash
python tools/build_web_assets.py extract --input APP/web_assets.h --output web/index.html
```

### Rebuild header from edited HTML
```bash
python tools/build_web_assets.py build --input web/index.html --output APP/web_assets.h
```
