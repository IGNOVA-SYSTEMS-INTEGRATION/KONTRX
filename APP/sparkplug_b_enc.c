#include "sparkplug_b_enc.h"
#include <stdio.h>

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
    char hw_ver[16];
    snprintf(hw_ver, sizeof(hw_ver), "%lu", (unsigned long)cfg->serial);
    if (!write_metric_int32(&p, end, "serial", timestamp_ms, cfg->serial)) return 0;
    
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

    /* Relays status */
    for (int i = 0; i < cfg->relay_count && i < MAX_RELAYS; i++) {
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "Relays/Relay_%d", i);
        if (!write_metric_bool(&p, end, name_buf, timestamp_ms, relays[i])) return 0;
    }

    /* 3. seq (field 3, varint) */
    if (!write_key(&p, end, 3, 0) || !write_varint(&p, end, seq)) return 0;

    return p - buf;
}

size_t sparkplug_encode_ddata(uint8_t *buf, size_t max_len, uint64_t timestamp_ms, uint64_t seq,
                              const TelemetryBatch_t *batch, const uint8_t *relays) {
    uint8_t *p = buf;
    uint8_t *end = buf + max_len;

    /* 1. timestamp (field 1, varint) */
    if (!write_key(&p, end, 1, 0) || !write_varint(&p, end, timestamp_ms)) return 0;

    /* 2. Metrics (field 2) */
    for (int i = 0; i < batch->count; i++) {
        if (!batch->records[i].valid) continue;
        char name_buf[64];
        const char *tname = get_sensor_type_name(batch->records[i].type);
        
        snprintf(name_buf, sizeof(name_buf), "Sensors/%s_%u/Value", tname, batch->records[i].id);
        if (!write_metric_float(&p, end, name_buf, timestamp_ms, batch->records[i].avg_value)) return 0;
        
        snprintf(name_buf, sizeof(name_buf), "Sensors/%s_%u/Temperature", tname, batch->records[i].id);
        if (!write_metric_float(&p, end, name_buf, timestamp_ms, batch->records[i].avg_temp)) return 0;
    }

    /* Relays */
    for (int i = 0; i < MAX_RELAYS; i++) {
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "Relays/Relay_%d", i);
        if (!write_metric_bool(&p, end, name_buf, timestamp_ms, relays[i])) return 0;
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
