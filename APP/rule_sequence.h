/**
 * @file    rule_sequence.h
 * @brief   Timed action sequence state machine (rule "sequence" object).
 *
 * Pure logic, no RTOS calls: Task_ControlEngine passes the condition result
 * and the tick count every cycle; host tests drive it the same way.
 *
 * on_false behaviour (condition turns false while running):
 *   ABORT_SAFE   -> stop at once (caller applies auto_reverse if set)
 *   FINISH_CYCLE -> run to the end of the current cycle, then stop
 *   HOLD         -> keep the current step and pause its timer; resume from
 *                   the same point when the condition is true again
 * ONCE / COUNT run once per rising edge of the condition: after they finish,
 * they wait for the condition to go false before they can start again.
 */
#ifndef RULE_SEQUENCE_H
#define RULE_SEQUENCE_H

#include <stdint.h>
#include "flash_partition.h"

typedef struct {
    uint8_t  active;
    uint8_t  current_step;
    uint8_t  current_cycle;
    uint8_t  done;            /* ONCE/COUNT finished, condition still true */
    uint8_t  held;            /* HOLD: paused while condition is false */
    uint8_t  stopping;        /* FINISH_CYCLE: stop at the end of this cycle */
    uint8_t  padding[2];
    uint32_t step_start_ticks;
    uint32_t held_elapsed;    /* ticks already spent in the held step */
} RuleSeqState_t;

/**
 * Advances the sequence by one control cycle.
 * Returns 1 while a step drives the output (*out_step = its index),
 * 0 when the sequence is idle.
 */
uint8_t RuleSeq_Update(const ActionSequence_t *seq, RuleSeqState_t *st,
                       uint8_t cond_met, uint32_t now_ticks,
                       uint32_t tick_rate_hz, uint8_t *out_step);

#endif /* RULE_SEQUENCE_H */
