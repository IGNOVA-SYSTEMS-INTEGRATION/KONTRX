#ifndef PTO_MOTION_H
#define PTO_MOTION_H

#include <stdint.h>

#define MAX_PTO_CHANNELS  4U

typedef enum {
    LMT_STATE_CLEAR    = 0,
    LMT_STATE_HIT_FWD  = 1,
    LMT_STATE_HIT_REV  = 2
} LimitState_t;

typedef struct {
    uint8_t      id;
    int32_t      position;          // Current position (steps)
    int32_t      target;            // Target position (steps)
    uint32_t     speed_pps;         // Pulses per second (speed)
    uint32_t     steps_remaining;   // Steps left to execute
    uint8_t      moving;            // 1 = moving, 0 = stopped
    uint8_t      direction;         // 1 = CW / forward, 0 = CCW / reverse
    LimitState_t lmt_state;         // Limit switch status
    const char   *pul_pin;
    const char   *dir_pin;
    const char   *lmt_pin;
    const char   *timer_name;
} PTO_Channel_Status_t;

void PTO_Motion_Init(void);
void PTO_MoveRelative(uint8_t ch, int32_t steps, uint32_t speed_pps);
void PTO_MoveAbsolute(uint8_t ch, int32_t target_pos, uint32_t speed_pps);
void PTO_Stop(uint8_t ch);
void PTO_EmergencyStop(uint8_t ch);
void PTO_SetHome(uint8_t ch);
const PTO_Channel_Status_t* PTO_GetStatus(uint8_t ch);
uint8_t PTO_GetChannelCount(void);

#endif // PTO_MOTION_H