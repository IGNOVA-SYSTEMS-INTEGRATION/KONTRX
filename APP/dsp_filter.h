#ifndef DSP_FILTER_H
#define DSP_FILTER_H

#include <stdint.h>
#include <stddef.h>

#define DSP_MEDIAN_WINDOW_SIZE 5U

typedef struct {
    uint8_t  type;
    uint8_t  id;
    uint8_t  initialized;
    uint8_t  window_count;
    uint8_t  window_index;
    float    raw_history[DSP_MEDIAN_WINDOW_SIZE];
    float    last_accepted_value;
    float    ema_value;
    float    last_ema_value;
    uint32_t last_timestamp;
    float    rate_of_change; /* Derivative: (new - old) / dt */
} DSP_Sensor_State_t;

/* Reset all filter states */
void DSP_Filter_Init(void);

/* Process a raw sensor value through Outlier Rejection -> Median -> EMA -> Rate of Change */
float DSP_Filter_ProcessSample(uint8_t type, uint8_t id, float raw_val, uint32_t current_time_s, float *out_ema, float *out_roc);

#endif // DSP_FILTER_H
