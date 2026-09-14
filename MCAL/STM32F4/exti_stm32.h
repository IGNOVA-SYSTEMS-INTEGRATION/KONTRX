#ifndef EXTI_STM32_H
#define EXTI_STM32_H

#include <stdint.h>

typedef void (*LimitSwitch_Callback_t)(uint8_t channel);

/**
 * @brief Initialize EXTI lines for Limit Switches:
 *        Channel 0: PE0 (EXTI0)
 *        Channel 1: PE1 (EXTI1)
 *        Channel 2: PE3 (EXTI3)
 *        Channel 3: PE7 (EXTI7)
 */
void EXTI_LimitSwitches_Init(void);

/**
 * @brief Register callback function for limit switch trigger
 */
void EXTI_RegisterLimitSwitchCallback(LimitSwitch_Callback_t cb);

/**
 * @brief Get physical pin state of a limit switch (1 = triggered/active, 0 = clear)
 */
uint8_t EXTI_GetLimitSwitchState(uint8_t channel);

#endif // EXTI_STM32_H