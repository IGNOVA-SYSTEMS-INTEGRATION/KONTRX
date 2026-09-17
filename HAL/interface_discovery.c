/**
 * @file    interface_discovery.c
 * @brief   Kontrx Hardware Interface & Protocol Discovery Engine
 */

#include "interface_discovery.h"
#include "rcc_stm32.h"
#include "w5500.h"
#include <stdio.h>
#include <string.h>

static Hardware_Interface_Desc_t s_interfaces[TOTAL_SYSTEM_INTERFACES] = {
    {"Industrial Ethernet (W5500)", "Ethernet", "WIZnet W5500 (SPI2)", IF_STATUS_ACTIVE, "Online - 8 Hardware Sockets Active", 8, 3, 1},
    {"EtherCAT Slave", "Industrial Ethernet", "Beckhoff ET1100", IF_STATUS_WAITING_ASIC, "Expansion Slot Ready", 1, 0, 0},
    {"PROFINET IO Device", "Industrial Ethernet", "Hilscher netX", IF_STATUS_WAITING_ASIC, "Expansion Slot Ready", 1, 0, 0},
    {"RS485 Bus 1 (Modbus RTU / BACnet)", "RS485", "MAX485 on USART3", IF_STATUS_ACTIVE, "Active Master Polling (9600 8N1)", 1, 1, 1},
    {"RS485 Bus 2 (Auxiliary Network)", "RS485", "MAX485 on USART2", IF_STATUS_READY, "Standby Fieldbus", 1, 0, 1},
    {"Profibus DP", "Fieldbus", "VPC3+C / SPC3 ASIC", IF_STATUS_WAITING_ASIC, "Expansion Slot Ready", 1, 0, 0},
    {"Pulse Train Output (PTO Motion)", "Motion Control", "TIM1/9/3/2 + EXTI", IF_STATUS_ACTIVE, "4 Axes (Up to 100 kHz)", 4, 2, 1},
    {"PWM Outputs (VFD / Dimming)", "Proportional Out", "TIM4/10/11/14", IF_STATUS_ACTIVE, "7 Channels Hardware PWM", 7, 4, 1},
    {"Analog Current Output (4-20mA)", "Analog Output", "SPI3 DAC MCP4922", IF_STATUS_ACTIVE, "8 Channels 4-20mA Current", 8, 2, 1},
    {"Analog Voltage Output (0-10V)", "Analog Output", "PWM + 2-Pole LPF + OpAmp", IF_STATUS_ACTIVE, "2 Channels 0-10V DC Output", 2, 1, 1},
    {"Modbus TCP Server", "Industrial Protocol", "W5500 Socket 2 (Port 502)", IF_STATUS_ACTIVE, "Listening on Port 502", 1, 1, 1},
    {"BACnet/IP Gateway", "Building Protocol", "W5500 Socket 3 (Port 47808)", IF_STATUS_READY, "Ready on Port 47808", 1, 0, 1}
};

void Interface_Discovery_Init(void) {
    // Dynamic hardware probe for W5500
    uint8_t ver = getVERSIONR();
    if (ver == 0x04) {
        s_interfaces[0].status = IF_STATUS_ACTIVE;
        s_interfaces[0].status_desc = "Online - Hardware W5500 Verified (v4)";
    } else {
        s_interfaces[0].status = IF_STATUS_WAITING_ASIC;
        s_interfaces[0].status_desc = "Hardware fault / W5500 not detected";
    }
}

const Hardware_Interface_Desc_t* Interface_GetDesc(uint8_t idx) {
    if (idx < TOTAL_SYSTEM_INTERFACES) return &s_interfaces[idx];
    return 0;
}

uint8_t Interface_GetTotalCount(void) {
    return TOTAL_SYSTEM_INTERFACES;
}

static const char *s_default_descs[TOTAL_SYSTEM_INTERFACES] = {
    "Online - 8 Hardware Sockets Active",
    "Expansion Slot Ready",
    "Expansion Slot Ready",
    "Active Master Polling (9600 8N1)",
    "Standby Fieldbus",
    "Expansion Slot Ready",
    "4 Axes (Up to 100 kHz)",
    "7 Channels Hardware PWM",
    "8 Channels 4-20mA Current",
    "2 Channels 0-10V DC Output",
    "Listening on Port 502",
    "Ready on Port 47808"
};

void Interface_SetEnabled(uint8_t idx, uint8_t enabled) {
    if (idx < TOTAL_SYSTEM_INTERFACES) {
        s_interfaces[idx].enabled = enabled ? 1 : 0;
        if (!enabled) {
            s_interfaces[idx].status = IF_STATUS_DISABLED;
            s_interfaces[idx].status_desc = "Disabled by user";
        } else {
            s_interfaces[idx].status = (idx == 4 || idx == 11) ? IF_STATUS_READY : IF_STATUS_ACTIVE;
            s_interfaces[idx].status_desc = s_default_descs[idx];
        }
    }
}

uint32_t Interface_BuildArrayJSON(char *buffer, uint32_t max_len) {
    uint32_t len = 0;
    len += snprintf(buffer + len, max_len - len, "[");

    for (uint8_t i = 0; i < TOTAL_SYSTEM_INTERFACES; i++) {
        const char *st_str = "active";
        if (s_interfaces[i].status == IF_STATUS_READY) st_str = "ready";
        else if (s_interfaces[i].status == IF_STATUS_WAITING_ASIC) st_str = "waiting_asic";
        else if (s_interfaces[i].status == IF_STATUS_DISABLED) st_str = "disabled";

        len += snprintf(buffer + len, max_len - len,
            "%s{\"id\":%d,\"name\":\"%s\",\"type\":\"%s\",\"hardware\":\"%s\","
            "\"status\":\"%s\",\"desc\":\"%s\",\"channels_total\":%d,"
            "\"channels_used\":%d,\"enabled\":%s}",
            (i > 0) ? "," : "",
            i,
            s_interfaces[i].name,
            s_interfaces[i].type,
            s_interfaces[i].hardware_info,
            st_str,
            s_interfaces[i].status_desc,
            s_interfaces[i].channels_available,
            s_interfaces[i].channels_used,
            s_interfaces[i].enabled ? "true" : "false");

        if (len >= max_len - 32) break;
    }

    len += snprintf(buffer + len, max_len - len, "]");
    return len;
}

void Interface_BuildDiscoveryJSON(char *buffer, uint32_t max_len) {
    uint32_t len = 0;
    len += snprintf(buffer + len, max_len - len,
        "{\"cpu_clock_mhz\":%lu,\"clock_source\":\"%s\",\"interfaces\":",
        (unsigned long)(RCC_GetSysClockFreq() / 1000000UL),
        RCC_IsHSEUsed() ? "HSE PLL 168MHz" : "HSI PLL 168MHz");

    len += Interface_BuildArrayJSON(buffer + len, max_len - len);
    snprintf(buffer + len, max_len - len, "}");
}