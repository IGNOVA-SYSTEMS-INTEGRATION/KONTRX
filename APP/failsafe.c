/**
 * @file    failsafe.c
 * @brief   Per-actuator fail-safe states and last-state persistence.
 *          See failsafe.h for when each state applies.
 */
#include "failsafe.h"
#include "freertos_tasks.h"
#include "flash_partition.h"
#include "w25q16.h"
#include "gpio_stm32.h"
#include "stm32f407_regs.h"
#include "pwm_controller.h"
#include "dac_420ma.h"
#include "analog_010v.h"
#include "pto_motion.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

/* W25Q16 layout: the top of the unused 512 KB web partition. */
#define FS_CFG_PRIMARY_ADDR   0x0007C000U
#define FS_CFG_BACKUP_ADDR    0x0007D000U
#define FS_RING_ADDR          0x0007E000U   /* two 4 KB sectors */
#define FS_RING_SECTORS       2U
#define FS_SECTOR_SIZE        4096U
#define FS_SLOT_SIZE          128U          /* never crosses a 256 B page */
#define FS_SLOTS_PER_SECTOR   (FS_SECTOR_SIZE / FS_SLOT_SIZE)
#define FS_RING_SLOTS         (FS_RING_SECTORS * FS_SLOTS_PER_SECTOR)

#define FS_CFG_MAGIC          0xFA15AFE1U
#define FS_CFG_VERSION        1U
#define FS_STATE_MAGIC        0x57A7E5A1U
#define FAILSAFE_SAVE_MIN_MS  2000U

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t relay_mask;           /* bit i = relayStates[i] */
    float    value[MAX_RELAYS];    /* analog/PWM output per actuator */
    uint32_t crc;
} Failsafe_State_t;
_Static_assert(sizeof(Failsafe_State_t) <= FS_SLOT_SIZE, "state record exceeds ring slot");
_Static_assert(sizeof(Failsafe_Config_t) <= FS_SECTOR_SIZE, "failsafe config exceeds sector");

volatile uint32_t g_reset_csr = 0;

static Failsafe_Config_t s_cfg;
static Failsafe_State_t  s_last;          /* last saved (or loaded) state */
static uint8_t           s_have_last = 0;
static uint32_t          s_next_slot = 0;
static uint32_t          s_last_save_tick = 0;

/* Register-level plan for the fault handler: precomputed so the handler
 * reads no config and takes no lock. level = pin level that gives the
 * safe state; 0xFF = leave the pin alone. */
typedef struct {
    volatile uint32_t *bsrr;
    uint8_t pin;
    uint8_t level;
} FaultPin_t;
static FaultPin_t s_fault_plan[MAX_RELAYS];

/* ---------------------------------------------------------------- reset */
#define RCC_CSR_REG   (*(volatile uint32_t *)0x40023874UL)
#define RCC_CSR_RMVF  (1UL << 24)
#define RCC_CSR_BORRSTF (1UL << 25)
#define RCC_CSR_PORRSTF (1UL << 27)

void Failsafe_CaptureResetCause(void) {
    g_reset_csr = RCC_CSR_REG;
    RCC_CSR_REG |= RCC_CSR_RMVF;     /* next reset reports only its own cause */
}

uint8_t Failsafe_WasPowerOnReset(void) {
    return (uint8_t)((g_reset_csr & (RCC_CSR_PORRSTF | RCC_CSR_BORRSTF)) != 0U);
}

/* --------------------------------------------------------------- config */
static void Cfg_Defaults(Failsafe_Config_t *c) {
    memset(c, 0, sizeof(*c));
    c->magic = FS_CFG_MAGIC;
    c->version = FS_CFG_VERSION;
    c->stale_timeout_s = FAILSAFE_STALE_DEFAULT_S;   /* modes all HOLD (0) */
}

static uint8_t Cfg_Valid(const Failsafe_Config_t *c) {
    if (c->magic != FS_CFG_MAGIC || c->version != FS_CFG_VERSION) return 0;
    if (c->crc != Compute_CRC32((const uint8_t *)c, offsetof(Failsafe_Config_t, crc))) return 0;
    if (c->stale_timeout_s < FAILSAFE_STALE_MIN_S || c->stale_timeout_s > FAILSAFE_STALE_MAX_S) return 0;
    for (int i = 0; i < MAX_RELAYS; i++) {
        if (c->mode[i] > FAILSAFE_VALUE || c->restore_on_power[i] > 1) return 0;
        if (c->safe_value[i] != c->safe_value[i]) return 0;   /* NaN */
    }
    return 1;
}

static uint8_t State_Valid(const Failsafe_State_t *s) {
    return (uint8_t)(s->magic == FS_STATE_MAGIC &&
                     s->crc == Compute_CRC32((const uint8_t *)s, offsetof(Failsafe_State_t, crc)));
}

void Failsafe_Init(void) {
    Failsafe_Config_t c;
    W25Q_Read(FS_CFG_PRIMARY_ADDR, (uint8_t *)&c, sizeof(c));
    if (!Cfg_Valid(&c)) {
        W25Q_Read(FS_CFG_BACKUP_ADDR, (uint8_t *)&c, sizeof(c));
        if (!Cfg_Valid(&c)) Cfg_Defaults(&c);
    }
    s_cfg = c;

    /* Newest valid ring slot wins; the slot after it is written next. */
    uint32_t best_seq = 0, best_slot = 0;
    s_have_last = 0;
    for (uint32_t i = 0; i < FS_RING_SLOTS; i++) {
        Failsafe_State_t s;
        W25Q_Read(FS_RING_ADDR + i * FS_SLOT_SIZE, (uint8_t *)&s, sizeof(s));
        if (State_Valid(&s) && (!s_have_last || s.seq > best_seq)) {
            best_seq = s.seq; best_slot = i; s_last = s; s_have_last = 1;
        }
    }
    s_next_slot = s_have_last ? (best_slot + 1U) % FS_RING_SLOTS : 0U;
    if (!s_have_last) memset(&s_last, 0, sizeof(s_last));

    /* A write cut by power loss leaves a non-blank slot after the newest
     * one; NOR flash cannot be programmed over it. Move to the next sector,
     * which Failsafe_PersistTick erases before its first write. */
    if (s_next_slot % FS_SLOTS_PER_SECTOR != 0U) {
        uint8_t buf[FS_SLOT_SIZE];
        W25Q_Read(FS_RING_ADDR + s_next_slot * FS_SLOT_SIZE, buf, sizeof(buf));
        for (uint32_t i = 0; i < sizeof(buf); i++) {
            if (buf[i] != 0xFFU) {
                s_next_slot = ((s_next_slot / FS_SLOTS_PER_SECTOR + 1U) % FS_RING_SECTORS) * FS_SLOTS_PER_SECTOR;
                break;
            }
        }
    }
}

void Failsafe_GetConfig(Failsafe_Config_t *out) {
    taskENTER_CRITICAL();
    *out = s_cfg;
    taskEXIT_CRITICAL();
}

uint8_t Failsafe_SetConfig(const Failsafe_Config_t *in) {
    Failsafe_Config_t c = *in;
    c.magic = FS_CFG_MAGIC;
    c.version = FS_CFG_VERSION;
    c.crc = Compute_CRC32((const uint8_t *)&c, offsetof(Failsafe_Config_t, crc));
    if (!Cfg_Valid(&c)) return 0;

    W25Q_EraseSector(FS_CFG_PRIMARY_ADDR);
    W25Q_Write(FS_CFG_PRIMARY_ADDR, (const uint8_t *)&c, sizeof(c));
    W25Q_EraseSector(FS_CFG_BACKUP_ADDR);
    W25Q_Write(FS_CFG_BACKUP_ADDR, (const uint8_t *)&c, sizeof(c));

    taskENTER_CRITICAL();
    s_cfg = c;
    taskEXIT_CRITICAL();
    return 1;
}

uint16_t Failsafe_StaleTimeoutS(void) { return s_cfg.stale_timeout_s; }
uint8_t  Failsafe_Mode(uint8_t idx)   { return idx < MAX_RELAYS ? s_cfg.mode[idx] : FAILSAFE_HOLD; }

/* ---------------------------------------------------------------- apply */
static uint8_t Is_Gpio(uint8_t type) {
    return (uint8_t)(type == ACTUATOR_TYPE_LOCAL_GPIO || type == ACTUATOR_TYPE_DIGITAL_OUT);
}

/* Drives actuator idx to "on" (relay) or value (analog/PWM). */
static void Drive(uint8_t idx, const Actuator_Config_t *a, uint8_t on, float value) {
    uint8_t ch = a->pin_or_slave;
    switch (a->type) {
        case ACTUATOR_TYPE_PWM:       PWM_SetDuty(ch, value); break;
        case ACTUATOR_TYPE_ANALOG_MA: DAC_420MA_SetCurrent(ch, value); break;
        case ACTUATOR_TYPE_ANALOG_V:  Analog_010V_SetVoltage(ch, value); break;
        case ACTUATOR_TYPE_PTO:       if (!on) PTO_Stop(ch); break;   /* motion: OFF stops */
        default:                      Relay_SetState(idx, on); break;
    }
}

static float Off_Value(uint8_t type) {
    return (type == ACTUATOR_TYPE_ANALOG_MA) ? 4.0f : 0.0f;   /* 4 mA live zero */
}

void Failsafe_ApplyOne(uint8_t idx, const Gateway_Config_t *cfg, FailsafeReason_t why) {
    if (idx >= MAX_RELAYS || idx >= cfg->actuator_count) return;
    const Actuator_Config_t *a = &cfg->actuators[idx];
    uint8_t mode = s_cfg.mode[idx];
    if (mode == FAILSAFE_HOLD) return;
    if (mode == FAILSAFE_OFF)        Drive(idx, a, 0, Off_Value(a->type));
    else if (mode == FAILSAFE_ON)    Drive(idx, a, 1, 0.0f);
    else                             Drive(idx, a, 1, s_cfg.safe_value[idx]);

    static const char *const why_txt[] = { "boot", "fault", "stale inputs", "OTA" };
    char msg[64];
    snprintf(msg, sizeof(msg), "Fail-safe (%s): actuator %u -> %s", why_txt[why],
             (unsigned)(idx + 1), mode == FAILSAFE_OFF ? "OFF" : mode == FAILSAFE_ON ? "ON" : "VALUE");
    Log_Event("SYS", msg);
}

void Failsafe_ApplyAll(const Gateway_Config_t *cfg, FailsafeReason_t why) {
    for (uint8_t i = 0; i < cfg->actuator_count && i < MAX_RELAYS; i++) {
        Failsafe_ApplyOne(i, cfg, why);
    }
}

void Failsafe_RefreshFaultPlan(const Gateway_Config_t *cfg) {
    FaultPin_t plan[MAX_RELAYS];
    for (int i = 0; i < MAX_RELAYS; i++) {
        plan[i].bsrr = NULL; plan[i].pin = 0; plan[i].level = 0xFF;
        if (i >= cfg->actuator_count) continue;
        const Actuator_Config_t *a = &cfg->actuators[i];
        uint8_t mode = s_cfg.mode[i];
        if (!Is_Gpio(a->type) || (mode != FAILSAFE_OFF && mode != FAILSAFE_ON)) continue;
        int port = GPIO_GetPortId(a->port_or_ip);
        if (port < 0 || port > 4 || Relay_PinReserved((uint8_t)port, a->pin_or_slave)) continue;
        uint8_t on = (mode == FAILSAFE_ON);
        plan[i].bsrr  = (volatile uint32_t *)(GPIOA_BASE + (uint32_t)port * 0x400U + 0x18U);
        plan[i].pin   = a->pin_or_slave;
        plan[i].level = a->is_active_low ? (uint8_t)!on : on;
    }
    taskENTER_CRITICAL();
    memcpy(s_fault_plan, plan, sizeof(plan));
    taskEXIT_CRITICAL();
}

void Failsafe_ApplyFromFault(void) {
    for (int i = 0; i < MAX_RELAYS; i++) {
        const FaultPin_t *p = &s_fault_plan[i];
        if (!p->bsrr || p->level == 0xFF) continue;
        *p->bsrr = p->level ? (1UL << p->pin) : (1UL << (p->pin + 16U));
    }
}

/* -------------------------------------------------------------- restore */
void Failsafe_BootRestore(const Gateway_Config_t *cfg) {
    uint8_t power_on = Failsafe_WasPowerOnReset();
    for (uint8_t i = 0; i < cfg->actuator_count && i < MAX_RELAYS; i++) {
        const Actuator_Config_t *a = &cfg->actuators[i];
        uint8_t restore = s_have_last && (!power_on || s_cfg.restore_on_power[i]);
        if (restore) {
            uint8_t on = (uint8_t)((s_last.relay_mask >> i) & 1U);
            if (a->type == ACTUATOR_TYPE_PTO) continue;            /* never resume motion */
            if (a->type == ACTUATOR_TYPE_PWM || a->type == ACTUATOR_TYPE_ANALOG_MA ||
                a->type == ACTUATOR_TYPE_ANALOG_V) {
                Drive(i, a, 1, s_last.value[i]);
            } else if (on) {
                Drive(i, a, 1, 0.0f);
            }
        } else if (s_cfg.mode[i] == FAILSAFE_ON || s_cfg.mode[i] == FAILSAFE_VALUE) {
            /* Not restored: an explicit ON/VALUE fail-safe is the boot state. */
            Failsafe_ApplyOne(i, cfg, FS_REASON_BOOT);
        }
        /* Otherwise the output stays OFF, as Relay_Init / driver init left it. */
    }
    char msg[72];
    snprintf(msg, sizeof(msg), "Outputs at boot: %s reset, last state %s",
             power_on ? "power-on" : "soft", s_have_last ? "found" : "none");
    Log_Event("SYS", msg);
}

/* ---------------------------------------------------------------- persist */
static void Snapshot(const Gateway_Config_t *cfg, Failsafe_State_t *s) {
    memset(s, 0, sizeof(*s));
    for (uint8_t i = 0; i < cfg->actuator_count && i < MAX_RELAYS; i++) {
        const Actuator_Config_t *a = &cfg->actuators[i];
        uint8_t ch = a->pin_or_slave;
        if (relayStates[i]) s->relay_mask |= (1UL << i);
        switch (a->type) {
            case ACTUATOR_TYPE_PWM: {
                const PWM_Channel_Info_t *pi = PWM_GetChannelInfo(ch);
                s->value[i] = pi ? pi->duty_pct : 0.0f;
                break;
            }
            case ACTUATOR_TYPE_ANALOG_MA: s->value[i] = DAC_420MA_GetCurrent(ch); break;
            case ACTUATOR_TYPE_ANALOG_V:  s->value[i] = Analog_010V_GetVoltage(ch); break;
            default: break;
        }
    }
}

void Failsafe_PersistTick(const Gateway_Config_t *cfg) {
    Failsafe_State_t s;
    Snapshot(cfg, &s);
    if (s_have_last && s.relay_mask == s_last.relay_mask &&
        memcmp(s.value, s_last.value, sizeof(s.value)) == 0) return;

    uint32_t now = xTaskGetTickCount();
    if (s_have_last && (now - s_last_save_tick) < pdMS_TO_TICKS(FAILSAFE_SAVE_MIN_MS)) return;

    s.magic = FS_STATE_MAGIC;
    s.seq = s_last.seq + 1U;
    s.crc = Compute_CRC32((const uint8_t *)&s, offsetof(Failsafe_State_t, crc));

    uint32_t addr = FS_RING_ADDR + s_next_slot * FS_SLOT_SIZE;
    if ((s_next_slot % FS_SLOTS_PER_SECTOR) == 0U) {
        W25Q_EraseSector(addr);   /* entering a sector: the other one keeps the history */
    }
    W25Q_Write(addr, (const uint8_t *)&s, sizeof(s));

    s_last = s;
    s_have_last = 1;
    s_last_save_tick = now;
    s_next_slot = (s_next_slot + 1U) % FS_RING_SLOTS;
}
