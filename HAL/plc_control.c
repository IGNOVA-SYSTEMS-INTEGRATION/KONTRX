#include "plc_control.h"
#include "socket.h"
#include "wizchip_conf.h"
#include "cmsis_os2.h"
#include "freertos_tasks.h" // For Log_Event
#include <stdio.h>
#include <string.h>

#define MODBUS_CLIENT_SOCKET 4
#define OPCUA_CLIENT_SOCKET  5

static int ParseIP(const char *ip_str, uint8_t *ip_bytes) {
    int parts[4];
    if (sscanf(ip_str, "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]) == 4) {
        for (int i = 0; i < 4; i++) {
            if (parts[i] < 0 || parts[i] > 255) return 0;
            ip_bytes[i] = (uint8_t)parts[i];
        }
        return 1;
    }
    return 0;
}

uint8_t Modbus_TCP_WriteCoil(const char *ip, uint16_t port, uint8_t slave_id, uint16_t coil_addr, uint8_t state) {
    uint8_t dest_ip[4];
    if (!ParseIP(ip, dest_ip)) {
        char err[64];
        snprintf(err, sizeof(err), "Modbus TCP: Invalid target IP '%s'", ip);
        Log_Event("MODBUS", err);
        return 0;
    }

    // 1. Open Socket
    int8_t s = socket(MODBUS_CLIENT_SOCKET, Sn_MR_TCP, 0, 0);
    if (s != MODBUS_CLIENT_SOCKET) {
        Log_Event("MODBUS", "Modbus TCP: Failed to open socket 4");
        return 0;
    }

    // 2. Connect
    int8_t conn_res = connect(MODBUS_CLIENT_SOCKET, dest_ip, port);
    if (conn_res != SOCK_OK) {
        close(MODBUS_CLIENT_SOCKET);
        Log_Event("MODBUS", "Modbus TCP: connect call failed");
        return 0;
    }

    // Wait for connection to establish
    uint32_t start_ms = osKernelGetTickCount();
    while (getSn_SR(MODBUS_CLIENT_SOCKET) != SOCK_ESTABLISHED) {
        if (osKernelGetTickCount() - start_ms > 1000) { // 1 second connection timeout
            close(MODBUS_CLIENT_SOCKET);
            char warn[64];
            snprintf(warn, sizeof(warn), "PLC connection timeout at %s:%d", ip, port);
            Log_Event("MODBUS", warn);
            return 0;
        }
        osDelay(10);
    }

    // 3. Form and Send Modbus TCP Request
    uint8_t req[12] = {
        0x00, 0x01, // Transaction ID
        0x00, 0x00, // Protocol ID
        0x00, 0x06, // Length (6 bytes follow)
        slave_id,   // Unit ID (Slave ID)
        0x05,       // Function Code 5: Write Single Coil
        (uint8_t)(coil_addr >> 8), (uint8_t)(coil_addr & 0xFF), // Coil address
        (state ? 0xFF : 0x00), 0x00 // Value to write
    };

    send(MODBUS_CLIENT_SOCKET, req, 12);

    // 4. Wait for and verify response
    uint8_t resp[12];
    start_ms = osKernelGetTickCount();
    while (getSn_RX_RSR(MODBUS_CLIENT_SOCKET) < 12) {
        if (osKernelGetTickCount() - start_ms > 1000) { // 1 second read timeout
            disconnect(MODBUS_CLIENT_SOCKET);
            close(MODBUS_CLIENT_SOCKET);
            Log_Event("MODBUS", "PLC response timeout (no data received)");
            return 0;
        }
        if (getSn_SR(MODBUS_CLIENT_SOCKET) != SOCK_ESTABLISHED) {
            close(MODBUS_CLIENT_SOCKET);
            Log_Event("MODBUS", "PLC connection closed prematurely");
            return 0;
        }
        osDelay(10);
    }

    recv(MODBUS_CLIENT_SOCKET, resp, 12);

    // Clean up connection
    disconnect(MODBUS_CLIENT_SOCKET);
    close(MODBUS_CLIENT_SOCKET);

    // Verify Function Code and Address
    if (resp[7] == 0x05 && resp[8] == (uint8_t)(coil_addr >> 8) && resp[9] == (uint8_t)(coil_addr & 0xFF)) {
        return 1; // Write succeeded!
    }

    Log_Event("MODBUS", "Modbus TCP: Write verified failure (response mismatch)");
    return 0;
}

uint8_t OPC_UA_Client_WriteNode(const char *endpoint, uint16_t ns, const char *node_id, uint8_t state) {
    (void)endpoint;
    (void)ns;
    (void)node_id;
    (void)state;

    // Direct OPC UA Client is stubbed due to RAM limits (open62541 requires 40KB+ heap)
    // Recommend user to map node to Modbus TCP register or write via MQTT Bridge
    Log_Event("OTA", "[OPC UA] Direct Client disabled. Use Modbus TCP or MQTT Bridge.");
    return 0;
}
