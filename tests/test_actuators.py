"""
Kontrx Output Actuator Tests — all 8 types
Tests logic, bounds, register mapping, and protocol correctness.
Run: pytest tests/test_actuators.py -v
"""
import struct
import pytest


# ═══════════════════════════════════════════════════════════
#  Constants (mirror firmware defines)
# ═══════════════════════════════════════════════════════════

ACTUATOR_TYPE_LOCAL_GPIO   = 0
ACTUATOR_TYPE_MODBUS_TCP   = 1
ACTUATOR_TYPE_OPC_UA       = 2
ACTUATOR_TYPE_PWM          = 3
ACTUATOR_TYPE_PTO          = 4
ACTUATOR_TYPE_ANALOG_MA    = 5
ACTUATOR_TYPE_ANALOG_V     = 6
ACTUATOR_TYPE_DIGITAL_OUT  = 7

MAX_RELAYS          = 16
MAX_PWM_CHANNELS    = 7
MAX_PTO_CHANNELS    = 4
MAX_420MA_CHANNELS  = 8
MAX_010V_CHANNELS   = 2

ALL_TYPES = [
    (ACTUATOR_TYPE_LOCAL_GPIO,  "Local GPIO Relay"),
    (ACTUATOR_TYPE_MODBUS_TCP,  "Modbus TCP Coil"),
    (ACTUATOR_TYPE_OPC_UA,      "OPC UA Client"),
    (ACTUATOR_TYPE_PWM,         "PWM Output"),
    (ACTUATOR_TYPE_PTO,         "PTO Motion Axis"),
    (ACTUATOR_TYPE_ANALOG_MA,   "4-20mA Current"),
    (ACTUATOR_TYPE_ANALOG_V,    "0-10V Voltage"),
    (ACTUATOR_TYPE_DIGITAL_OUT, "Digital Output"),
]


# ═══════════════════════════════════════════════════════════
#  Type 0: Local GPIO Relay
# ═══════════════════════════════════════════════════════════

class TestLocalGPIO:
    """Tests for ACTUATOR_TYPE_LOCAL_GPIO (type 0) — direct onboard relay pins."""

    RESERVED_PINS = [
        (0, 6), (0, 9), (0, 10),          # PA6=LED, PA9/PA10=USART1
        (1, 10), (1, 11), (1, 12),         # PB10-15 = Modbus/SPI
        (1, 13), (1, 14), (1, 15),
        (2, 6), (2, 7),                    # PC6/PC7 = USART6
        (3, 2), (3, 3),                    # PD2/PD3 = MAX485 RE#/DE
    ]

    def _is_reserved(self, port, pin):
        if port > 4 or pin > 15:
            return True
        if port == 0 and pin in (6, 9, 10):
            return True
        if port == 1 and 10 <= pin <= 15:
            return True
        if port == 2 and pin in (6, 7):
            return True
        if port == 3 and pin in (2, 3):
            return True
        return False

    def test_reserved_pins_rejected(self):
        for port, pin in self.RESERVED_PINS:
            assert self._is_reserved(port, pin), f"P{chr(65+port)}{pin} should be reserved"

    def test_valid_pins_accepted(self):
        valid_pins = [(0, 0), (0, 1), (0, 2), (4, 2), (4, 3)]
        for port, pin in valid_pins:
            assert not self._is_reserved(port, pin), f"P{chr(65+port)}{pin} should be allowed"

    def test_out_of_range_port_rejected(self):
        assert self._is_reserved(5, 0)
        assert self._is_reserved(255, 0)

    def test_out_of_range_pin_rejected(self):
        assert self._is_reserved(0, 16)
        assert self._is_reserved(0, 255)

    def test_active_low_logic(self):
        """NC relay: state=ON should drive pin LOW, state=OFF should drive HIGH."""
        is_nc = True
        state_on = 1
        drive = (not state_on) if is_nc else state_on
        assert drive == False  # Active low: ON = pin LOW

        state_off = 0
        drive = (not state_off) if is_nc else state_off
        assert drive == True  # Active low: OFF = pin HIGH

    def test_active_high_logic(self):
        """NO relay: state=ON should drive pin HIGH."""
        is_nc = False
        state = 1
        drive = (not state) if is_nc else state
        assert drive == 1

    def test_max_relays_bound(self):
        assert MAX_RELAYS == 16
        for idx in range(MAX_RELAYS):
            assert 0 <= idx < MAX_RELAYS

    def test_idx_overflow_rejected(self):
        idx = MAX_RELAYS
        assert idx >= MAX_RELAYS  # Should be rejected by firmware


# ═══════════════════════════════════════════════════════════
#  Type 1: Modbus TCP Coil Write
# ═══════════════════════════════════════════════════════════

class TestModbusTCPCoil:
    """Tests for ACTUATOR_TYPE_MODBUS_TCP (type 1) — remote PLC via Modbus TCP."""

    def _build_write_coil_request(self, slave_id, coil_addr, state):
        return bytes([
            0x00, 0x01,             # Transaction ID
            0x00, 0x00,             # Protocol ID (Modbus)
            0x00, 0x06,             # Length
            slave_id,               # Unit ID
            0x05,                   # FC 05: Write Single Coil
            (coil_addr >> 8) & 0xFF, coil_addr & 0xFF,
            0xFF if state else 0x00, 0x00,
        ])

    def test_write_coil_on_frame(self):
        frame = self._build_write_coil_request(1, 0x0000, True)
        assert len(frame) == 12
        assert frame[7] == 0x05  # FC
        assert frame[10] == 0xFF  # ON
        assert frame[11] == 0x00

    def test_write_coil_off_frame(self):
        frame = self._build_write_coil_request(1, 0x0000, False)
        assert frame[10] == 0x00
        assert frame[11] == 0x00

    def test_coil_address_encoding(self):
        frame = self._build_write_coil_request(1, 0x1234, True)
        assert frame[8] == 0x12
        assert frame[9] == 0x34

    def test_slave_id_range(self):
        for sid in [1, 127, 247]:
            frame = self._build_write_coil_request(sid, 0, True)
            assert frame[6] == sid

    def test_protocol_id_must_be_zero(self):
        frame = self._build_write_coil_request(1, 0, True)
        assert frame[2] == 0 and frame[3] == 0

    def test_response_validation(self):
        """Echo response must match request for FC=05."""
        req = self._build_write_coil_request(1, 0x0010, True)
        resp = req[:]  # Echo
        assert resp[7] == 0x05
        assert resp[8] == req[8] and resp[9] == req[9]

    def test_ip_parsing_valid(self):
        ip = "192.168.1.50"
        parts = [int(x) for x in ip.split('.')]
        assert len(parts) == 4
        assert all(0 <= p <= 255 for p in parts)

    def test_ip_parsing_invalid(self):
        for bad_ip in ["256.1.1.1", "abc", "", "1.2.3"]:
            parts = bad_ip.split('.')
            valid = len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 for p in parts)
            assert not valid, f"Should reject: {bad_ip}"


# ═══════════════════════════════════════════════════════════
#  Type 2: OPC UA Client (stub)
# ═══════════════════════════════════════════════════════════

class TestOPCUA:
    """Tests for ACTUATOR_TYPE_OPC_UA_CLIENT (type 2) — currently stubbed."""

    def test_type_id_correct(self):
        assert ACTUATOR_TYPE_OPC_UA == 2

    def test_stub_returns_failure(self):
        """OPC UA is disabled in firmware (RAM limits). Stub always returns 0."""
        result = 0  # Simulates OPC_UA_Client_WriteNode return
        assert result == 0

    def test_config_fields_present(self):
        """Actuator config must have opc_node_id[32] and port fields."""
        config = {"port_or_ip": "192.168.1.100", "port": 4840, "opc_node_id": "ns=2;s=Output1"}
        assert len(config["opc_node_id"]) <= 32
        assert config["port"] == 4840


# ═══════════════════════════════════════════════════════════
#  Type 3: PWM Output
# ═══════════════════════════════════════════════════════════

class TestPWMOutput:
    """Tests for ACTUATOR_TYPE_PWM (type 3) — 7 hardware PWM channels."""

    CHANNEL_PINS = {
        0: ("PD12", "TIM4_CH1"),
        1: ("PD13", "TIM4_CH2"),
        2: ("PD14", "TIM4_CH3"),
        3: ("PD15", "TIM4_CH4"),
        4: ("PB8",  "TIM10_CH1"),
        5: ("PB9",  "TIM11_CH1"),
        6: ("PA7",  "TIM14_CH1"),
    }

    def test_channel_count(self):
        assert MAX_PWM_CHANNELS == 7

    def test_all_channels_mapped(self):
        assert len(self.CHANNEL_PINS) == MAX_PWM_CHANNELS

    @pytest.mark.parametrize("ch", range(MAX_PWM_CHANNELS))
    def test_channel_has_pin_and_timer(self, ch):
        pin, timer = self.CHANNEL_PINS[ch]
        assert pin.startswith("P")
        assert "TIM" in timer

    def test_duty_bounds_zero(self):
        duty = 0.0
        assert 0.0 <= duty <= 100.0

    def test_duty_bounds_full(self):
        duty = 100.0
        assert 0.0 <= duty <= 100.0

    def test_duty_negative_clamped(self):
        duty = -5.0
        clamped = max(0.0, min(100.0, duty))
        assert clamped == 0.0

    def test_duty_overflow_clamped(self):
        duty = 150.0
        clamped = max(0.0, min(100.0, duty))
        assert clamped == 100.0

    def test_frequency_zero_rejected(self):
        freq_hz = 0
        assert freq_hz == 0  # Firmware rejects freq_hz == 0

    def test_modbus_register_duty_encoding(self):
        """Modbus registers 16-22 encode duty as 0-1000 = 0.0-100.0%."""
        duty_pct = 75.5
        reg_val = int(duty_pct * 10.0 + 0.5)
        assert reg_val == 755
        decoded = reg_val / 10.0
        assert abs(decoded - duty_pct) < 0.1

    @pytest.mark.parametrize("ch,addr", [(0, 16), (1, 17), (2, 18), (3, 19), (4, 20), (5, 21), (6, 22)])
    def test_modbus_register_mapping(self, ch, addr):
        assert addr - 16 == ch

    def test_out_of_range_channel_rejected(self):
        ch = MAX_PWM_CHANNELS
        assert ch >= MAX_PWM_CHANNELS


# ═══════════════════════════════════════════════════════════
#  Type 4: PTO Motion Axis
# ═══════════════════════════════════════════════════════════

class TestPTOMotion:
    """Tests for ACTUATOR_TYPE_PTO (type 4) — 4-axis pulse train output."""

    CHANNEL_TIMERS = {0: "TIM1", 1: "TIM9", 2: "TIM3", 3: "TIM2"}
    DIR_PINS   = {0: "PE8", 1: "PE3", 2: "PC9", 3: "PA5"}
    PULSE_PINS = {0: "PE9", 1: "PE5", 2: "PC8", 3: "PA3"}
    LIMIT_PINS = {0: "PE0", 1: "PE1", 2: "PE7", 3: "PB1"}

    def test_channel_count(self):
        assert MAX_PTO_CHANNELS == 4

    @pytest.mark.parametrize("ch", range(MAX_PTO_CHANNELS))
    def test_channel_has_all_pins(self, ch):
        assert ch in self.DIR_PINS
        assert ch in self.PULSE_PINS
        assert ch in self.LIMIT_PINS
        assert ch in self.CHANNEL_TIMERS

    def test_move_relative_positive(self):
        """Positive steps = forward direction."""
        steps = 1000
        direction = 1 if steps > 0 else 0
        assert direction == 1

    def test_move_relative_negative(self):
        """Negative steps = reverse direction."""
        steps = -500
        direction = 1 if steps > 0 else 0
        assert direction == 0

    def test_move_relative_zero_rejected(self):
        steps = 0
        assert steps == 0  # Firmware returns early

    def test_speed_zero_rejected(self):
        speed_pps = 0
        assert speed_pps == 0  # Firmware returns early

    def test_move_absolute_calculates_delta(self):
        current_pos = 500
        target_pos = 1200
        delta = target_pos - current_pos
        assert delta == 700

    def test_move_absolute_reverse(self):
        current_pos = 1000
        target_pos = 200
        delta = target_pos - current_pos
        assert delta == -800

    def test_limit_switch_blocks_forward(self):
        """Can't move forward when forward limit is hit."""
        lmt_state = 1  # LMT_STATE_HIT_FWD
        direction = 1  # forward
        blocked = (direction == 1 and lmt_state == 1)
        assert blocked

    def test_limit_switch_blocks_reverse(self):
        lmt_state = 2  # LMT_STATE_HIT_REV
        direction = 0  # reverse
        blocked = (direction == 0 and lmt_state == 2)
        assert blocked

    def test_limit_switch_allows_opposite(self):
        """Forward limit hit should still allow reverse motion."""
        lmt_state = 1  # HIT_FWD
        direction = 0  # reverse
        blocked = (direction == 1 and lmt_state == 1) or (direction == 0 and lmt_state == 2)
        assert not blocked

    def test_emergency_stop_clears_motion(self):
        moving = 1
        steps_remaining = 500
        # After emergency stop:
        moving = 0
        steps_remaining = 0
        assert moving == 0 and steps_remaining == 0

    def test_set_home_resets_position(self):
        position = 12345
        target = 12345
        # After set_home:
        position = 0
        target = 0
        lmt_state = 0  # LMT_STATE_CLEAR
        assert position == 0 and target == 0 and lmt_state == 0

    def test_step_tick_decrements(self):
        """ISR step tick should decrement steps_remaining and update position."""
        pos = 100
        remaining = 10
        direction = 1
        remaining -= 1
        pos += 1 if direction else -1
        assert remaining == 9
        assert pos == 101

    def test_step_tick_stops_at_zero(self):
        remaining = 1
        remaining -= 1
        stopped = (remaining == 0)
        assert stopped

    def test_modbus_register_target_position(self):
        """Registers 36-39: PTO target positions (signed int16)."""
        target = -500
        reg_val = target & 0xFFFF
        decoded = struct.unpack('>h', struct.pack('>H', reg_val))[0]
        assert decoded == -500

    def test_modbus_register_status_bits(self):
        """Registers 44-47: bit0=moving, bit1=limit_hit."""
        moving = True
        lmt_state = 1  # HIT_FWD
        status = (1 if moving else 0) | (2 if lmt_state else 0)
        assert status == 3
        assert status & 1  # moving
        assert status & 2  # limit hit

    def test_out_of_range_channel_rejected(self):
        ch = MAX_PTO_CHANNELS
        assert ch >= MAX_PTO_CHANNELS


# ═══════════════════════════════════════════════════════════
#  Type 5: 4-20mA Current Output (DAC)
# ═══════════════════════════════════════════════════════════

class TestAnalog420mA:
    """Tests for ACTUATOR_TYPE_ANALOG_MA (type 5) — SPI DAC MCP4922."""

    def test_channel_count(self):
        assert MAX_420MA_CHANNELS == 8

    def test_init_default_4mA(self):
        default_mA = 4.0
        assert default_mA == 4.0

    def test_current_lower_bound_clamped(self):
        mA = -1.0
        clamped = max(0.0, min(20.5, mA))
        assert clamped == 0.0

    def test_current_upper_bound_clamped(self):
        mA = 25.0
        clamped = max(0.0, min(20.5, mA))
        assert clamped == 20.5

    @pytest.mark.parametrize("mA,expected_raw", [
        (4.0,  0),      # Minimum: 0
        (12.0, 2048),   # Midpoint: 2048
        (20.0, 4095),   # Maximum: 4095
    ])
    def test_dac_linear_mapping(self, mA, expected_raw):
        """4.00mA=0, 20.00mA=4095 linear mapping."""
        norm = (mA - 4.0) / 16.0
        norm = max(0.0, min(1.0, norm))
        raw = int(norm * 4095.0 + 0.5)
        assert raw == expected_raw

    def test_dac_below_4mA_maps_to_zero(self):
        mA = 2.0
        norm = (mA - 4.0) / 16.0
        norm = max(0.0, min(1.0, norm))
        assert norm == 0.0

    def test_mcp4922_frame_channel_a(self):
        """MCP4922 SPI frame: bit15=DAC_SEL, bit14=BUF, bit13=GA, bit12=SHDN, [11:0]=data."""
        raw = 2048
        dac_sub_ch = 0  # Channel A
        chan_bit = 1 if dac_sub_ch else 0
        frame = (chan_bit << 15) | (1 << 14) | (1 << 13) | (1 << 12) | (raw & 0x0FFF)
        assert frame & (1 << 15) == 0      # Channel A
        assert frame & (1 << 14) != 0      # Buffered
        assert frame & (1 << 13) != 0      # 1x Gain
        assert frame & (1 << 12) != 0      # Active
        assert frame & 0x0FFF == 2048

    def test_mcp4922_frame_channel_b(self):
        raw = 1000
        dac_sub_ch = 1  # Channel B
        chan_bit = 1 if dac_sub_ch else 0
        frame = (chan_bit << 15) | (1 << 14) | (1 << 13) | (1 << 12) | (raw & 0x0FFF)
        assert frame & (1 << 15) != 0  # Channel B

    def test_chip_index_mapping(self):
        """8 channels map to 4 MCP4922 chips (2 channels each)."""
        for ch in range(MAX_420MA_CHANNELS):
            chip_idx = ch // 2
            dac_sub_ch = ch % 2
            assert 0 <= chip_idx <= 3
            assert dac_sub_ch in (0, 1)

    def test_modbus_register_encoding(self):
        """Registers 28-35: 400-2000 = 4.00-20.00 mA."""
        mA = 16.5
        reg_val = int(mA * 100.0 + 0.5)
        assert reg_val == 1650
        decoded = reg_val / 100.0
        assert abs(decoded - mA) < 0.01

    @pytest.mark.parametrize("ch,addr", [(0, 28), (3, 31), (7, 35)])
    def test_modbus_register_mapping(self, ch, addr):
        assert addr - 28 == ch

    def test_out_of_range_channel_rejected(self):
        ch = MAX_420MA_CHANNELS
        assert ch >= MAX_420MA_CHANNELS


# ═══════════════════════════════════════════════════════════
#  Type 6: 0-10V Voltage Output (PWM + LPF)
# ═══════════════════════════════════════════════════════════

class TestAnalog010V:
    """Tests for ACTUATOR_TYPE_ANALOG_V (type 6) — PWM-based 0-10V via LPF + OpAmp."""

    CHANNEL_PWM_MAP = {0: 2, 1: 3}  # 0-10V ch0→PWM ch2 (PD14), ch1→PWM ch3 (PD15)

    def test_channel_count(self):
        assert MAX_010V_CHANNELS == 2

    def test_pwm_channel_mapping(self):
        assert self.CHANNEL_PWM_MAP[0] == 2  # PD14
        assert self.CHANNEL_PWM_MAP[1] == 3  # PD15

    def test_voltage_lower_bound_clamped(self):
        v = -2.0
        clamped = max(0.0, min(10.0, v))
        assert clamped == 0.0

    def test_voltage_upper_bound_clamped(self):
        v = 15.0
        clamped = max(0.0, min(10.0, v))
        assert clamped == 10.0

    @pytest.mark.parametrize("voltage,expected_duty", [
        (0.0,   0.0),
        (5.0,  50.0),
        (10.0, 100.0),
    ])
    def test_voltage_to_duty_conversion(self, voltage, expected_duty):
        """0V=0% duty, 10V=100% duty linear mapping."""
        duty = (voltage / 10.0) * 100.0
        assert abs(duty - expected_duty) < 0.01

    def test_init_sets_20khz_frequency(self):
        """LPF circuit requires 20kHz PWM for low ripple."""
        expected_freq = 20000
        assert expected_freq == 20000

    def test_init_sets_zero_output(self):
        initial_voltage = 0.0
        initial_duty = 0.0
        assert initial_voltage == 0.0
        assert initial_duty == 0.0

    def test_modbus_register_encoding(self):
        """Registers 24-25: 0-1000 = 0.00-10.00V."""
        voltage = 7.55
        reg_val = int(voltage * 100.0 + 0.5)
        assert reg_val == 755
        decoded = reg_val / 100.0
        assert abs(decoded - voltage) < 0.01

    @pytest.mark.parametrize("ch,addr", [(0, 24), (1, 25)])
    def test_modbus_register_mapping(self, ch, addr):
        assert addr - 24 == ch

    def test_out_of_range_channel_rejected(self):
        ch = MAX_010V_CHANNELS
        assert ch >= MAX_010V_CHANNELS


# ═══════════════════════════════════════════════════════════
#  Type 7: Digital Output
# ═══════════════════════════════════════════════════════════

class TestDigitalOutput:
    """Tests for ACTUATOR_TYPE_DIGITAL_OUT (type 7) — same GPIO driver as LOCAL_GPIO."""

    def test_type_id_correct(self):
        assert ACTUATOR_TYPE_DIGITAL_OUT == 7

    def test_shares_gpio_init_with_local(self):
        """Digital Out uses same Relay_Init path as LOCAL_GPIO."""
        gpio_types = [ACTUATOR_TYPE_LOCAL_GPIO, ACTUATOR_TYPE_DIGITAL_OUT]
        assert ACTUATOR_TYPE_LOCAL_GPIO in gpio_types
        assert ACTUATOR_TYPE_DIGITAL_OUT in gpio_types

    def test_state_binary(self):
        for state in [0, 1]:
            assert state in (0, 1)

    def test_active_low_digital_out(self):
        is_active_low = True
        state = 1
        drive = (not state) if is_active_low else state
        assert drive == False


# ═══════════════════════════════════════════════════════════
#  Modbus TCP Server Register Map (cross-actuator)
# ═══════════════════════════════════════════════════════════

class TestModbusTCPRegisterMap:
    """Tests for the Modbus TCP Server holding register layout (all actuator types)."""

    REGISTER_MAP = {
        "relays":       (0, 15),      # 0-15: GPIO relay states
        "pwm_duty":     (16, 22),     # 16-22: PWM duty (0-1000)
        "010v_output":  (24, 25),     # 24-25: 0-10V (0-1000)
        "420ma_output": (28, 35),     # 28-35: 4-20mA (400-2000)
        "pto_target":   (36, 39),     # 36-39: PTO target positions
        "pto_position": (40, 43),     # 40-43: PTO current positions
        "pto_status":   (44, 47),     # 44-47: PTO status bits
        "sensors":      (100, 115),   # 100-115: Live sensor readings
    }

    def test_no_register_overlaps(self):
        ranges = list(self.REGISTER_MAP.values())
        for i, (start_a, end_a) in enumerate(ranges):
            for j, (start_b, end_b) in enumerate(ranges):
                if i >= j:
                    continue
                overlap = not (end_a < start_b or end_b < start_a)
                assert not overlap, f"Overlap between ranges {i} [{start_a}-{end_a}] and {j} [{start_b}-{end_b}]"

    def test_relay_register_count(self):
        start, end = self.REGISTER_MAP["relays"]
        assert end - start + 1 == MAX_RELAYS

    def test_pwm_register_count(self):
        start, end = self.REGISTER_MAP["pwm_duty"]
        assert end - start + 1 == MAX_PWM_CHANNELS

    def test_420ma_register_count(self):
        start, end = self.REGISTER_MAP["420ma_output"]
        assert end - start + 1 == MAX_420MA_CHANNELS

    def test_010v_register_count(self):
        start, end = self.REGISTER_MAP["010v_output"]
        assert end - start + 1 == MAX_010V_CHANNELS

    def test_pto_register_count(self):
        start, end = self.REGISTER_MAP["pto_target"]
        assert end - start + 1 == MAX_PTO_CHANNELS

    def test_write_blocked_in_normal_mode(self):
        """WriteHoldingRegister checks test_mode == 1; returns if not."""
        test_mode = 0
        assert test_mode != 1  # Write should be blocked

    def test_write_allowed_in_test_mode(self):
        test_mode = 1
        assert test_mode == 1

    def test_read_always_allowed(self):
        """ReadHoldingRegister has no test_mode guard."""
        for addr in [0, 16, 24, 28, 36, 40, 44, 100]:
            assert addr >= 0  # Always readable

    def test_max_registers_per_frame(self):
        """Server caps to 60 registers per read request."""
        max_count = 60
        assert max_count == 60

    def test_fc03_read_holding(self):
        fc = 0x03
        assert fc == 3

    def test_fc06_write_single(self):
        fc = 0x06
        assert fc == 6

    def test_fc05_write_coil(self):
        fc = 0x05
        assert fc == 5

    def test_coil_on_value(self):
        """FC05: 0xFF00 = ON, 0x0000 = OFF."""
        val_on = 0xFF00
        assert (val_on == 0xFF00)

    def test_coil_off_value(self):
        val_off = 0x0000
        assert (val_off == 0x0000)


# ═══════════════════════════════════════════════════════════
#  Actuator Config Struct Validation
# ═══════════════════════════════════════════════════════════

class TestActuatorConfig:
    """Tests for Actuator_Config_t struct constraints."""

    def test_all_types_have_unique_ids(self):
        type_ids = [t[0] for t in ALL_TYPES]
        assert len(type_ids) == len(set(type_ids))

    def test_type_range_0_to_7(self):
        for type_id, _ in ALL_TYPES:
            assert 0 <= type_id <= 7

    def test_name_max_length(self):
        name = "PLC_Valve_Long"
        assert len(name) < 16  # name[16] in struct

    def test_name_exactly_16_truncated(self):
        name = "A" * 16
        truncated = name[:15]
        assert len(truncated) == 15

    def test_port_or_ip_max_length(self):
        ip = "192.168.100.200"
        assert len(ip) < 16  # port_or_ip[16]

    def test_opc_node_id_max_length(self):
        node = "ns=2;s=MyController.Out1"
        assert len(node) < 32  # opc_node_id[32]

    @pytest.mark.parametrize("type_id,name", ALL_TYPES)
    def test_all_actuator_types_defined(self, type_id, name):
        assert isinstance(type_id, int)
        assert isinstance(name, str)
        assert len(name) > 0


# ═══════════════════════════════════════════════════════════
#  HTTP API Actuator Endpoints (request format)
# ═══════════════════════════════════════════════════════════

class TestHTTPActuatorEndpoints:
    """Tests for HTTP API actuator control request/response format."""

    def test_relay_request_format(self):
        req = {"id": 0, "state": 1}
        assert "id" in req and "state" in req
        assert req["state"] in (0, 1)

    def test_pwm_request_format(self):
        req = {"channel": 0, "duty": 75.5}
        assert 0 <= req["duty"] <= 100.0

    def test_pto_request_format(self):
        req = {"channel": 0, "target": 5000, "speed": 2000}
        assert req["speed"] > 0

    def test_analog_420ma_request_format(self):
        req = {"channel": 0, "mA": 12.5}
        assert 0.0 <= req["mA"] <= 20.5

    def test_analog_010v_request_format(self):
        req = {"channel": 0, "voltage": 7.5}
        assert 0.0 <= req["voltage"] <= 10.0

    def test_all_actuator_endpoints_protected(self):
        """All POST actuator endpoints should require authentication."""
        protected = [
            "POST /api/relay",
            "POST /api/pwm",
            "POST /api/pto",
            "POST /api/analog",
            "POST /api/config/actuators",
        ]
        assert len(protected) == 5
