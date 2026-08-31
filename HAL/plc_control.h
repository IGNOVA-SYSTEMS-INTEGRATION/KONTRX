#ifndef PLC_CONTROL_H
#define PLC_CONTROL_H

#include <stdint.h>

/* Modbus TCP outgoing control (opens socket 4, writes FC05, verifies, closes) */
uint8_t Modbus_TCP_WriteCoil(const char *ip, uint16_t port, uint8_t slave_id, uint16_t coil_addr, uint8_t state);

/* OPC UA Client outgoing control (stubbed option for future implementation) */
uint8_t OPC_UA_Client_WriteNode(const char *endpoint, uint16_t ns, const char *node_id, uint8_t state);

#endif // PLC_CONTROL_H
