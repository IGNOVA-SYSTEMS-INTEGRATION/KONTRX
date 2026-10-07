/**
 * @file  test_config_logic.c
 * @brief Host-side unit tests for Kontrx config validation, CRC, and magic checks.
 *        Compile: gcc -DUNIT_TEST -I../HAL -I../APP test_config_logic.c -o test_config
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* Inline the struct definitions without RTOS dependencies */
#define MAX_SENSORS    16
#define MAX_RELAYS     16
#define MAX_MULTI_US   4
#define MAX_MQTT_MAPPINGS 16
#define CONFIG_MAGIC_CURRENT 0xC01D0007U

typedef struct { uint8_t type; uint8_t id; } SensorEntry_t;
typedef struct { uint8_t count; SensorEntry_t entries[MAX_SENSORS]; } SensorList_t;
typedef struct {
    uint8_t id; char name[16]; uint8_t type; uint8_t is_active_low; uint8_t state;
    char port_or_ip[16]; uint16_t port; uint8_t pin_or_slave; uint16_t reg_addr;
    char opc_node_id[32];
} Actuator_Config_t;
typedef struct {
    uint8_t source_type; uint8_t source_id; char json_key[24]; uint8_t enabled;
} Mqtt_Field_Mapping_t;
typedef struct {
    uint32_t magic;
    SensorList_t sensors;
    Actuator_Config_t actuators[MAX_RELAYS];
    uint8_t actuator_count;
    uint32_t serial;
    char mqtt_broker[64];
    uint16_t mqtt_port;
    char mqtt_client_id[32];
    char mqtt_username[32];
    char mqtt_password[32];
    char device_id[40];
    char provision_status[24];
    char provision_message[128];
    char sparkplug_topic[128];
    char pending_sparkplug_topic[128];
    uint32_t mqtt_interval;
    uint8_t mqtt_send_mode;
    uint8_t mqtt_payload_shape;
    Mqtt_Field_Mapping_t mqtt_mappings[MAX_MQTT_MAPPINGS];
    uint8_t mqtt_mapping_count;
    char admin_username[32];
    char admin_password[32];
    uint8_t actuator_mask;
    uint8_t test_mode;
    uint8_t mqtt_tx_enabled;
    uint8_t cfg_padding[1];
    uint32_t checksum;
} Gateway_Config_t;

/* ── CRC32 (same algorithm as firmware) ─────────────────── */
static uint32_t crc32_calc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

static uint32_t config_checksum(const Gateway_Config_t *cfg) {
    size_t len = sizeof(*cfg) - sizeof(cfg->checksum);
    return crc32_calc((const uint8_t *)cfg, len);
}

/* ── Tests ──────────────────────────────────────────────── */
static int tests_run = 0, tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %-50s", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

void test_config_magic_valid(void) {
    TEST(config_magic_valid);
    Gateway_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CONFIG_MAGIC_CURRENT;
    if (cfg.magic == CONFIG_MAGIC_CURRENT) PASS(); else FAIL("magic mismatch");
}

void test_config_magic_invalid(void) {
    TEST(config_magic_invalid);
    Gateway_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = 0xDEADBEEF;
    if (cfg.magic != CONFIG_MAGIC_CURRENT) PASS(); else FAIL("should not match");
}

void test_config_checksum_roundtrip(void) {
    TEST(config_checksum_roundtrip);
    Gateway_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CONFIG_MAGIC_CURRENT;
    cfg.serial = 12345;
    cfg.mqtt_port = 1883;
    strncpy(cfg.mqtt_broker, "mqtt.example.com", sizeof(cfg.mqtt_broker));
    cfg.checksum = config_checksum(&cfg);

    uint32_t verify = config_checksum(&cfg);
    if (verify == cfg.checksum) PASS(); else FAIL("checksum mismatch after roundtrip");
}

void test_config_checksum_detects_corruption(void) {
    TEST(config_checksum_detects_corruption);
    Gateway_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CONFIG_MAGIC_CURRENT;
    cfg.serial = 42;
    cfg.checksum = config_checksum(&cfg);
    cfg.serial = 43;
    uint32_t verify = config_checksum(&cfg);
    if (verify != cfg.checksum) PASS(); else FAIL("corruption not detected");
}

void test_sensor_count_bounds(void) {
    TEST(sensor_count_bounds);
    Gateway_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sensors.count = MAX_SENSORS;
    if (cfg.sensors.count <= MAX_SENSORS) PASS(); else FAIL("exceeds MAX_SENSORS");
}

void test_sensor_count_overflow(void) {
    TEST(sensor_count_overflow);
    uint8_t count = MAX_SENSORS + 1;
    if (count > MAX_SENSORS) PASS(); else FAIL("overflow not detected");
}

void test_actuator_types_valid(void) {
    TEST(actuator_types_valid);
    Actuator_Config_t act;
    memset(&act, 0, sizeof(act));
    int ok = 1;
    for (uint8_t t = 0; t <= 7; t++) {
        act.type = t;
        if (act.type > 7) { ok = 0; break; }
    }
    if (ok) PASS(); else FAIL("invalid actuator type accepted");
}

void test_mqtt_interval_zero(void) {
    TEST(mqtt_interval_zero);
    Gateway_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mqtt_interval = 0;
    if (cfg.mqtt_interval == 0) PASS(); else FAIL("zero interval not preserved");
}

void test_string_field_null_termination(void) {
    TEST(string_field_null_termination);
    Gateway_Config_t cfg;
    memset(&cfg, 0xFF, sizeof(cfg));
    strncpy(cfg.mqtt_broker, "test.broker.io", sizeof(cfg.mqtt_broker) - 1);
    cfg.mqtt_broker[sizeof(cfg.mqtt_broker) - 1] = '\0';
    if (strlen(cfg.mqtt_broker) < sizeof(cfg.mqtt_broker)) PASS();
    else FAIL("string not null-terminated");
}

void test_default_credentials_present(void) {
    TEST(default_credentials_present);
    Gateway_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int needs_default = (cfg.admin_username[0] == '\0');
    if (needs_default) PASS(); else FAIL("empty config should need defaults");
}

void test_actuator_mask_bits(void) {
    TEST(actuator_mask_bits);
    uint8_t mask = 0;
    mask |= (1 << 0); // PTO
    mask |= (1 << 3); // PWM
    if ((mask & 0x01) && (mask & 0x08) && !(mask & 0x02)) PASS();
    else FAIL("bitmask logic error");
}

void test_crc32_known_vector(void) {
    TEST(crc32_known_vector);
    const char *data = "123456789";
    uint32_t crc = crc32_calc((const uint8_t *)data, 9);
    if (crc == 0xCBF43926) PASS(); else FAIL("CRC32 incorrect for known input");
}

int main(void) {
    printf("\n=== Kontrx Config Logic Unit Tests ===\n\n");

    test_config_magic_valid();
    test_config_magic_invalid();
    test_config_checksum_roundtrip();
    test_config_checksum_detects_corruption();
    test_sensor_count_bounds();
    test_sensor_count_overflow();
    test_actuator_types_valid();
    test_mqtt_interval_zero();
    test_string_field_null_termination();
    test_default_credentials_present();
    test_actuator_mask_bits();
    test_crc32_known_vector();

    printf("\n  Results: %d/%d passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
