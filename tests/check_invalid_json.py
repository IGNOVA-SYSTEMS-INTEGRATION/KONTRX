"""Confirms every input the new rules parser rejected (where cJSON accepted it)
is invalid JSON under a strict parser. Usage: python check_invalid_json.py strict_check.txt"""
import json
import sys


def strict_loads(text):
    def no_const(c):
        raise ValueError('non-standard constant ' + c)
    dec = json.JSONDecoder(parse_constant=no_const, strict=True)
    obj, end = dec.raw_decode(text.lstrip(' \t\r\n'))   # text after the first value is allowed
    return obj


bad = 0
lines = open(sys.argv[1], encoding='utf-8', errors='surrogateescape').read().split('\n')
lines = [l for l in lines if l]
for line in lines:
    t = line[1:] if line.startswith('\ufeff') else line
    try:
        strict_loads(t)
        bad += 1
        if bad <= 5:
            print('VALID JSON but rejected:', t[:160])
    except (ValueError, RecursionError):
        pass
print(f'{len(lines)} rejected inputs checked, {bad} were valid JSON')
sys.exit(1 if bad else 0)
