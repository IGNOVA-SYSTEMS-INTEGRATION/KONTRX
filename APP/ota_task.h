#ifndef OTA_TASK_H
#define OTA_TASK_H

/**
 * @brief  OTA background task function.
 *         Runs at Low priority.
 *         Triggered by sem_ota_start from the HTTP server task.
 *         Verifies firmware CRC32, writes OTA_Meta to Sector 1,
 *         then triggers a software reset.
 */
void Task_OTAUpdate(void *arg);

#endif /* OTA_TASK_H */
