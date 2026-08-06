"""
TDD Test: DO Sensor Float Decoding

The KWS-630 DO sensor uses DCBA (little-endian) float format.
The production code (modbus_dma.c) uses BADC format which is WRONG.

Register layout (from main_do_test.c):
  0x2600-0x2601: Temperature (DCBA)
  0x2604-0x2605: DO value (DCBA)

Production code reads:
  0x2600-0x2601 as DO value (BADC)  <- WRONG: should be temp, wrong byte order
  0x2602-0x2603 as DO temp (BADC)   <- WRONG: register doesn't exist
"""
import struct

def decode_float_badc(r0, r1):
    """BADC byte order (currently used in modbus_dma.c - WRONG)"""
    b = bytearray(4)
    b[0] = r1 & 0xFF;        b[1] = (r1 >> 8) & 0xFF
    b[2] = r0 & 0xFF;        b[3] = (r0 >> 8) & 0xFF
    return struct.unpack('<f', bytes(b))[0]

def decode_float_dcba(r0, r1):
    """DCBA byte order (correct for KWS-630, from main_do_test.c)"""
    b = bytearray(4)
    b[0] = (r0 >> 8) & 0xFF  # D
    b[1] = r0 & 0xFF         # C
    b[2] = (r1 >> 8) & 0xFF  # B
    b[3] = r1 & 0xFF         # A
    return struct.unpack('<f', bytes(b))[0]

def registers_from_float_dcba(value):
    """Convert a float to two registers in DCBA format"""
    b = struct.pack('<f', value)
    r0 = (b[0] << 8) | b[1]  # D(high) C(low)
    r1 = (b[2] << 8) | b[3]  # B(high) A(low)
    return r0, r1


failures = 0

def check(condition, msg):
    global failures
    if not condition:
        print(f"  FAIL: {msg}")
        failures += 1
    else:
        print(f"  PASS: {msg}")

print("=" * 60)
print("TDD: DO Sensor Float Decoding Tests")
print("=" * 60)

# ---- TEST 1: DCBA round-trip ----
print("\n--- Test 1: DCBA round-trip ---")
known_temp = 25.5
r0, r1 = registers_from_float_dcba(known_temp)
decoded = decode_float_dcba(r0, r1)
check(abs(decoded - known_temp) < 0.001,
      f"DCBA({r0:#x}, {r1:#x}) = {decoded} (expected {known_temp})")

# ---- TEST 2: BADC gives WRONG value ----
print("\n--- Test 2: BADC gives wrong value (this MUST prove BADC is wrong) ---")
wrong = decode_float_badc(r0, r1)
check(abs(wrong - known_temp) >= 0.1,
      f"BADC should NOT decode {known_temp}: got {wrong}")
print(f"  => CONFIRMED: BADC({r0:#x}, {r1:#x}) = {wrong} (wrong!)")

# ---- TEST 3: Another known value ----
print("\n--- Test 3: DO value round-trip ---")
known_do = 8.25
r0, r1 = registers_from_float_dcba(known_do)
decoded = decode_float_dcba(r0, r1)
check(abs(decoded - known_do) < 0.001,
      f"DCBA({r0:#x}, {r1:#x}) = {decoded} (expected {known_do})")

wrong = decode_float_badc(r0, r1)
check(abs(wrong - known_do) >= 0.1,
      f"BADC should NOT decode {known_do}: got {wrong}")
print(f"  => CONFIRMED: BADC({r0:#x}, {r1:#x}) = {wrong} (wrong!)")

# ---- TEST 4: Simulated real sensor values ----
print("\n--- Test 4: Simulated sensor read at 0x2600 (DO=6.75, Temp=28.3) ---")
temp_r0, temp_r1 = registers_from_float_dcba(28.3)    # Registers 0x2600-0x2601
do_r0, do_r1     = registers_from_float_dcba(6.75)     # Registers 0x2604-0x2605

# DCBA decoding (CORRECT - what the test file does)
temp_dcba = decode_float_dcba(temp_r0, temp_r1)
do_dcba   = decode_float_dcba(do_r0, do_r1)
check(abs(temp_dcba - 28.3) < 0.001,
      f"DCBA temp failed: {temp_dcba} != 28.3")
check(abs(do_dcba - 6.75) < 0.001,
      f"DCBA DO failed: {do_dcba} != 6.75")
print(f"  CORRECT (DCBA): Temp={temp_dcba}C, DO={do_dcba} mg/L")

# BADC decoding (WRONG - what production modbus_dma.c does)
temp_badc = decode_float_badc(temp_r0, temp_r1)
do_badc   = decode_float_badc(do_r0, do_r1)
print(f"  WRONG (BADC): Temp={temp_badc}C, DO={do_badc} mg/L (garbage!)")
check(abs(temp_badc - 28.3) >= 0.1,
      f"BADC should be wrong for temp, got {temp_badc}")
check(abs(do_badc - 6.75) >= 0.1,
      f"BADC should be wrong for DO, got {do_badc}")

# ---- TEST 5: Edge cases ----
print("\n--- Test 5: Edge cases ---")
# Zero
r0, r1 = registers_from_float_dcba(0.0)
check(decode_float_dcba(r0, r1) == 0.0, f"DCBA zero: {decode_float_dcba(r0, r1)}")
# Negative value
r0, r1 = registers_from_float_dcba(-5.0)
check(abs(decode_float_dcba(r0, r1) - (-5.0)) < 0.001,
      f"DCBA negative: {decode_float_dcba(r0, r1)} != -5.0")

print("\n" + "=" * 60)
if failures:
    print(f"FAILED: {failures} test(s) failed!")
    exit(1)
else:
    print("ALL TESTS PASSED - Bug confirmed: BADC is wrong, DCBA is correct")
    print("=" * 60)
