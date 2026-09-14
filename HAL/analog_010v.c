/**
 * @file    analog_010v.c
 * @brief   Kontrx 0-10V DC Voltage Output Driver (PWM + LPF + OpAmp Circuit)
 */

#include "analog_010v.h"
#include "pwm_controller.h"

static Analog_010V_Channel_t s_010v[MAX_010V_CHANNELS] = {
    {0, 0.0f, 0.0f, 2, "PD14 (PWM3)"},
    {1, 0.0f, 0.0f, 3, "PD15 (PWM4)"}
};

void Analog_010V_Init(void) {
    for (uint8_t i = 0; i < MAX_010V_CHANNELS; i++) {
        s_010v[i].voltage_V = 0.0f;
        s_010v[i].duty_pct = 0.0f;
        // High frequency (20 kHz) ensures low ripple after Low-Pass Filter
        PWM_SetFrequency(s_010v[i].pwm_channel, 20000U);
        PWM_SetDuty(s_010v[i].pwm_channel, 0.0f);
    }
}

void Analog_010V_SetVoltage(uint8_t ch, float voltage) {
    if (ch >= MAX_010V_CHANNELS) return;
    if (voltage < 0.0f)   voltage = 0.0f;
    if (voltage > 10.0f)  voltage = 10.0f;

    s_010v[ch].voltage_V = voltage;
    float duty = (voltage / 10.0f) * 100.0f;
    s_010v[ch].duty_pct = duty;

    PWM_SetDuty(s_010v[ch].pwm_channel, duty);
}

float Analog_010V_GetVoltage(uint8_t ch) {
    if (ch < MAX_010V_CHANNELS) return s_010v[ch].voltage_V;
    return 0.0f;
}

const Analog_010V_Channel_t* Analog_010V_GetChannelInfo(uint8_t ch) {
    if (ch < MAX_010V_CHANNELS) return &s_010v[ch];
    return 0;
}

uint8_t Analog_010V_GetChannelCount(void) {
    return MAX_010V_CHANNELS;
}