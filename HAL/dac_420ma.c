/**
 * @file    dac_420ma.c
 * @brief   Kontrx 4-20mA Analog Current Output Driver (SPI3 DAC)
 */

#include "dac_420ma.h"
#include "spi3_stm32.h"

static DAC_420MA_Channel_t s_420ma[MAX_420MA_CHANNELS];

void DAC_420MA_Init(void) {
    SPI3_Init();

    for (uint8_t i = 0; i < MAX_420MA_CHANNELS; i++) {
        s_420ma[i].id = i;
        s_420ma[i].current_mA = 4.0f; // 4.0 mA safe minimum
        s_420ma[i].raw_dac = 0;
        s_420ma[i].chip_idx = i / 2U;
        s_420ma[i].dac_sub_ch = i % 2U;
    }
}

void DAC_420MA_SetCurrent(uint8_t ch, float mA) {
    if (ch >= MAX_420MA_CHANNELS) return;
    if (mA < 0.0f)  mA = 0.0f;
    if (mA > 20.5f) mA = 20.5f;

    s_420ma[ch].current_mA = mA;

    // Linear mapping: 4.00 mA = 0, 20.00 mA = 4095
    float norm = (mA - 4.0f) / 16.0f;
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    uint16_t raw = (uint16_t)(norm * 4095.0f + 0.5f);
    s_420ma[ch].raw_dac = raw;

    // Build MCP4922 SPI 16-bit frame:
    // [15] DACA/DACB: 0=A, 1=B
    // [14] BUF: 1=Buffered
    // [13] GA: 1=1x Gain
    // [12] SHDN: 1=Active output
    // [11:0] Data
    uint16_t chan_bit = s_420ma[ch].dac_sub_ch ? 1U : 0U;
    uint16_t frame = (chan_bit << 15) | (1U << 14) | (1U << 13) | (1U << 12) | (raw & 0x0FFFU);

    uint8_t chip = s_420ma[ch].chip_idx;
    SPI3_CS_Select(chip);
    SPI3_Write16(frame);
    SPI3_CS_Deselect(chip);
}

float DAC_420MA_GetCurrent(uint8_t ch) {
    if (ch < MAX_420MA_CHANNELS) return s_420ma[ch].current_mA;
    return 0.0f;
}

const DAC_420MA_Channel_t* DAC_420MA_GetChannelInfo(uint8_t ch) {
    if (ch < MAX_420MA_CHANNELS) return &s_420ma[ch];
    return 0;
}

uint8_t DAC_420MA_GetChannelCount(void) {
    return MAX_420MA_CHANNELS;
}