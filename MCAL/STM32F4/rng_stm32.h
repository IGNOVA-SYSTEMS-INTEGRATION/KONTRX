#ifndef RNG_STM32_H
#define RNG_STM32_H

#include <stdint.h>

/* STM32F4 true random number generator (clocked by PLL48CK = 48 MHz). */
void    RNG_Init(void);

/* Writes one 32-bit random word. Returns 1 on success, 0 if the generator
 * reported a clock/seed error or timed out (the caller must not fall back
 * to a predictable value). */
uint8_t RNG_Read32(uint32_t *out);

#endif /* RNG_STM32_H */
