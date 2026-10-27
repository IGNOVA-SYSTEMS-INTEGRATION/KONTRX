/**
 * @file    fault_supervisor.c
 * @brief   Task-level fault isolation (see fault_supervisor.h).
 *
 * How a faulting task is removed:
 *   1. The fault handler (or the stack-overflow hook) rewrites the task's
 *      stacked return address so that, when it next runs, it executes
 *      Fault_TaskExit() instead of the faulting code.
 *   2. Fault_TaskExit() runs in the dying task: it gives back every
 *      registered mutex it still holds (otherwise other tasks would block on
 *      them forever) and deletes itself.
 *   3. Supervisor_Service() (LogWriter task) logs the fault and re-creates
 *      the task once the idle task has freed its stack.
 * A task that faults 5 times within 60 s is left stopped (logged).
 */
#include "fault_supervisor.h"
#include "freertos_tasks.h"
#include "safe_fmt.h"
#include <string.h>
#include <stdio.h>

#define FAULT_RECORD_MAGIC   0x4B464C54U   /* "KFLT" */
#define SUP_MAX_TASKS        10U
#define SUP_MAX_MUTEXES      12U
#define SUP_RESTART_DELAY_MS 200U          /* let the idle task free the old stack */
#define SUP_STORM_WINDOW_MS  60000U
#define SUP_STORM_MAX        5U

#define SCB_SHCSR (*(volatile uint32_t *)0xE000ED24U)
#define SCB_CFSR  (*(volatile uint32_t *)0xE000ED28U)
#define SCB_HFSR  (*(volatile uint32_t *)0xE000ED2CU)
#define SCB_MMFAR (*(volatile uint32_t *)0xE000ED34U)
#define SCB_BFAR  (*(volatile uint32_t *)0xE000ED38U)
#define SHCSR_FAULTS_ENA ((1U << 16) | (1U << 17) | (1U << 18))

typedef enum { SUP_RUNNING = 0, SUP_KILLING, SUP_DEAD, SUP_DISABLED } SupState_t;

typedef struct {
    void          (*fn)(void *);
    osThreadAttr_t  attr;
    osThreadId_t   *slot;
    TaskHandle_t    handle;
    volatile uint8_t state;
    uint8_t         storm_count;
    uint32_t        storm_start;
    uint32_t        dead_tick;
    uint32_t        restarts;
} SupTask_t;

typedef struct {
    SemaphoreHandle_t h;
    uint8_t           recursive;
} SupMutex_t;

volatile FaultRecord_t g_fault_record __attribute__((section(".noinit")));

/* The last few faults, so two faults between supervisor passes are both
 * logged (g_fault_record only holds the latest). Indexed by total % size. */
#define FAULT_RING_SIZE 4U
typedef struct {
    uint32_t type, pc, cfsr;
    char     task[configMAX_TASK_NAME_LEN];
} FaultBrief_t;
static volatile FaultBrief_t s_fault_ring[FAULT_RING_SIZE] __attribute__((section(".noinit")));
volatile uint32_t      g_malloc_fail_count  = 0;
volatile uint32_t      g_task_restart_count = 0;

static SupTask_t  s_tasks[SUP_MAX_TASKS];
static uint8_t    s_task_count;
static SupMutex_t s_mutexes[SUP_MAX_MUTEXES];
static uint8_t    s_mutex_count;
static volatile uint8_t s_service_busy;

/* Reached only from the naked fault entries' inline asm: `used` keeps LTO
 * from dropping it. */
void Fault_Dispatch(uint32_t *frame, uint32_t exc_return, uint32_t type) __attribute__((used));
static void Fault_TaskExit(void) __attribute__((noreturn, used));

void Supervisor_Init(void) {
    if (g_fault_record.magic != FAULT_RECORD_MAGIC) {
        memset((void *)&g_fault_record, 0, sizeof(g_fault_record));
        g_fault_record.magic = FAULT_RECORD_MAGIC;
    }
    SCB_SHCSR |= SHCSR_FAULTS_ENA;
}

static SupTask_t *Find_Task(TaskHandle_t h) {
    for (uint8_t i = 0; i < s_task_count; i++) {
        if (s_tasks[i].handle == h) return &s_tasks[i];
    }
    return NULL;
}

osThreadId_t Supervisor_CreateTask(void (*fn)(void *), const osThreadAttr_t *attr,
                                   osThreadId_t *handle_slot) {
    osThreadId_t h = osThreadNew(fn, NULL, attr);
    if (h && s_task_count < SUP_MAX_TASKS) {
        SupTask_t *t = &s_tasks[s_task_count++];
        memset(t, 0, sizeof(*t));
        t->fn     = fn;
        t->attr   = *attr;
        t->slot   = handle_slot;
        t->handle = (TaskHandle_t)h;
        t->state  = SUP_RUNNING;
    }
    if (handle_slot) *handle_slot = h;
    return h;
}

void Supervisor_RegisterMutex(SemaphoreHandle_t m, uint8_t recursive) {
    if (!m) return;
    taskENTER_CRITICAL();
    if (s_mutex_count < SUP_MAX_MUTEXES) {
        s_mutexes[s_mutex_count].h = m;
        s_mutexes[s_mutex_count].recursive = recursive;
        s_mutex_count++;
    }
    taskEXIT_CRITICAL();
}

static void Record_Fault(uint32_t type, const char *task, uint32_t pc, uint32_t lr, uint32_t psr) {
    g_fault_record.total++;
    g_fault_record.last_type     = type;
    g_fault_record.last_uptime_s = g_uptime_seconds;
    g_fault_record.pc    = pc;
    g_fault_record.lr    = lr;
    g_fault_record.psr   = psr;
    g_fault_record.cfsr  = SCB_CFSR;
    g_fault_record.hfsr  = SCB_HFSR;
    g_fault_record.mmfar = SCB_MMFAR;
    g_fault_record.bfar  = SCB_BFAR;
    for (uint32_t i = 0; i < sizeof(g_fault_record.task); i++) {
        char c = (task && task[i]) ? task[i] : '\0';
        g_fault_record.task[i] = c;
        if (c == '\0') break;
    }
    g_fault_record.task[sizeof(g_fault_record.task) - 1U] = '\0';

    volatile FaultBrief_t *b = &s_fault_ring[g_fault_record.total % FAULT_RING_SIZE];
    b->type = type;
    b->pc   = pc;
    b->cfsr = g_fault_record.cfsr;
    for (uint32_t i = 0; i < sizeof(b->task); i++) b->task[i] = g_fault_record.task[i];
}

/* No safe way to continue: keep outputs as they are and stop. */
static void Fault_Unrecoverable(void) __attribute__((noreturn));
static void Fault_Unrecoverable(void) {
    g_fault_record.last_type = FAULT_UNRECOVERABLE;
    __asm volatile ("cpsid i" ::: "memory");
    for (;;) { }
}

/* Points a stacked exception frame at Fault_TaskExit (Thumb, no IT state). */
static void Redirect_Frame(uint32_t *hw_frame) {
    hw_frame[6] = ((uint32_t)&Fault_TaskExit) & ~1U;
    hw_frame[7] = 0x01000000U;
}

void Fault_Dispatch(uint32_t *frame, uint32_t exc_return, uint32_t type) {
    uint32_t basepri;
    __asm volatile ("mrs %0, basepri" : "=r" (basepri));

    uint8_t thread_psp = ((exc_return & 0xCU) == 0xCU);   /* thread mode, PSP */
    uint8_t sched_ok   = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    TaskHandle_t cur   = sched_ok ? xTaskGetCurrentTaskHandle() : NULL;
    SupTask_t *t       = cur ? Find_Task(cur) : NULL;

    Record_Fault(type, cur ? pcTaskGetName(cur) : "-",
                 thread_psp ? frame[6] : 0U, thread_psp ? frame[5] : 0U,
                 thread_psp ? frame[7] : 0U);

    uint32_t exit_lo = ((uint32_t)&Fault_TaskExit) & ~1U;
    uint8_t in_exit  = thread_psp && (frame[6] >= exit_lo) && (frame[6] < exit_lo + 0x100U);

    if (!thread_psp || basepri != 0U || !t || t->state != SUP_RUNNING || in_exit) {
        Fault_Unrecoverable();
    }

    SCB_CFSR = SCB_CFSR;   /* write-1-to-clear */
    SCB_HFSR = SCB_HFSR;
    t->state = SUP_KILLING;
    Redirect_Frame(frame);
}

#define FAULT_ENTRY(name, code)                                   \
    __attribute__((naked)) void name(void) {                      \
        __asm volatile (                                          \
            "tst lr, #4        \n"                                \
            "ite eq            \n"                                \
            "mrseq r0, msp     \n"                                \
            "mrsne r0, psp     \n"                                \
            "mov r1, lr        \n"                                \
            "mov r2, #" #code "\n"                                \
            "b Fault_Dispatch  \n");                              \
    }

FAULT_ENTRY(HardFault_Handler, 1)
FAULT_ENTRY(MemManage_Handler, 2)
FAULT_ENTRY(BusFault_Handler, 3)
FAULT_ENTRY(UsageFault_Handler, 4)

/* Runs in the dying task's own context. */
static void Fault_TaskExit(void) {
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (uint8_t i = 0; i < s_mutex_count; i++) {
        for (uint8_t guard = 0; guard < 64U && xSemaphoreGetMutexHolder(s_mutexes[i].h) == self; guard++) {
            if (s_mutexes[i].recursive) xSemaphoreGiveRecursive(s_mutexes[i].h);
            else xSemaphoreGive(s_mutexes[i].h);
        }
    }
    SupTask_t *t = Find_Task(self);
    if (t) {
        t->dead_tick = xTaskGetTickCount();
        t->state = SUP_DEAD;
    }
    vTaskDelete(NULL);
    for (;;) { }
}

void Supervisor_OnStackOverflow(TaskHandle_t task, const char *name) {
    SupTask_t *t = Find_Task(task);
    if (t && t->state != SUP_RUNNING) return;   /* already being removed */

    Record_Fault(FAULT_STACK_OVERFLOW, name, 0U, 0U, 0U);
    if (!t) Fault_Unrecoverable();               /* idle / timer task */

    /* The task's context was just saved by PendSV: r4-r11, EXC_RETURN,
     * then s16-s31 when it used the FPU, then the hardware frame. */
    uint32_t *top = *(uint32_t **)task;
    uint32_t addr = (uint32_t)top;
    uint8_t in_ram = (addr >= 0x20000000U && addr < 0x20020000U) ||
                     (addr >= 0x10000000U && addr < 0x10010000U);
    if (!in_ram) Fault_Unrecoverable();
    uint32_t exc = top[8];
    uint32_t *hw = top + 9U + (((exc & 0x10U) == 0U) ? 16U : 0U);
    t->state = SUP_KILLING;
    Redirect_Frame(hw);
}

void Supervisor_OnMallocFailed(void) {
    g_malloc_fail_count++;
}

static const char *Fault_Name(uint32_t type) {
    switch (type) {
        case FAULT_HARD:           return "HardFault";
        case FAULT_MEMMANAGE:      return "MemManage";
        case FAULT_BUS:            return "BusFault";
        case FAULT_USAGE:          return "UsageFault";
        case FAULT_STACK_OVERFLOW: return "StackOverflow";
        case FAULT_UNRECOVERABLE:  return "Unrecoverable";
        default:                   return "None";
    }
}

void Supervisor_Service(void) {
    static uint32_t s_logged_total = 0xFFFFFFFFU;
    static uint32_t s_logged_malloc = 0;
    static uint32_t s_malloc_log_tick = 0;
    char msg[64];

    taskENTER_CRITICAL();
    uint8_t busy = s_service_busy;
    s_service_busy = 1;
    taskEXIT_CRITICAL();
    if (busy) return;

    if (s_logged_total == 0xFFFFFFFFU) {
        /* First pass after boot: report a fault recorded before the reset. */
        s_logged_total = 0;
        if (g_fault_record.last_type == FAULT_UNRECOVERABLE) {
            snprintf(msg, sizeof(msg), "Halted before reset: %s pc=0x%08lX",
                     g_fault_record.task, (unsigned long)g_fault_record.pc);
            Log_Event("SYS", msg);
        }
        s_logged_total = g_fault_record.total;
    }
    uint32_t total = g_fault_record.total;
    if (total - s_logged_total > FAULT_RING_SIZE) {
        snprintf(msg, sizeof(msg), "%lu faults not logged (too many at once)",
                 (unsigned long)(total - s_logged_total - FAULT_RING_SIZE));
        Log_Event("SYS", msg);
        s_logged_total = total - FAULT_RING_SIZE;
    }
    while (s_logged_total != total) {
        s_logged_total++;
        const volatile FaultBrief_t *b = &s_fault_ring[s_logged_total % FAULT_RING_SIZE];
        snprintf(msg, sizeof(msg), "%s in %.15s pc=0x%08lX cfsr=0x%08lX",
                 Fault_Name(b->type), (const char *)b->task,
                 (unsigned long)b->pc, (unsigned long)b->cfsr);
        Log_Event("SYS", msg);
    }

    uint32_t now = xTaskGetTickCount();
    if (g_malloc_fail_count != s_logged_malloc &&
        (now - s_malloc_log_tick) >= pdMS_TO_TICKS(10000)) {
        snprintf(msg, sizeof(msg), "Heap allocation failed %lu times (free %u B)",
                 (unsigned long)(g_malloc_fail_count - s_logged_malloc),
                 (unsigned)xPortGetFreeHeapSize());
        Log_Event("SYS", msg);
        s_logged_malloc = g_malloc_fail_count;
        s_malloc_log_tick = now;
    }

    for (uint8_t i = 0; i < s_task_count; i++) {
        SupTask_t *t = &s_tasks[i];
        if (t->state != SUP_DEAD) continue;
        if ((now - t->dead_tick) < pdMS_TO_TICKS(SUP_RESTART_DELAY_MS)) continue;

        if ((now - t->storm_start) >= pdMS_TO_TICKS(SUP_STORM_WINDOW_MS)) {
            t->storm_start = now;
            t->storm_count = 0;
        }
        if (t->storm_count >= SUP_STORM_MAX) {
            t->state = SUP_DISABLED;
            snprintf(msg, sizeof(msg), "Task %s stopped: %u faults in 60 s",
                     t->attr.name ? t->attr.name : "?", (unsigned)SUP_STORM_MAX);
            Log_Event("SYS", msg);
            continue;
        }
        if (xPortGetFreeHeapSize() < (t->attr.stack_size + 512U)) continue;

        osThreadId_t h = osThreadNew(t->fn, NULL, &t->attr);
        if (!h) continue;
        t->handle = (TaskHandle_t)h;
        t->state  = SUP_RUNNING;
        t->storm_count++;
        t->restarts++;
        g_task_restart_count++;
        if (t->slot) *t->slot = h;
        snprintf(msg, sizeof(msg), "Task %s restarted after fault (#%lu)",
                 t->attr.name ? t->attr.name : "?", (unsigned long)t->restarts);
        Log_Event("SYS", msg);
    }

    s_service_busy = 0;
}

int Supervisor_FormatHealthJSON(char *buf, int buflen) {
    size_t cap = (buflen > 0) ? (size_t)buflen : 0U;
    size_t pos = 0;
    pos = sfmt_append(buf, cap, pos,
        "\"health\":{\"heap_free\":%u,\"heap_min\":%u,\"malloc_fail\":%lu,"
        "\"faults\":%lu,\"restarts\":%lu,\"log_dropped\":%lu,"
        "\"last_fault\":{\"type\":\"%s\",\"task\":\"%s\",\"pc\":\"0x%08lX\",\"cfsr\":\"0x%08lX\",\"up_s\":%lu},"
        "\"tasks\":[",
        (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize(),
        (unsigned long)g_malloc_fail_count, (unsigned long)g_fault_record.total,
        (unsigned long)g_task_restart_count, (unsigned long)g_log_dropped,
        Fault_Name(g_fault_record.last_type), g_fault_record.task,
        (unsigned long)g_fault_record.pc, (unsigned long)g_fault_record.cfsr,
        (unsigned long)g_fault_record.last_uptime_s);
    for (uint8_t i = 0; i < s_task_count; i++) {
        const SupTask_t *t = &s_tasks[i];
        unsigned long free_b = (t->state == SUP_RUNNING)
            ? (unsigned long)uxTaskGetStackHighWaterMark(t->handle) * sizeof(StackType_t) : 0UL;
        pos = sfmt_append(buf, cap, pos, "%s{\"n\":\"%s\",\"stack\":%lu,\"min_free\":%lu,\"state\":%u,\"restarts\":%lu}",
                          i ? "," : "", t->attr.name ? t->attr.name : "?",
                          (unsigned long)t->attr.stack_size, free_b,
                          (unsigned)t->state, (unsigned long)t->restarts);
    }
    pos = sfmt_append(buf, cap, pos, "]}");
    return (int)pos;
}

#ifdef KONTRX_FAULT_TEST
/* ======================================================================
 *  Fault injection (test builds only, -DKONTRX_FAULT_TEST=ON)
 *
 *  FaultTest_Arm() picks a task and a fault; the fault then happens inside
 *  that task, at its next FAULT_TEST_POINT(), so the real recovery path runs.
 * ====================================================================== */
#include "modbus_dma.h"

static volatile uint8_t s_ft_type;
static char s_ft_task[configMAX_TASK_NAME_LEN];

int FaultTest_Arm(const char *task, uint8_t type) {
    if (!task || type < FT_BAD_READ || type > FT_MUTEX_HELD) return 0;
    taskENTER_CRITICAL();
    strncpy(s_ft_task, task, sizeof(s_ft_task) - 1U);
    s_ft_task[sizeof(s_ft_task) - 1U] = '\0';
    s_ft_type = type;
    taskEXIT_CRITICAL();
    return 1;
}

/* Overwrites the stack-bottom pattern that configCHECK_FOR_STACK_OVERFLOW=2
 * checks, so the kernel reports an overflow at the next context switch
 * without anything past the stack actually being written. */
static void FaultTest_FakeStackOverflow(void) {
    uint8_t *tcb = (uint8_t *)xTaskGetCurrentTaskHandle();
    size_t off = sizeof(void *) + 2U * sizeof(ListItem_t) + sizeof(UBaseType_t);
    uint32_t *stack_base = *(uint32_t **)(tcb + off);
    for (uint8_t i = 0; i < 4U; i++) stack_base[i] = 0U;
    taskYIELD();
}

void FaultTest_Point(void) {
    if (s_ft_type == 0U) return;
    if (strncmp(pcTaskGetName(NULL), s_ft_task, sizeof(s_ft_task)) != 0) return;

    taskENTER_CRITICAL();
    uint8_t type = s_ft_type;
    s_ft_type = 0;
    taskEXIT_CRITICAL();

    switch (type) {
        case FT_BAD_READ: {                 /* BusFault: unmapped address */
            volatile uint32_t v = *(volatile uint32_t *)0xA0001000U;
            (void)v;
            break;
        }
        case FT_UNDEF_INSTR:                /* UsageFault: undefined instruction */
            __asm volatile ("udf #0");
            break;
        case FT_STACK_OVERFLOW:
            FaultTest_FakeStackOverflow();
            break;
        case FT_MALLOC_FAIL: {              /* request larger than the whole heap */
            void *p = pvPortMalloc(configTOTAL_HEAP_SIZE * 2U);
            if (p) vPortFree(p);
            break;
        }
        case FT_MUTEX_HELD:                 /* fault while holding configMutex */
            if (configMutex) osMutexAcquire(configMutex, osWaitForever);
            {
                volatile uint32_t v = *(volatile uint32_t *)0xA0001000U;
                (void)v;
            }
            break;
        default:
            break;
    }
}
#endif /* KONTRX_FAULT_TEST */
