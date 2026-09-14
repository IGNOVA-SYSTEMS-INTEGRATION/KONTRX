#ifndef ANALOG_010V_H
#define ANALOG_010V_H

#include <stdint.h>

#define MAX_010V_CHANNELS  2U

typedef struct {
    uint8_t id;
    float   voltage_V;     // 0.0 to 10.0 V
    float   duty_pct;      // 0.0 to 100.0%
    uint8_t pwm_channel;   // maps to PWM channel (2 for PD14, 3 for PD15)
    const char *pin_name;
} Analog_010V_Channel_t;

void Analog_010V_Init(void);
void Analog_010V_SetVoltage(uint8_t ch, float voltage);
float Analog_010V_GetVoltage(uint8_t ch);
const Analog_010V_Channel_t* Analog_010V_GetChannelInfo(uint8_t ch);
uint8_t Analog_010V_GetChannelCount(void);

#endif // ANALOG_010V_H