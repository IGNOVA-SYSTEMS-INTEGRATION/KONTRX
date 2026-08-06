#ifndef HTTP_SERVER_TASK_H
#define HTTP_SERVER_TASK_H

#include <stdint.h>
#include "cmsis_os2.h"

/* OTA session flags — set by HTTP task, consumed by OTA task */
extern volatile uint8_t  ota_request_pending;
extern volatile uint32_t ota_content_length;
extern volatile uint8_t  ota_otp_validated;

/* Flag set by OTA task when write is complete */
extern volatile uint8_t  ota_write_done;
extern volatile uint8_t  ota_write_ok;

typedef struct {
    uint32_t fw_size;
    uint32_t computed_crc;
    uint32_t staged_sp;
    uint8_t  sp_valid;
    uint8_t  meta_ok;
    uint32_t meta_magic;
    uint32_t meta_status;
    uint32_t meta_size;
    uint32_t meta_crc32;
    uint8_t  write_ok;
    uint32_t step;
} OTA_Debug_t;

extern volatile OTA_Debug_t g_ota_debug;

/* Semaphore: HTTP task posts it when OTA body is fully written to staging */
extern osSemaphoreId_t   sem_ota_start;
extern osSemaphoreId_t   sem_ota_done;

/**
 * @brief  HTTP server FreeRTOS task function.
 *         Runs at Normal priority.
 *         Serves the SPA dashboard and REST APIs via W5500 socket 0 on port 80.
 */
void Task_HTTPServer(void *arg);

/**
 * @brief  Returns human-readable name of sensor type.
 */
const char *SensorTypeName(uint8_t type);

#endif /* HTTP_SERVER_TASK_H */
