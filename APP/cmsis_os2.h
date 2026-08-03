#ifndef CMSIS_OS2_H
#define CMSIS_OS2_H

#include <stdint.h>
#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status code values returned by CMSIS-OS functions */
typedef enum {
    osOK                    =  0,   ///< Operation completed successfully.
    osError                 = -1,   ///< Unspecified error.
    osErrorTimeout          = -2,   ///< Operation timed out.
    osErrorResource         = -3,   ///< Resource not available.
    osErrorParameter        = -4,   ///< Parameter error.
    osErrorNoMemory         = -5,   ///< Out of memory.
    osErrorISR              = -6,   ///< Cannot be called from Interrupt Service Routine.
    osStatusReserved        = 0x7FFFFFFF ///< Prevents enum down-size compiler optimization.
} osStatus_t;

/* Priority values */
typedef enum {
    osPriorityNone          =  0,   ///< No priority (not initialized)
    osPriorityIdle          =  1,   ///< Reserved for Idle thread
    osPriorityLow           =  8,   ///< Priority: low
    osPriorityBelowNormal   = 16,   ///< Priority: below normal
    osPriorityNormal        = 24,   ///< Priority: normal (default)
    osPriorityAboveNormal   = 32,   ///< Priority: above normal
    osPriorityHigh          = 40,   ///< Priority: high
    osPriorityRealtime      = 48,   ///< Priority: realtime
    osPriorityISR           = 56,   ///< Reserved for ISR deferred processing
    osPriorityReserved      = 0x7FFFFFFF ///< Prevents enum down-size compiler optimization.
} osPriority_t;

/* Timeout value definition */
#define osWaitForever       0xFFFFFFFFU ///< Wait forever timeout value.

/* Kernel state values */
typedef enum {
    osKernelInactive    = 0,   ///< Kernel not initialized.
    osKernelReady       = 1,   ///< Kernel initialized and ready.
    osKernelRunning     = 2,   ///< Kernel running.
    osKernelLocked      = 3,   ///< Kernel locked.
    osKernelSuspended   = 4,   ///< Kernel suspended.
    osKernelError       = -1,  ///< Kernel error state.
    osKernelReserved    = 0x7FFFFFFF ///< Prevents enum down-size compiler optimization.
} osKernelState_t;

/* Thread ID placeholder */
typedef TaskHandle_t osThreadId_t;

/* Mutex ID placeholder */
typedef SemaphoreHandle_t osMutexId_t;

/* Semaphore ID placeholder */
typedef SemaphoreHandle_t osSemaphoreId_t;

/* Attributes structure for thread */
typedef struct {
    const char                   *name;         ///< name of the thread
    uint32_t                      attr_bits;    ///< attribute bits
    void                         *cb_mem;       ///< memory for control block
    uint32_t                      cb_size;      ///< size of control block
    void                         *stack_mem;    ///< memory for stack
    uint32_t                      stack_size;   ///< size of stack in bytes
    osPriority_t                  priority;     ///< initial thread priority
    uint32_t                      tz_module;    ///< TrustZone module identifier
    uint32_t                      reserved;     ///< reserved (must be 0)
} osThreadAttr_t;

/* Attributes structure for mutex */
typedef struct {
    const char                   *name;         ///< name of the mutex
    uint32_t                      attr_bits;    ///< attribute bits
    void                         *cb_mem;       ///< memory for control block
    uint32_t                      cb_size;      ///< size of control block
} osMutexAttr_t;

/* Attributes structure for semaphore */
typedef struct {
    const char                   *name;         ///< name of the semaphore
    uint32_t                      attr_bits;    ///< attribute bits
    void                         *cb_mem;       ///< memory for control block
    uint32_t                      cb_size;      ///< size of control block
} osSemaphoreAttr_t;

/* Mutex Attribute flags */
#define osMutexRecursive    0x00000001U ///< Recursive mutex.
#define osMutexRobust       0x00000002U ///< Robust mutex (unused).

/* Kernel Management */
osStatus_t osKernelInitialize(void);
osStatus_t osKernelStart(void);
osKernelState_t osKernelGetState(void);
uint32_t osKernelGetTickCount(void);

/* Thread Management */
osThreadId_t osThreadNew(void (*func)(void *argument), void *argument, const osThreadAttr_t *attr);
const char *osThreadGetName(osThreadId_t thread_id);
osStatus_t osThreadTerminate(osThreadId_t thread_id);
void osThreadYield(void);

/* Generic delay functions */
osStatus_t osDelay(uint32_t ticks);
osStatus_t osDelayUntil(uint32_t ticks);

/* Mutex Management */
osMutexId_t osMutexNew(const osMutexAttr_t *attr);
osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout);
osStatus_t osMutexRelease(osMutexId_t mutex_id);
osStatus_t osMutexDelete(osMutexId_t mutex_id);

/* Semaphore Management */
osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count, const osSemaphoreAttr_t *attr);
osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout);
osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id);
osStatus_t osSemaphoreDelete(osSemaphoreId_t semaphore_id);

#ifdef __cplusplus
}
#endif

#endif /* CMSIS_OS2_H */
