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

/* Semaphore: HTTP task posts it when OTA body is fully written to staging */
extern osSemaphoreId_t   sem_ota_start;
extern osSemaphoreId_t   sem_ota_done;

/**
 * @brief  HTTP server FreeRTOS task function.
 *         Runs at Normal priority.
 *         Serves the SPA dashboard and REST APIs via W5500 socket 0 on port 80.
 */
void Task_HTTPServer(void *arg);

#endif /* HTTP_SERVER_TASK_H */
