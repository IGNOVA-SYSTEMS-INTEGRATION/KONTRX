#include "sparkplug_b_enc.h"
#include <stdio.h>
#include "pwm_controller.h"
#include "dac_420ma.h"
#include "analog_010v.h"
#include "pto_motion.h"

static inline int write_varint(uint8_t **p, uint8_t *end, uint64_t val) {
    while (val >= 0x80) {
        if (*p >= end) return 0;
        **p = (uint8_t)((val & 0x7F) | 0x80);
        (*p)++;
        val >>= 7;
    }
    if (*p >= end) return 0;
    **p = (uint8_t)(val & 0x7F);
    (*p)++;
    return 1;
}

static inline int write_key(uint8_t **p, uint8_t *end, uint32_t field_num, uint8_t wire_type) {
    return write_varint(p, end, (field_num << 3) | wire_type);
}

static inline int write_float(uint8_t **p, uint8_t *end, float val) {
    if (*p + 4 > end) return 0;
    memcpy(*p, &val, 4);
    *p += 4;
    return 1;
}

static inline int write_string(uint8_t **p, uint8_t *end, uint32_t field_num, const char *str) {
    size_t len = strlen(str);
    if (!write_key(p, end, field_num, 2)) return 0;
    if (!write_varint(p, end, len)) return 0;
    if (*p + len > end) return 0;
    memcpy(*p, str, len);
    *p += len;
    return 1;
}

static int write_metric_float(uint8_t **p, uint8_t *end, const char *name, uint64_t ts_ms, float val) {
    uint8_t temp[128];
    uint8_t *tp = temp;
    uint8_t *tend = temp + sizeof(temp);

    if (!write_string(&tp, tend, 1, name)) return 0;
    if (!write_key(&tp, tend, 3, 0) || !write_varint(&tp, tend, ts_ms)) return 0;
    if (!write_key(&tp, tend, 4, 0) || !write_varint(&tp, tend, SPB_DATA_TYPE_FLOAT)) return 0;
    if (!write_key(&tp, tend, 12, 5) || !write_float(&tp, tend, val)) return 0;

    size_t metric_len = tp - temp;
    if (!write_key(p, end, 2, 2)) return 0;
    if (!write_varint(p, end, metric_len)) return 0;
    if (*p + metric_len > end) return 0;
    memcpy(*p, temp, metric_len);
    *p += metric_len;
    return 1;
}

static int write_metric_bool(uint8_t **p, uint8_t *end, const char *name, uint64_t ts_ms, uint8_t val) {
    uint8_t temp[128];
    uint8_t *tp = temp;
    uint8_t *tend = temp + sizeof(temp);

    if (!write_string(&tp, tend, 1, name)) return 0;
    if (!write_key(&tp, tend, 3, 0) || !write_varint(&tp, tend, ts_ms)) return 0;
    if (!write_key(&tp, tend, 4, 0) || !write_varint(&tp, tend, SPB_DATA_TYPE_BOOLEAN)) return 0;
    if (!write_key(&tp, tend, 14, 0) || !write_varint(&tp, tend, val ? 1 : 0)) return 0;

    size_t metric_len = tp - temp;
    if (!write_key(p, end, 2, 2)) return 0;
    if (!write_varint(p, end, metric_len)) return 0;
    if (*p + metric_len > end) return 0;
    memcpy(*p, temp, metric_len);
    *p += metric_len;
    return 1;
}

static int write_metric_int32(uint8_t **p, uint8_t *end, const char *name, uint64_t ts_ms, int32_t val) {
    uint8_t temp[128];
    uint8_t *tp = temp;
    uint8_t *tend = temp + sizeof(temp);

    if (!write_string(&tp, tend, 1, name)) return 0;
    if (!write_key(&tp, tend, 3, 0) || !write_varint(&tp, tend, ts_ms)) return 0;
    if (!write_key(&tp, tend, 4, 0) || !write_varint(&tp, tend, SPB_DATA_TYPE_INT32)) return 0;
    if (!write_key(&tp, tend, 10, 0) || !write_varint(&tp, tend, (uint64_t)val)) return 0;

    size_t metric_len = tp - temp;
    if (!write_key(p, end, 2, 2)) return 0;
    if (!write_varint(p, end, metric_len)) return 0;
    if (*p + metric_len > end) return 0;
    memcpy(*p, temp, metric_len);
    *p += metric_len;
    return 1;
}

static int write_metric_string(uint8_t **p, uint8_t *end, const char *name, uint64_t ts_ms, const char *val) {
    uint8_t temp[256];
    uint8_t *tp = temp;
    uint8_t *tend = temp + sizeof(temp);

    if (!write_string(&tp, tend, 1, name)) return 0;
    if (!write_key(&tp, tend, 3, 0) || !write_varint(&tp, tend, ts_ms)) return 0;
    if (!write_key(&tp, tend, 4, 0) || !write_varint(&tp, tend, 12)) return 0; /* SPB_DATA_TYPE_STRING = 12 */
    if (!write_string(&tp, tend, 17, val)) return 0; /* String_value = field 17 */

    size_t metric_len = tp - temp;
    if (!write_key(p, end, 2, 2)) return 0;
    if (!write_varint(p, end, metric_len)) return 0;
    if (*p + metric_len > end) return 0;
    memcpy(*p, temp, metric_len);
    *p += metric_len;
    return 1;
}

static const char *get_sensor_type_name(uint8_t type) {
    switch (type) {
        case 1: return "pH";
        case 2: return "ORP";
        case 3: return "EC";
        case 4: return "DO";
        case 5: return "Ammonia";
        case 6: return "Ultrasonic";
        case 7: return "Multi-US";
        default: return "Unknown";
    }
}

size_t sparkplug_encode_nbirth(uint8_t *buf, size_t max_len, uint64_t timestamp_ms, uint64_t seq,
                               const TelemetryBatch_t *batch, const Gateway_Config_t *cfg, const uint8_t *relays) {
    uint8_t *p = buf;
    uint8_t *end = buf + max_len;

    /* 1. timestamp (field 1, varint) */
    if (!write_key(&p, end, 1, 0) || !write_varint(&p, end, timestamp_ms)) return 0;

    /* 2. Metrics (field 2, repeated length-delimited) */
    
    /* System properties / metadata as birth metrics */
    if (!write_metric_int32(&p, end, "bdSeq", timestamp_ms, 0)) return 0;
    
    /* Device serial and hardware status */
    if (!write_metric_int32(&p, end, "System/Serial", timestamp_ms, cfg->serial)) return 0;
    if (!write_metric_int32(&p, end, "System/Uptime", timestamp_ms, (int32_t)(timestamp_ms / 1000))) return 0;
    if (!write_metric_string(&p, end, "System/Status", timestamp_ms, "ONLINE")) return 0;
    if (!write_metric_string(&p, end, "System/Firmware", timestamp_ms, FW_VERSION)) return 0;
    
    /* Sensor properties and values */
    for (int i = 0; i < batch->count; i++) {
        if (!batch->records[i].valid) continue;
        char name_buf[64];
        const char *tname = get_sensor_type_name(batch->records[i].type);
        
        snprintf(name_buf, sizeof(name_buf), "Sensors/%s_%u/Value", tname, batch->records[i].id);
        if (!write_metric_float(&p, end, name_buf, timestamp_ms, batch->records[i].avg_value)) return 0;
        
        snprintf(name_buf, sizeof(name_buf), "Sensors/%s_%u/Temperature", tname, batch->records[i].id);
        if (!write_metric_float(&p, end, name_buf, timestamp_ms, batch->records[i].avg_temp)) return 0;
    }

    /* Actuators status */
    uint8_t relay_cnt = cfg ? cfg->actuator_count : MAX_RELAYS;
    if (relay_cnt > MAX_RELAYS) relay_cnt = MAX_RELAYS;
    for (int i = 0; i < relay_cnt; i++) {
        char name_buf[64];
        if (cfg && cfg->actuators[i].name[0] != '\0') {
            snprintf(name_buf, sizeof(name_buf), "Actuators/%s", cfg->actuators[i].name);
        } else {
            snprintf(name_buf, sizeof(name_buf), "Actuators/Actuator_%d", i + 1);
        }
        uint8_t atype = cfg ? cfg->actuators[i].type : ACTUATOR_TYPE_LOCAL_GPIO;
        uint8_t ch = cfg ? cfg->actuators[i].pin_or_slave : 0;

        if (atype == ACTUATOR_TYPE_PWM) {
            const PWM_Channel_Info_t *pw = PWM_GetChannelInfo(ch < MAX_PWM_CHANNELS ? ch : 0);
            float duty = pw ? pw->duty_pct : 0.0f;
            if (!write_metric_float(&p, end, name_buf, timestamp_ms, duty)) return 0;
        } else if (atype == ACTUATOR_TYPE_ANALOG_MA) {
            float ma = DAC_420MA_GetCurrent(ch < MAX_420MA_CHANNELS ? ch : 0);
            if (!write_metric_float(&p, end, name_buf, timestamp_ms, ma)) return 0;
        } else if (atype == ACTUATOR_TYPE_ANALOG_V) {
            float v = Analog_010V_GetVoltage(ch < MAX_010V_CHANNELS ? ch : 0);
            if (!write_metric_float(&p, end, name_buf, timestamp_ms, v)) return 0;
        } else if (atype == ACTUATOR_TYPE_PTO) {
            const PTO_Channel_Status_t *pt = PTO_GetStatus(ch < MAX_PTO_CHANNELS ? ch : 0);
            int32_t pos = pt ? (int32_t)pt->position : 0;
            uint32_t freq = (pt && pt->speed_pps > 0) ? pt->speed_pps : (cfg && cfg->actuators[i].reg_addr ? cfg->actuators[i].reg_addr : 5000);
            if (!write_metric_int32(&p, end, name_buf, timestamp_ms, pos)) return 0;
            char sub_name[80];
            snprintf(sub_name, sizeof(sub_name), "%s/Frequency", name_buf);
            if (!write_metric_int32(&p, end, sub_name, timestamp_ms, (int32_t)freq)) return 0;
            snprintf(sub_name, sizeof(sub_name), "%s/Steps", name_buf);
            if (!write_metric_int32(&p, end, sub_name, timestamp_ms, pos)) return 0;
            snprintf(sub_name, sizeof(sub_name), "%s/Moving", name_buf);
            if (!write_metric_bool(&p, end, sub_name, timestamp_ms, pt ? pt->moving : 0)) return 0;
        } else {
            if (!write_metric_bool(&p, end, name_buf, timestamp_ms, relays[i])) return 0;
        }
    }

    /* 3. seq (field 3, varint) */
    if (!write_key(&p, end, 3, 0) || !write_varint(&p, end, seq)) return 0;

    return p - buf;
}

size_t sparkplug_encode_ddata(uint8_t *buf, size_t max_len, uint64_t timestamp_ms, uint64_t seq,
                              const TelemetryBatch_t *batch, const Gateway_Config_t *cfg, const uint8_t *relays) {
    uint8_t *p = buf;
    uint8_t *end = buf + max_len;

    /* 1. timestamp (field 1, varint) */
    if (!write_key(&p, end, 1, 0) || !write_varint(&p, end, timestamp_ms)) return 0;

    /* 2. Metrics (field 2) */
    
    /* Device serial, uptime, status, version */
    if (cfg && cfg->serial > 0) {
        if (!write_metric_int32(&p, end, "System/Serial", timestamp_ms, cfg->serial)) return 0;
    }
    if (!write_metric_int32(&p, end, "System/Uptime", timestamp_ms, (int32_t)(timestamp_ms / 1000))) return 0;
    if (!write_metric_string(&p, end, "System/Status", timestamp_ms, "ONLINE")) return 0;
    if (!write_metric_string(&p, end, "System/Firmware", timestamp_ms, FW_VERSION)) return 0;

    /* Sensors — emit all configured sensors (or batch records if no config), with online/offline quality flag */
    uint8_t total_cfg_sensors = (cfg && cfg->sensors.count > 0 && cfg->sensors.count <= MAX_SENSORS) ? cfg->sensors.count : 0;
    if (total_cfg_sensors > 0) {
        for (uint8_t i = 0; i < total_cfg_sensors; i++) {
            uint8_t stype = cfg->sensors.entries[i].type;
            uint8_t sid   = cfg->sensors.entries[i].id;
            const char *tname = get_sensor_type_name(stype);

            int b_idx = -1;
            if (batch) {
                for (int b = 0; b < batch->count; b++) {
                    if (batch->records[b].type == stype && batch->records[b].id == sid) {
                        b_idx = b;
                        break;
                    }
                }
            }

            uint8_t is_online = (b_idx >= 0 && batch->records[b_idx].valid);
            if (cfg->mqtt_skip_offline && !is_online) continue;

            float val  = is_online ? batch->records[b_idx].avg_value : 0.0f;
            float temp = is_online ? batch->records[b_idx].avg_temp : 0.0f;

            char name_buf[64];
            snprintf(name_buf, sizeof(name_buf), "Sensors/%s_%u/Value", tname, sid);
            if (!write_metric_float(&p, end, name_buf, timestamp_ms, val)) return 0;

            snprintf(name_buf, sizeof(name_buf), "Sensors/%s_%u/Temperature", tname, sid);
            if (!write_metric_float(&p, end, name_buf, timestamp_ms, temp)) return 0;

            snprintf(name_buf, sizeof(name_buf), "Sensors/%s_%u/Online", tname, sid);
            if (!write_metric_bool(&p, end, name_buf, timestamp_ms, is_online)) return 0;
        }
    } else if (batch && batch->count > 0) {
        for (int i = 0; i < batch->count; i++) {
            if (cfg && cfg->mqtt_skip_offline && !batch->records[i].valid) continue;
            char name_buf[64];
            const char *tname = get_sensor_type_name(batch->records[i].type);

            snprintf(name_buf, sizeof(name_buf), "Sensors/%s_%u/Value", tname, batch->records[i].id);
            if (!write_metric_float(&p, end, name_buf, timestamp_ms,
                batch->records[i].valid ? batch->records[i].avg_value : 0.0f)) return 0;

            snprintf(name_buf, sizeof(name_buf), "Sensors/%s_%u/Temperature", tname, batch->records[i].id);
            if (!write_metric_float(&p, end, name_buf, timestamp_ms,
                batch->records[i].valid ? batch->records[i].avg_temp : 0.0f)) return 0;

            snprintf(name_buf, sizeof(name_buf), "Sensors/%s_%u/Online", tname, batch->records[i].id);
            if (!write_metric_bool(&p, end, name_buf, timestamp_ms, batch->records[i].valid)) return 0;
        }
    }

    /* Actuators */
    uint8_t relay_cnt = cfg ? cfg->actuator_count : MAX_RELAYS;
    if (relay_cnt > MAX_RELAYS) relay_cnt = MAX_RELAYS;
    for (int i = 0; i < relay_cnt; i++) {
        char name_buf[64];
        if (cfg && cfg->actuators[i].name[0] != '\0') {
            snprintf(name_buf, sizeof(name_buf), "Actuators/%s", cfg->actuators[i].name);
        } else {
            snprintf(name_buf, sizeof(name_buf), "Actuators/Actuator_%d", i + 1);
        }
        uint8_t atype = cfg ? cfg->actuators[i].type : ACTUATOR_TYPE_LOCAL_GPIO;
        uint8_t ch = cfg ? cfg->actuators[i].pin_or_slave : 0;

        if (atype == ACTUATOR_TYPE_PWM) {
            const PWM_Channel_Info_t *pw = PWM_GetChannelInfo(ch < MAX_PWM_CHANNELS ? ch : 0);
            float duty = pw ? pw->duty_pct : 0.0f;
            if (!write_metric_float(&p, end, name_buf, timestamp_ms, duty)) return 0;
        } else if (atype == ACTUATOR_TYPE_ANALOG_MA) {
            float ma = DAC_420MA_GetCurrent(ch < MAX_420MA_CHANNELS ? ch : 0);
            if (!write_metric_float(&p, end, name_buf, timestamp_ms, ma)) return 0;
        } else if (atype == ACTUATOR_TYPE_ANALOG_V) {
            float v = Analog_010V_GetVoltage(ch < MAX_010V_CHANNELS ? ch : 0);
            if (!write_metric_float(&p, end, name_buf, timestamp_ms, v)) return 0;
        } else if (atype == ACTUATOR_TYPE_PTO) {
            const PTO_Channel_Status_t *pt = PTO_GetStatus(ch < MAX_PTO_CHANNELS ? ch : 0);
            int32_t pos = pt ? (int32_t)pt->position : 0;
            uint32_t freq = (pt && pt->speed_pps > 0) ? pt->speed_pps : (cfg && cfg->actuators[i].reg_addr ? cfg->actuators[i].reg_addr : 5000);
            if (!write_metric_int32(&p, end, name_buf, timestamp_ms, pos)) return 0;
            char sub_name[80];
            snprintf(sub_name, sizeof(sub_name), "%s/Frequency", name_buf);
            if (!write_metric_int32(&p, end, sub_name, timestamp_ms, (int32_t)freq)) return 0;
            snprintf(sub_name, sizeof(sub_name), "%s/Steps", name_buf);
            if (!write_metric_int32(&p, end, sub_name, timestamp_ms, pos)) return 0;
            snprintf(sub_name, sizeof(sub_name), "%s/Moving", name_buf);
            if (!write_metric_bool(&p, end, sub_name, timestamp_ms, pt ? pt->moving : 0)) return 0;
        } else {
            if (!write_metric_bool(&p, end, name_buf, timestamp_ms, relays[i])) return 0;
        }
    }

    /* 3. seq (field 3, varint) */
    if (!write_key(&p, end, 3, 0) || !write_varint(&p, end, seq)) return 0;

    return p - buf;
}

size_t sparkplug_encode_ndeath(uint8_t *buf, size_t max_len, uint64_t timestamp_ms, uint64_t seq) {
    uint8_t *p = buf;
    uint8_t *end = buf + max_len;

    /* 1. timestamp (field 1, varint) */
    if (!write_key(&p, end, 1, 0) || !write_varint(&p, end, timestamp_ms)) return 0;

    /* bdSeq metric for Node Death */
    if (!write_metric_int32(&p, end, "bdSeq", timestamp_ms, 0)) return 0;

    /* 3. seq (field 3, varint) */
    if (!write_key(&p, end, 3, 0) || !write_varint(&p, end, seq)) return 0;

    return p - buf;
}
