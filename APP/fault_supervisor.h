/**
 * @file    fault_supervisor.h
 * @brief   Task-level fault isolation: a faulting task is removed and
 *          restarted while the rest of the system keeps running. Outputs
 *          keep their last state (nothing here touches them).
 *
 * Handled without a reset:
 *   - HardFault / MemManage / BusFault / UsageFault raised by task code
 *   - FreeRTOS stack overflow (configCHECK_FOR_STACK_OVERFLOW = 2)
 *   - heap allocation failure (the request fails; the board stays up)
 * Not recoverable (recorded, then the CPU halts with outputs frozen):
 *   - a fault inside an interrupt handler, the kernel, a critical section
 *     or before the scheduler starts.
 */
#ifndef FAULT_SUPERVISOR_H
#define FAULT_SUPERVISOR_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "cmsis_os2.h"

typedef enum {
    FAULT_NONE           = 0,
    FAULT_HARD           = 1,
    FAULT_MEMMANAGE      = 2,
    FAULT_BUS            = 3,
    FAULT_USAGE          = 4,
    FAULT_STACK_OVERFLOW = 5,
    FAULT_UNRECOVERABLE  = 6
} FaultType_t;

/* Survives resets (RAM .noinit): the last fault is still readable after an
 * OTA reset or a power-supply watchdog cycle. */
typedef struct {
    uint32_t magic;
    uint32_t total;          /* faults recorded since the record was created */
    uint32_t last_type;      /* FaultType_t */
    uint32_t last_uptime_s;
    uint32_t pc, lr, psr;
    uint32_t cfsr, hfsr, mmfar, bfar;
    char     task[configMAX_TASK_NAME_LEN];
} FaultRecord_t;

extern volatile FaultRecord_t g_fault_record;
extern volatile uint32_t      g_malloc_fail_count;
extern volatile uint32_t      g_task_restart_count;

/* Enable the separate MemManage/BusFault/UsageFault handlers. Call once at boot. */
void Supervisor_Init(void);

/* Creates a task and remembers how, so it can be re-created after a fault.
 * The handle is written to *handle_slot (also on every restart). */
osThreadId_t Supervisor_CreateTask(void (*fn)(void *), const osThreadAttr_t *attr,
                                   osThreadId_t *handle_slot);

/* Mutexes a dying task must release before it is deleted. */
void Supervisor_RegisterMutex(SemaphoreHandle_t m, uint8_t recursive);

/* Called periodically from a task (LogWriter): logs new faults and
 * re-creates killed tasks once their memory has been freed. */
void Supervisor_Service(void);

/* JSON fragment for /api/status: "health":{...}. Returns bytes written. */
int Supervisor_FormatHealthJSON(char *buf, int buflen);

/* FreeRTOS hooks forward here. */
void Supervisor_OnStackOverflow(TaskHandle_t task, const char *name);
void Supervisor_OnMallocFailed(void);

#ifdef KONTRX_FAULT_TEST
/* Test builds only: make the named task fault at its next test point. */
enum { FT_BAD_READ = 1, FT_UNDEF_INSTR = 2, FT_STACK_OVERFLOW = 3,
       FT_MALLOC_FAIL = 4, FT_MUTEX_HELD = 5 };
int  FaultTest_Arm(const char *task, uint8_t type);
void FaultTest_Point(void);
#define FAULT_TEST_POINT() FaultTest_Point()
#else
#define FAULT_TEST_POINT() ((void)0)
#endif

#endif /* FAULT_SUPERVISOR_H */
