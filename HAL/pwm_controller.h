#ifndef PWM_CONTROLLER_H
#define PWM_CONTROLLER_H

#include <stdint.h>

#define MAX_PWM_CHANNELS  7U

typedef struct {
    uint8_t  id;
    uint8_t  enabled;
    float    duty_pct;      // 0.0 to 100.0%
    uint32_t freq_hz;       // e.g. 1000 Hz
    const char *pin_name;
    const char *timer_name;
} PWM_Channel_Info_t;

/**
 * @brief Initialize all configured hardware PWM channels
 */
void PWM_Controller_Init(void);

/**
 * @brief Set PWM duty cycle for specific channel (0..6)
 */
void PWM_SetDuty(uint8_t channel, float duty_pct);

/**
 * @brief Set PWM frequency for specific channel
 */
void PWM_SetFrequency(uint8_t channel, uint32_t freq_hz);

/**
 * @brief Get info/status for all PWM channels
 */
const PWM_Channel_Info_t* PWM_GetChannelInfo(uint8_t channel);
uint8_t PWM_GetChannelCount(void);

#endif // PWM_CONTROLLER_H