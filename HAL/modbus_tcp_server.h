#ifndef MODBUS_TCP_SERVER_H
#define MODBUS_TCP_SERVER_H

#include <stdint.h>

#define MODBUS_TCP_SERVER_SOCKET  2U
#define MODBUS_TCP_SERVER_PORT    502U

/**
 * @brief Initialize Modbus TCP Server on W5500 Socket 2
 */
void Modbus_TCP_Server_Init(void);

/**
 * @brief Non-blocking poll handler called inside background RTOS loop
 */
void Modbus_TCP_Server_Poll(void);

#endif // MODBUS_TCP_SERVER_H