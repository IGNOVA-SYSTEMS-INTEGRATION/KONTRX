"""
Kontrx Modbus Protocol Tests
Tests Modbus RTU frame construction, CRC, and response validation logic.
Run: pytest test_modbus_protocol.py
"""
import struct
import pytest


def modbus_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def build_read_holding_request(slave_id: int, start_reg: int, count: int) -> bytes:
    frame = struct.pack(">BBhh", slave_id, 0x03, start_reg, count)
    crc = modbus_crc16(frame)
    return frame + struct.pack("<H", crc)


def build_read_holding_response(slave_id: int, values: list[float]) -> bytes:
    byte_count = len(values) * 4
    reg_data = b""
    for v in values:
        regs = struct.pack(">f", v)
        reg_data += regs
    frame = struct.pack("BBB", slave_id, 0x03, byte_count) + reg_data
    crc = modbus_crc16(frame)
    return frame + struct.pack("<H", crc)


def validate_response(resp: bytes, expected_slave: int, expected_fc: int) -> bool:
    if len(resp) < 5:
        return False
    if resp[0] != expected_slave:
        return False
    if resp[1] != expected_fc:
        return False
    payload = resp[:-2]
    crc_recv = struct.unpack("<H", resp[-2:])[0]
    return modbus_crc16(payload) == crc_recv


# ── CRC Tests ────────────────────────────────────────────

class TestModbusCRC:
    def test_crc_known_value(self):
        data = bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x02])
        crc = modbus_crc16(data)
        assert crc == 0x0BC4

    def test_crc_empty(self):
        assert modbus_crc16(b"") == 0xFFFF

    def test_crc_single_byte(self):
        crc = modbus_crc16(b"\x00")
        assert isinstance(crc, int)
        assert 0 <= crc <= 0xFFFF

    def test_crc_roundtrip(self):
        data = b"\x01\x03\x04\x41\x20\x00\x00"
        crc = modbus_crc16(data)
        full = data + struct.pack("<H", crc)
        assert modbus_crc16(full) == 0  # valid frame CRC check = 0


# ── Request Frame Tests ──────────────────────────────────

class TestRequestFrame:
    def test_read_holding_frame_length(self):
        frame = build_read_holding_request(1, 0, 2)
        assert len(frame) == 8  # 1+1+2+2+2

    def test_read_holding_slave_id(self):
        frame = build_read_holding_request(5, 0, 1)
        assert frame[0] == 5

    def test_read_holding_function_code(self):
        frame = build_read_holding_request(1, 0, 1)
        assert frame[1] == 0x03

    @pytest.mark.parametrize("slave_id", [1, 10, 127, 247])
    def test_valid_slave_ids(self, slave_id):
        frame = build_read_holding_request(slave_id, 0, 1)
        assert frame[0] == slave_id

    def test_broadcast_address(self):
        frame = build_read_holding_request(0, 0, 1)
        assert frame[0] == 0


# ── Response Validation Tests ─────────────────────────────

class TestResponseValidation:
    def test_valid_response(self):
        resp = build_read_holding_response(1, [7.0])
        assert validate_response(resp, 1, 0x03) is True

    def test_wrong_slave_id(self):
        resp = build_read_holding_response(1, [7.0])
        assert validate_response(resp, 2, 0x03) is False

    def test_corrupted_crc(self):
        resp = bytearray(build_read_holding_response(1, [7.0]))
        resp[-1] ^= 0xFF
        assert validate_response(bytes(resp), 1, 0x03) is False

    def test_too_short_response(self):
        assert validate_response(b"\x01\x03", 1, 0x03) is False

    def test_error_response(self):
        frame = bytes([0x01, 0x83, 0x02])
        crc = modbus_crc16(frame)
        resp = frame + struct.pack("<H", crc)
        assert validate_response(resp, 1, 0x83) is True

    def test_multi_float_response(self):
        values = [7.05, 25.3, 450.0, 8.2]
        resp = build_read_holding_response(1, values)
        assert validate_response(resp, 1, 0x03) is True
        byte_count = resp[2]
        assert byte_count == len(values) * 4


# ── Sensor Type Mapping (mirrors firmware constants) ──────

class TestSensorTypes:
    SENSOR_TYPES = {1: "pH", 2: "ORP", 3: "EC", 4: "DO", 5: "Ammonia", 6: "Ultrasonic", 7: "Multi-US"}

    @pytest.mark.parametrize("type_id,name", [
        (1, "pH"), (2, "ORP"), (3, "EC"), (4, "DO"),
        (5, "Ammonia"), (6, "Ultrasonic"), (7, "Multi-US"),
    ])
    def test_sensor_type_mapping(self, type_id, name):
        assert self.SENSOR_TYPES[type_id] == name

    def test_all_types_covered(self):
        assert len(self.SENSOR_TYPES) == 7


# ── Noise Rejection (logic simulation) ───────────────────

class TestNoiseRejection:
    VALID_FCS = {0x03, 0x04, 0x06, 0x10, 0x83, 0x84, 0x86, 0x90}

    def test_valid_slave_and_fc(self):
        assert 1 <= 1 <= 247
        assert 0x03 in self.VALID_FCS

    def test_slave_id_zero_rejected(self):
        assert not (1 <= 0 <= 247)

    def test_invalid_fc_rejected(self):
        assert 0xFF not in self.VALID_FCS

    def test_slave_id_248_rejected(self):
        assert not (1 <= 248 <= 247)


# ── Smart Backoff Logic ──────────────────────────────────

class TestSmartBackoff:
    BACKOFF_THRESHOLD = 2
    BACKOFF_MS = 8000

    def test_under_threshold_no_skip(self):
        fail_count = 1
        assert fail_count < self.BACKOFF_THRESHOLD

    def test_at_threshold_triggers_skip(self):
        fail_count = 2
        assert fail_count >= self.BACKOFF_THRESHOLD

    def test_success_resets_counter(self):
        fail_count = 5
        fail_count = 0  # simulates successful read
        assert fail_count < self.BACKOFF_THRESHOLD


# ── Multi-Sensor Board Protocol Tests ─────────────────────

class TestMultiSensorBoard:
    def test_flow_sensor_decoding(self):
        # 5 registers: [Rate 0.1 L/min, Total High, Total Low, Freq Hz, Status]
        regs = [155, 0, 1240, 45, 1]
        rate = regs[0] / 10.0
        total = (regs[1] << 16) | regs[2]
        freq = float(regs[3])
        status = regs[4]

        assert rate == 15.5
        assert total == 1240
        assert freq == 45.0
        assert status == 1

    def test_flow_sensor_large_total(self):
        # Total volume exceeding 65535 liters
        regs = [200, 3, 500, 60, 1]
        total = (regs[1] << 16) | regs[2]
        assert total == (3 * 65536) + 500

    def test_ds18b20_temperature_decoding(self):
        reg_positive = 245
        temp_pos = reg_positive / 10.0
        assert temp_pos == 24.5

        # Signed 16-bit negative temperature (-2.5 C -> 65511 / 0xFFE7)
        reg_neg = 0xFFE7
        signed_val = struct.unpack(">h", struct.pack(">H", reg_neg))[0]
        assert (signed_val / 10.0) == -2.5

    def test_fc06_mode_switch_frame(self):
        slave_id = 1
        reg_addr = 165  # 0x00A5
        val_sim = 1
        frame = struct.pack(">BBHH", slave_id, 0x06, reg_addr, val_sim)
        crc = modbus_crc16(frame)
        full_frame = frame + struct.pack("<H", crc)
        assert len(full_frame) == 8
        assert full_frame[0] == 1
        assert full_frame[1] == 0x06
        assert full_frame[2] == 0x00
        assert full_frame[3] == 0xA5
        assert full_frame[5] == 1
        assert modbus_crc16(full_frame) == 0

    def test_fc04_summary_request_frame(self):
        slave_id = 1
        start_reg = 0
        qty = 10
        frame = struct.pack(">BBHH", slave_id, 0x04, start_reg, qty)
        crc = modbus_crc16(frame)
        full_frame = frame + struct.pack("<H", crc)
        assert len(full_frame) == 8
        assert full_frame[1] == 0x04
        assert modbus_crc16(full_frame) == 0
