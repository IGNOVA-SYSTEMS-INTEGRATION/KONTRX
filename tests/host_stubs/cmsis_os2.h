/* Host-test stand-in for APP/cmsis_os2.h: only the types the firmware
 * headers mention, so pure-logic modules compile on a PC. */
#ifndef CMSIS_OS2_HOST_STUB_H
#define CMSIS_OS2_HOST_STUB_H
#include <stdint.h>
#include <stddef.h>
typedef void *osThreadId_t;
typedef void *osMutexId_t;
typedef void *osSemaphoreId_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
#endif
