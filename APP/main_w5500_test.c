#include "stm32f407_regs.h"
#include "gpio_stm32.h"
#include "uart_stm32.h"
#include "spi_stm32.h"
#include "led.h"
#include "wizchip_conf.h"
#include "socket.h"
#include "dhcp.h"
#include <stdio.h>
#include <string.h>

// Override _write so standard printf() uses our Debug USART (USART1)
int _write(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) {
        UART_Debug_SendByte((uint8_t)ptr[i]);
    }
    return len;
}

// Simple delay
void Delay_ms(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 3200; j++) {
            __asm__("nop");
        }
    }
}

// SPI Wrapper Functions for WIZnet ioLibrary
uint8_t W5500_SPI_ReadByte(void) {
    return SPI2_ReadWriteByte(0xFF);
}

void W5500_SPI_WriteByte(uint8_t data) {
    SPI2_ReadWriteByte(data);
}

// Web Server Buffer
uint8_t rx_buf[2048];
uint8_t tx_buf[2048];

const char *html_page = 
"HTTP/1.1 200 OK\r\n"
"Content-Type: text/html\r\n"
"Connection: close\r\n"
"\r\n"
"<!DOCTYPE html>"
"<html>"
"<head><title>STM32 W5500 Web Server</title>"
"<style>"
"body { font-family: Arial; text-align: center; margin-top: 50px; }"
"button { font-size: 20px; padding: 10px 20px; margin: 10px; cursor: pointer; border-radius: 5px; }"
".on { background-color: #4CAF50; color: white; }"
".off { background-color: #f44336; color: white; }"
"</style></head>"
"<body>"
"<h1>STM32F407 W5500 Bare-Metal Test</h1>"
"<h2>Onboard LED (PA6) Control</h2>"
"<a href=\"/led_on\"><button class=\"on\">Turn LED ON</button></a>"
"<a href=\"/led_off\"><button class=\"off\">Turn LED OFF</button></a>"
"</body>"
"</html>";

int main(void) {
    // 1. Hardware Initialization
    GPIO_Init_USART1_Pins();
    UART_Debug_Init();
    LED_Init();

    // Init SPI2 and W5500 Pins
    GPIO_Init_W5500_Pins();
    SPI2_Init();

    printf("\r\n=== STM32F407 W5500 Test App ===\r\n");

    // 2. Hardware Reset W5500
    printf("Hardware Resetting W5500...\r\n");
    W5500_Hardware_Reset();

    // 3. Register Callbacks to WIZnet ioLibrary
    reg_wizchip_cs_cbfunc(W5500_CS_Select, W5500_CS_Deselect);
    reg_wizchip_spi_cbfunc(W5500_SPI_ReadByte, W5500_SPI_WriteByte);

    // 4. Initialize W5500 Buffers (2KB per socket)
    uint8_t memsize[2][8] = { {2,2,2,2,2,2,2,2}, {2,2,2,2,2,2,2,2} };
    if(ctlwizchip(CW_INIT_WIZCHIP, (void*)memsize) == -1) {
        printf("WIZCHIP Initialization Failed.\r\n");
        while(1);
    }

    // 5. Initialize DHCP and Request IP
    uint8_t mac[6] = {0x00, 0x08, 0xDC, 0x11, 0x22, 0x33};
    setSHAR(mac); // Set MAC address to hardware for DHCP

    uint8_t dhcp_buffer[1024];
    DHCP_init(1, dhcp_buffer); // Use Socket 1 for DHCP

    printf("Requesting IP via DHCP...\r\n");

    uint32_t dhcp_tick = 0;
    while (1) {
        uint8_t ret = DHCP_run();
        if (ret == DHCP_IP_LEASED || ret == DHCP_IP_ASSIGN) {
            printf("DHCP Success!\r\n");
            break;
        } else if (ret == DHCP_FAILED) {
            printf("DHCP Failed. Retrying...\r\n");
            DHCP_stop();
            DHCP_init(1, dhcp_buffer);
        }
        
        Delay_ms(1);
        dhcp_tick++;
        if (dhcp_tick >= 1000) {
            DHCP_time_handler(); // Called every 1s
            dhcp_tick = 0;
        }
    }

    wiz_NetInfo net_info;
    memcpy(net_info.mac, mac, 6);
    getIPfromDHCP(net_info.ip);
    getGWfromDHCP(net_info.gw);
    getSNfromDHCP(net_info.sn);
    getDNSfromDHCP(net_info.dns);
    net_info.dhcp = NETINFO_DHCP;
    
    ctlnetwork(CN_SET_NETINFO, (void*)&net_info);

    // Print Network Info
    ctlnetwork(CN_GET_NETINFO, (void*)&net_info);
    printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n", net_info.mac[0], net_info.mac[1], net_info.mac[2], net_info.mac[3], net_info.mac[4], net_info.mac[5]);
    printf("IP:  %d.%d.%d.%d\r\n", net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);
    printf("SN:  %d.%d.%d.%d\r\n", net_info.sn[0], net_info.sn[1], net_info.sn[2], net_info.sn[3]);
    printf("GW:  %d.%d.%d.%d\r\n", net_info.gw[0], net_info.gw[1], net_info.gw[2], net_info.gw[3]);

    // 6. Check Hardware PHY Link
    if (wizphy_getphylink() == PHY_LINK_OFF) {
        printf("Warning: LAN Cable is Disconnected! Waiting for connection...\r\n");
        while (wizphy_getphylink() == PHY_LINK_OFF) {
            Delay_ms(1000);
        }
    }
    printf("LAN Cable Connected!\r\n");
    printf("Web Server Started on Port 80.\r\n");

    // 7. Web Server Main Loop
    uint8_t sn = 0; // Use Socket 0
    uint16_t port = 80;

    while (1) {
        switch (getSn_SR(sn)) {
            case SOCK_ESTABLISHED:
                if (getSn_IR(sn) & Sn_IR_CON) {
                    setSn_IR(sn, Sn_IR_CON); // Clear interrupt
                    printf("Client Connected!\r\n");
                }
                uint16_t size = getSn_RX_RSR(sn);
                if (size > 0) {
                    if (size > sizeof(rx_buf)) size = sizeof(rx_buf) - 1;
                    recv(sn, rx_buf, size);
                    rx_buf[size] = '\0'; // Null terminate for string functions
                    
                    // Parse HTTP Request
                    if (strstr((char*)rx_buf, "GET /led_on")) {
                        printf("Action: Turn LED ON\r\n");
                        LED_On();
                    } else if (strstr((char*)rx_buf, "GET /led_off")) {
                        printf("Action: Turn LED OFF\r\n");
                        LED_Off();
                    }

                    // Send HTTP Response
                    send(sn, (uint8_t*)html_page, strlen(html_page));
                    disconnect(sn);
                }
                break;
                
            case SOCK_CLOSE_WAIT:
                disconnect(sn);
                break;
                
            case SOCK_INIT:
                listen(sn);
                break;
                
            case SOCK_CLOSED:
                socket(sn, Sn_MR_TCP, port, 0x00);
                break;
                
            default:
                break;
        }
    }
    return 0;
}
