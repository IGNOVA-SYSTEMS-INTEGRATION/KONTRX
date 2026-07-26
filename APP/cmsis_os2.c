#include "cmsis_os2.h"

/* Kernel Management */
osStatus_t osKernelInitialize(void) {
    return osOK;
}

osStatus_t osKernelStart(void) {
    vTaskStartScheduler();
    return osOK;
}

uint32_t osKernelGetTickCount(void) {
    return (uint32_t)xTaskGetTickCount();
}

/* Thread Management */
static UBaseType_t MapPriority(osPriority_t priority) {
    if (priority >= osPriorityRealtime) return 4;
    if (priority >= osPriorityHigh) return 3;
    if (priority >= osPriorityAboveNormal) return 3;
    if (priority >= osPriorityNormal) return 2;
    if (priority >= osPriorityBelowNormal) return 2;
    if (priority >= osPriorityLow) return 1;
    return 0; // Idle
}

osThreadId_t osThreadNew(void (*func)(void *argument), void *argument, const osThreadAttr_t *attr) {
    const char *name = (attr && attr->name) ? attr->name : "rtos_task";
    uint32_t stack_size = (attr && attr->stack_size > 0) ? (attr->stack_size / sizeof(StackType_t)) : configMINIMAL_STACK_SIZE;
    osPriority_t priority = (attr) ? attr->priority : osPriorityNormal;
    UBaseType_t freertos_priority = MapPriority(priority);

    TaskHandle_t handle = NULL;
    BaseType_t res = xTaskCreate(func, name, (uint16_t)stack_size, argument, freertos_priority, &handle);
    if (res == pdPASS) {
        return (osThreadId_t)handle;
    }
    return NULL;
}

const char *osThreadGetName(osThreadId_t thread_id) {
    if (thread_id == NULL) {
        thread_id = (osThreadId_t)xTaskGetCurrentTaskHandle();
    }
    return pcTaskGetName((TaskHandle_t)thread_id);
}

osStatus_t osThreadTerminate(osThreadId_t thread_id) {
    vTaskDelete((TaskHandle_t)thread_id);
    return osOK;
}

void osThreadYield(void) {
    taskYIELD();
}

/* Generic delay functions */
osStatus_t osDelay(uint32_t ticks) {
    vTaskDelay((TickType_t)ticks);
    return osOK;
}

osStatus_t osDelayUntil(uint32_t ticks) {
    TickType_t current = xTaskGetTickCount();
    if (ticks <= current) {
        return osOK;
    }
    TickType_t delay = ticks - current;
    vTaskDelay(delay);
    return osOK;
}

/* Mutex wrapper struct */
typedef struct {
    SemaphoreHandle_t handle;
    uint8_t is_recursive;
} os_mutex_t;

/* Mutex Management */
osMutexId_t osMutexNew(const osMutexAttr_t *attr) {
    os_mutex_t *mutex = (os_mutex_t *)pvPortMalloc(sizeof(os_mutex_t));
    if (!mutex) return NULL;
    
    mutex->is_recursive = (attr && (attr->attr_bits & osMutexRecursive)) ? 1 : 0;
    if (mutex->is_recursive) {
        mutex->handle = xSemaphoreCreateRecursiveMutex();
    } else {
        mutex->handle = xSemaphoreCreateMutex();
    }
    
    if (!mutex->handle) {
        vPortFree(mutex);
        return NULL;
    }
    return (osMutexId_t)mutex;
}

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout) {
    if (!mutex_id) return osErrorParameter;
    os_mutex_t *mutex = (os_mutex_t *)mutex_id;
    TickType_t ticks = (timeout == osWaitForever) ? portMAX_DELAY : (TickType_t)timeout;
    BaseType_t res;
    
    if (mutex->is_recursive) {
        res = xSemaphoreTakeRecursive(mutex->handle, ticks);
    } else {
        res = xSemaphoreTake(mutex->handle, ticks);
    }
    return (res == pdPASS) ? osOK : osErrorTimeout;
}

osStatus_t osMutexRelease(osMutexId_t mutex_id) {
    if (!mutex_id) return osErrorParameter;
    os_mutex_t *mutex = (os_mutex_t *)mutex_id;
    BaseType_t res;
    
    if (mutex->is_recursive) {
        res = xSemaphoreGiveRecursive(mutex->handle);
    } else {
        res = xSemaphoreGive(mutex->handle);
    }
    return (res == pdPASS) ? osOK : osError;
}

osStatus_t osMutexDelete(osMutexId_t mutex_id) {
    if (!mutex_id) return osErrorParameter;
    os_mutex_t *mutex = (os_mutex_t *)mutex_id;
    vSemaphoreDelete(mutex->handle);
    vPortFree(mutex);
    return osOK;
}

/* Semaphore Management */
osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count, const osSemaphoreAttr_t *attr) {
    (void)attr;
    return (osSemaphoreId_t)xSemaphoreCreateCounting(max_count, initial_count);
}

osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout) {
    if (!semaphore_id) return osErrorParameter;
    TickType_t ticks = (timeout == osWaitForever) ? portMAX_DELAY : (TickType_t)timeout;
    BaseType_t res = xSemaphoreTake((SemaphoreHandle_t)semaphore_id, ticks);
    return (res == pdPASS) ? osOK : osErrorTimeout;
}

osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id) {
    if (!semaphore_id) return osErrorParameter;
    BaseType_t res = xSemaphoreGive((SemaphoreHandle_t)semaphore_id);
    return (res == pdPASS) ? osOK : osError;
}

osStatus_t osSemaphoreDelete(osSemaphoreId_t semaphore_id) {
    if (!semaphore_id) return osErrorParameter;
    vSemaphoreDelete((SemaphoreHandle_t)semaphore_id);
    return osOK;
}
