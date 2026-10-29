/**
 * @file    failsafe.h
 * @brief   Per-actuator fail-safe states and last-state persistence
 *          (audit items ACT-01, ACT-02).
 *
 * Each actuator has a fail-safe mode applied when:
 *   - an unrecoverable fault halts the CPU (GPIO relays/DO only),
 *   - every rule driving it has had stale inputs for stale_timeout_s,
 *   - an OTA is about to erase flash,
 *   - the board boots (see restore below).
 * HOLD (the default) keeps the output as it is.
 *
 * Boot: after a soft reset (OTA, watchdog, fault) every output returns to
 * its last saved state. After a power-on / brown-out reset only actuators
 * with restore_on_power return to their last state; the rest start OFF,
 * so a machine never restarts on its own after a power cut unless asked.
 *
 * Settings live in their own W25Q16 record (primary + backup sectors);
 * the last output states live in a wear-spreading ring. Gateway_Config_t
 * is unchanged.
 */
#ifndef FAILSAFE_H
#define FAILSAFE_H

#include <stdint.h>
#include "modbus_dma.h"   /* MAX_RELAYS, Gateway_Config_t */

typedef enum {
    FAILSAFE_HOLD  = 0,   /* keep the current output */
    FAILSAFE_OFF   = 1,   /* relay off / analog minimum / PWM 0 % */
    FAILSAFE_ON    = 2,   /* relay on (digital outputs only) */
    FAILSAFE_VALUE = 3    /* analog/PWM: drive safe_value */
} FailsafeMode_t;

typedef enum {
    FS_REASON_BOOT = 0,
    FS_REASON_FAULT,
    FS_REASON_STALE,
    FS_REASON_OTA
} FailsafeReason_t;

#define FAILSAFE_STALE_DEFAULT_S   30U
#define FAILSAFE_STALE_MIN_S       5U
#define FAILSAFE_STALE_MAX_S       3600U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t stale_timeout_s;
    uint8_t  mode[MAX_RELAYS];             /* FailsafeMode_t */
    uint8_t  restore_on_power[MAX_RELAYS]; /* 1 = restore last state after power-on */
    float    safe_value[MAX_RELAYS];       /* mA / V / duty % for FAILSAFE_VALUE */
    uint32_t crc;
} Failsafe_Config_t;

/* RCC_CSR as read at reset, before the flags are cleared. */
extern volatile uint32_t g_reset_csr;

/* Reads RCC_CSR into g_reset_csr and clears the reset flags (RMVF).
 * Call once, first thing in main(). */
void    Failsafe_CaptureResetCause(void);
uint8_t Failsafe_WasPowerOnReset(void);

/* Loads settings and the last saved output states (W25Q16 must be up). */
void    Failsafe_Init(void);

void    Failsafe_GetConfig(Failsafe_Config_t *out);
/* Validates, stores and activates new settings. Returns 0 if rejected. */
uint8_t Failsafe_SetConfig(const Failsafe_Config_t *in);

/* Rebuilds the register-level plan the fault handler uses. Call after any
 * actuator configuration change. */
void    Failsafe_RefreshFaultPlan(const Gateway_Config_t *cfg);

/* Drives one / every actuator to its fail-safe state (HOLD = no change). */
void    Failsafe_ApplyOne(uint8_t idx, const Gateway_Config_t *cfg, FailsafeReason_t why);
void    Failsafe_ApplyAll(const Gateway_Config_t *cfg, FailsafeReason_t why);

/* Fault handler only: GPIO relay/DO outputs via BSRR, no RTOS, no locks. */
void    Failsafe_ApplyFromFault(void);

/* Boot: restore per the rules above. Call after every output driver init. */
void    Failsafe_BootRestore(const Gateway_Config_t *cfg);

/* Periodic (~1 s): saves the current output states when they changed and
 * at least FAILSAFE_SAVE_MIN_MS passed since the last save. */
void    Failsafe_PersistTick(const Gateway_Config_t *cfg);

uint16_t Failsafe_StaleTimeoutS(void);
uint8_t  Failsafe_Mode(uint8_t idx);

#endif /* FAILSAFE_H */
