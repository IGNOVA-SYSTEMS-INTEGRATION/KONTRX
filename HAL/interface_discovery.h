#ifndef INTERFACE_DISCOVERY_H
#define INTERFACE_DISCOVERY_H

#include <stdint.h>

typedef enum {
    IF_STATUS_ACTIVE       = 0,  // Hardware detected and running
    IF_STATUS_READY        = 1,  // Supported in software, idle/standby
    IF_STATUS_WAITING_ASIC = 2,  // Requires external hardware ASIC module (Profibus, EtherCAT, PROFINET)
    IF_STATUS_DISABLED     = 3   // Disabled in configuration
} InterfaceStatus_t;

typedef struct {
    const char        *name;
    const char        *type;             // "Ethernet", "RS485", "Motion", "PWM", "Analog Out", "Fieldbus"
    const char        *hardware_info;    // e.g. "W5500 SPI2 @ 21MHz", "TIM1/TIM9/TIM3/TIM2"
    InterfaceStatus_t status;
    const char        *status_desc;
    uint8_t           channels_available;
    uint8_t           channels_used;
    uint8_t           enabled;
} Hardware_Interface_Desc_t;

#define TOTAL_SYSTEM_INTERFACES  12U

void Interface_Discovery_Init(void);
const Hardware_Interface_Desc_t* Interface_GetDesc(uint8_t idx);
uint8_t Interface_GetTotalCount(void);
void Interface_SetEnabled(uint8_t idx, uint8_t enabled);
void Interface_BuildDiscoveryJSON(char *buffer, uint32_t max_len);
uint32_t Interface_BuildArrayJSON(char *buffer, uint32_t max_len);

#endif // INTERFACE_DISCOVERY_H