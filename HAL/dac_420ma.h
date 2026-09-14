#ifndef DAC_420MA_H
#define DAC_420MA_H

#include <stdint.h>

#define MAX_420MA_CHANNELS  8U

typedef struct {
    uint8_t  id;
    float    current_mA;    // 4.0 to 20.0 mA
    uint16_t raw_dac;       // 0 to 4095
    uint8_t  chip_idx;      // 0..3
    uint8_t  dac_sub_ch;    // 0 or 1 (A or B)
} DAC_420MA_Channel_t;

void DAC_420MA_Init(void);
void DAC_420MA_SetCurrent(uint8_t ch, float mA);
float DAC_420MA_GetCurrent(uint8_t ch);
const DAC_420MA_Channel_t* DAC_420MA_GetChannelInfo(uint8_t ch);
uint8_t DAC_420MA_GetChannelCount(void);

#endif // DAC_420MA_H