#include "dsp_filter.h"
#include <string.h>

#define MAX_DSP_SENSORS 32
static DSP_Sensor_State_t s_dsp_states[MAX_DSP_SENSORS];
static uint8_t s_dsp_count = 0;

void DSP_Filter_Init(void) {
    memset(s_dsp_states, 0, sizeof(s_dsp_states));
    s_dsp_count = 0;
}

static DSP_Sensor_State_t *Get_Sensor_State(uint8_t type, uint8_t id) {
    for (uint8_t i = 0; i < s_dsp_count; i++) {
        if (s_dsp_states[i].type == type && s_dsp_states[i].id == id) {
            return &s_dsp_states[i];
        }
    }
    if (s_dsp_count < MAX_DSP_SENSORS) {
        DSP_Sensor_State_t *st = &s_dsp_states[s_dsp_count++];
        memset(st, 0, sizeof(DSP_Sensor_State_t));
        st->type = type;
        st->id = id;
        return st;
    }
    return &s_dsp_states[0]; /* Fallback */
}

static float Get_Max_Delta(uint8_t type) {
    switch (type) {
        case 1: return 2.0f;    /* pH: max 2.0 pH unit jump per sample */
        case 2: return 100.0f;  /* ORP: max 100 mV jump */
        case 3: return 1000.0f; /* EC: max 1000 uS/cm jump */
        case 4: return 3.0f;    /* DO: max 3.0 mg/L jump */
        case 5: return 5.0f;    /* Ammonia: max 5.0 ppm jump */
        case 6:
        case 7: return 50.0f;   /* Ultrasonic: max 50 cm jump */
        default: return 20.0f;
    }
}

static float Compute_Median_5(float *arr, uint8_t count) {
    if (count == 0) return 0.0f;
    if (count == 1) return arr[0];

    float temp[DSP_MEDIAN_WINDOW_SIZE];
    memcpy(temp, arr, count * sizeof(float));

    /* Simple Bubble Sort */
    for (uint8_t i = 0; i < count - 1; i++) {
        for (uint8_t j = 0; j < count - i - 1; j++) {
            if (temp[j] > temp[j + 1]) {
                float swap = temp[j];
                temp[j] = temp[j + 1];
                temp[j + 1] = swap;
            }
        }
    }

    return temp[count / 2];
}

float DSP_Filter_ProcessSample(uint8_t type, uint8_t id, float raw_val, uint32_t current_time_s, float *out_ema, float *out_roc) {
    DSP_Sensor_State_t *st = Get_Sensor_State(type, id);

    if (!st->initialized) {
        st->initialized = 1;
        st->last_accepted_value = raw_val;
        st->ema_value = raw_val;
        st->last_ema_value = raw_val;
        st->last_timestamp = current_time_s;
        st->rate_of_change = 0.0f;
        for (uint8_t i = 0; i < DSP_MEDIAN_WINDOW_SIZE; i++) {
            st->raw_history[i] = raw_val;
        }
        st->window_count = 1;
        st->window_index = 1;
        if (out_ema) *out_ema = raw_val;
        if (out_roc) *out_roc = 0.0f;
        return raw_val;
    }

    /* ── Step 1: Thresholding / Outlier Rejection ── */
    float max_delta = Get_Max_Delta(type);
    float accepted_val = raw_val;
    float diff = raw_val - st->last_accepted_value;

    if (diff > max_delta) {
        accepted_val = st->last_accepted_value + max_delta; /* Clip positive spike */
    } else if (diff < -max_delta) {
        accepted_val = st->last_accepted_value - max_delta; /* Clip negative spike */
    }
    st->last_accepted_value = accepted_val;

    /* ── Step 2: Median Filter (Circular Window = 5) ── */
    st->raw_history[st->window_index % DSP_MEDIAN_WINDOW_SIZE] = accepted_val;
    st->window_index++;
    if (st->window_count < DSP_MEDIAN_WINDOW_SIZE) {
        st->window_count++;
    }
    float median_val = Compute_Median_5(st->raw_history, st->window_count);

    /* ── Step 3: Exponential Moving Average (EMA) ──
     * α = 0.15 gives 15% weight to new sample, 85% to history */
    const float alpha = 0.15f;
    st->ema_value = (alpha * median_val) + ((1.0f - alpha) * st->ema_value);

    /* ── Step 4: Rate of Change Calculation ── */
    if (current_time_s > st->last_timestamp) {
        uint32_t dt = current_time_s - st->last_timestamp;
        st->rate_of_change = (st->ema_value - st->last_ema_value) / (float)dt;
        st->last_ema_value = st->ema_value;
        st->last_timestamp = current_time_s;
    }

    if (out_ema) *out_ema = st->ema_value;
    if (out_roc) *out_roc = st->rate_of_change;

    return st->ema_value;
}
