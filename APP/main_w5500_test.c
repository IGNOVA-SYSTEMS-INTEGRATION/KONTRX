#include "stm32f407_regs.h"
#include "gpio_stm32.h"
#include "uart_stm32.h"
#include "spi_stm32.h"
#include "led.h"
#include "wizchip_conf.h"
#include "socket.h"
#include "dhcp.h"
#include "flash_stm32.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  printf → USART6 (PC6 TX / PC7 RX)                                 */
/* ------------------------------------------------------------------ */
int _write(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) UART_Debug_SendByte((uint8_t)ptr[i]);
    return len;
}

/* ================================================================== */
/*  ACCURATE 1 ms TIMER — SysTick                                     */
/* ================================================================== */
#define SYSCLOCK_HZ  16000000UL

static volatile uint32_t g_tick_ms = 0;

void SysTick_Handler(void) { g_tick_ms++; }

static void SysTick_Init(void) {
    SysTick->LOAD = (SYSCLOCK_HZ / 1000U) - 1U;
    SysTick->VAL  = 0U;
    SysTick->CTRL = (1U << 2) | (1U << 1) | (1U << 0); /* AHB clk, IRQ, Enable */
}

static uint32_t millis(void) { return g_tick_ms; }

static void Delay_ms(uint32_t ms) {
    uint32_t start = millis();
    while ((millis() - start) < ms) __asm__("nop");
}

void osDelay(uint32_t ms) {
    Delay_ms(ms);
}

/* ================================================================== */
/*  SPI Wrappers for WIZnet ioLibrary                                 */
/* ================================================================== */
static uint8_t W5500_SPI_ReadByte(void)          { return SPI2_ReadWriteByte(0xFF); }
static void    W5500_SPI_WriteByte(uint8_t data) { SPI2_ReadWriteByte(data); }

/* ================================================================== */
/*  Buffers                                                            */
/* ================================================================== */
static uint8_t rx_buf[2048];

/* ================================================================== */
/*  PE2 Output Control                                                 */
/* ================================================================== */
static void PE2_Init(void) {
    RCC->AHB1ENR |= (1U << 4);               /* Enable GPIOE clock     */
    GPIOE->MODER  &= ~(3U << (2 * 2));
    GPIOE->MODER  |=  (1U << (2 * 2));       /* Output mode            */
    GPIOE->OTYPER &= ~(1U << 2);             /* Push-Pull              */
    GPIOE->OSPEEDR &= ~(3U << (2 * 2));
    GPIOE->OSPEEDR |=  (2U << (2 * 2));      /* High speed             */
    GPIOE->PUPDR  &= ~(3U << (2 * 2));       /* No pull                */
    GPIOE->BSRR    = (1U << (2 + 16));       /* Start LOW (OFF)        */
}

static void PE2_On(void)  { GPIOE->BSRR = (1U <<  2);       }
static void PE2_Off(void) { GPIOE->BSRR = (1U << (2 + 16)); }

/* ================================================================== */
/*  HTML Page                                                          */
/* ================================================================== */
static const char *html_page =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html>\n"
    "<html><head><title>STM32 W5500 OTA</title>\n"
    "<style>\n"
    "body{font-family:Arial;text-align:center;margin-top:40px;background:#1a1a2e;color:#eee}\n"
    "h1{color:#e94560}h2{color:#0f3460;background:#16213e;padding:10px;border-radius:8px}\n"
    "button{font-size:18px;padding:12px 28px;margin:8px;cursor:pointer;border:none;border-radius:8px;font-weight:bold}\n"
    ".on{background:#4CAF50;color:#fff;box-shadow:0 4px 12px #4CAF5066}\n"
    ".off{background:#e94560;color:#fff;box-shadow:0 4px 12px #e9456066}\n"
    ".box{background:#16213e;padding:20px;margin:20px auto;max-width:500px;border-radius:8px}\n"
    "input[type=file]{margin:15px 0;font-size:16px}\n"
    "</style></head><body>\n"
    "<h1>STM32F407 W5500 Web + OTA [Blink 200ms]</h1>\n"
    "<h2>LED (PA6)</h2>\n"
    "<a href=\"/led_on\"><button class=\"on\">LED ON</button></a>\n"
    "<a href=\"/led_off\"><button class=\"off\">LED OFF</button></a>\n"
    "<h2>Output PE2</h2>\n"
    "<a href=\"/pe2_on\"><button class=\"on\">PE2 ON</button></a>\n"
    "<a href=\"/pe2_off\"><button class=\"off\">PE2 OFF</button></a>\n"
    "<div class=\"box\">\n"
    "<h2>Firmware OTA Update</h2>\n"
    "<input type=\"file\" id=\"fw_file\"><br>\n"
    "<button onclick=\"uploadFirmware()\" class=\"on\" id=\"btn_upload\" style=\"margin-top:10px;\">Upload & Flash Firmware</button>\n"
    "<div id=\"status\" style=\"margin-top:15px;font-weight:bold;\"></div>\n"
    "</div>\n"
    "<script>\n"
    "function uploadFirmware() {\n"
    "  var fileInput = document.getElementById('fw_file');\n"
    "  var file = fileInput.files[0];\n"
    "  if (!file) {\n"
    "    alert('Please select a file first!');\n"
    "    return;\n"
    "  }\n"
    "  var btn = document.getElementById('btn_upload');\n"
    "  var status = document.getElementById('status');\n"
    "  btn.disabled = true;\n"
    "  status.style.color = '#fff';\n"
    "  status.innerText = 'Uploading: 0%';\n"
    "  var reader = new FileReader();\n"
    "  reader.onload = function(e) {\n"
    "    var arrayBuffer = e.target.result;\n"
    "    var xhr = new XMLHttpRequest();\n"
    "    xhr.open('POST', '/update', true);\n"
    "    xhr.setRequestHeader('Content-Type', 'application/octet-stream');\n"
    "    xhr.upload.onprogress = function(evt) {\n"
    "      if (evt.lengthComputable) {\n"
    "        var percent = Math.round((evt.loaded / evt.total) * 100);\n"
    "        status.innerText = 'Uploading: ' + percent + '%';\n"
    "      }\n"
    "    };\n"
    "    xhr.onreadystatechange = function() {\n"
    "      if (xhr.readyState == 4) {\n"
    "        if (xhr.status == 200) {\n"
    "          status.style.color = '#4CAF50';\n"
    "          status.innerText = 'Upload Successful! MCU Rebooting...';\n"
    "        } else {\n"
    "          status.style.color = '#e94560';\n"
    "          status.innerText = 'Error: ' + xhr.statusText;\n"
    "          btn.disabled = false;\n"
    "        }\n"
    "      }\n"
    "    };\n"
    "    xhr.send(arrayBuffer);\n"
    "  };\n"
    "  reader.readAsArrayBuffer(file);\n"
    "}\n"
    "</script>\n"
    "</body></html>";



/* ================================================================== */
/*  W5500 Diagnostics                                                  */
/* ================================================================== */
static uint8_t W5500_Check(void) {
    /* Read VERSIONR register — W5500 always returns 0x04 */
    uint8_t ver = getVERSIONR();
    printf("[W5500] VERSIONR = 0x%02X %s\r\n",
           ver, (ver == 0x04) ? "(OK)" : "(ERROR — check SPI/wiring!)");
    return (ver == 0x04);
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */
int main(void) {

    /* ---- 1. Init timing first ---- */
    SysTick_Init();

    /* ---- 2. Peripherals ---- */
    UART_Debug_Init();          /* USART6: PC6(TX) / PC7(RX) @ 9600 baud */
    
    // Print startup metadata state
    {
        OTA_Meta_t *meta = (OTA_Meta_t *)OTA_META_ADDR;
        printf("[DEBUG] Startup Meta - magic: 0x%08lX, status: 0x%08lX, size: %lu\r\n", 
               (unsigned long)meta->magic, (unsigned long)meta->status, (unsigned long)meta->size);
    }

    LED_Init();
    PE2_Init();                 /* PE2 digital output                     */
    GPIO_Init_W5500_Pins();
    SPI2_Init();

    printf("\r\n========================================\r\n");
    printf("  STM32F407 W5500 — Debug Build\r\n");
    printf("  Debug UART : USART6 PC6/PC7\r\n");
    printf("  SysTick    : 1 ms (16 MHz HSI)\r\n");
    printf("========================================\r\n\r\n");

    /* ---- 3. Hardware reset W5500 ---- */
    printf("[INIT] Hardware Reset W5500...\r\n");
    W5500_Hardware_Reset();
    Delay_ms(200);              /* give W5500 time to boot */

    /* ---- 4. Register SPI callbacks ---- */
    reg_wizchip_cs_cbfunc(W5500_CS_Select, W5500_CS_Deselect);
    reg_wizchip_spi_cbfunc(W5500_SPI_ReadByte, W5500_SPI_WriteByte);

    /* ---- 5. Verify SPI communication ---- */
    printf("[INIT] Checking W5500 via SPI...\r\n");
    if (!W5500_Check()) {
        printf("[ERROR] W5500 not responding! Halted.\r\n");
        printf("        Check: SPI2 wiring, CS pin, RST pin, 3.3V power.\r\n");
        while (1) { LED_Toggle(); Delay_ms(100); }   /* fast blink = SPI error */
    }

    /* ---- 5.5 Prepare OTA Staging Area (Erase Sectors 6 & 7 at startup) ---- */
    printf("[INIT] Preparing OTA staging area (Erasing Sectors 6 & 7)... \r\n");
    FLASH_EraseSector(6);
    FLASH_EraseSector(7);
    printf("[INIT] OTA staging area ready.\r\n");

    /* ---- 6. Init W5500 socket buffers (Socket 0 gets 8KB to prevent HTML truncation) ---- */
    printf("[INIT] Configuring socket buffers (Socket 0 = 8KB)...\r\n");
    uint8_t memsize[2][8] = { {8,2,1,1,1,1,1,1}, {8,2,1,1,1,1,1,1} };
    if (ctlwizchip(CW_INIT_WIZCHIP, (void *)memsize) == -1) {
        printf("[ERROR] WIZCHIP buffer init failed!\r\n");
        while (1) { LED_Toggle(); Delay_ms(300); }
    }
    printf("[INIT] Socket buffers OK.\r\n");

    /* ---- 7. Wait for PHY link (cable must be plugged in!) ---- */
    printf("[PHY]  Waiting for LAN cable...\r\n");
    uint32_t phy_wait = 0;
    while (wizphy_getphylink() == PHY_LINK_OFF) {
        if ((phy_wait % 1000) == 0)
            printf("[PHY]  No link — plug in LAN cable (%lu s)\r\n",
                   (unsigned long)(phy_wait / 1000));
        Delay_ms(1);
        phy_wait++;
    }
    printf("[PHY]  Link UP! (waited %lu ms)\r\n", (unsigned long)phy_wait);
    Delay_ms(500);              /* wait for link to stabilise */

    /* ---- 8. Configure STATIC IP (no DHCP — WiFi bridge blocks broadcasts) ---- */
    /*
     * Network settings — adjust to match YOUR network:
     *   IP  : 192.168.1.200   (make sure no other device uses this)
     *   GW  : 192.168.1.1     (your router/gateway)
     *   SN  : 255.255.255.0
     *   DNS : 8.8.8.8
     */
    wiz_NetInfo net_info = {
        .mac  = {0x00, 0x08, 0xDC, 0x11, 0x22, 0x33},
        .ip   = {192, 168, 1, 200},
        .gw   = {192, 168, 1,   1},
        .sn   = {255, 255, 255, 0},
        .dns  = {8,   8,   8,   8},
        .dhcp = NETINFO_STATIC
    };
    ctlnetwork(CN_SET_NETINFO, (void *)&net_info);
    printf("[NET]  Static IP configured:\r\n");
    printf("       MAC : %02X:%02X:%02X:%02X:%02X:%02X\r\n",
           net_info.mac[0], net_info.mac[1], net_info.mac[2],
           net_info.mac[3], net_info.mac[4], net_info.mac[5]);
    printf("       IP  : %d.%d.%d.%d\r\n",
           net_info.ip[0],  net_info.ip[1],  net_info.ip[2],  net_info.ip[3]);
    printf("       GW  : %d.%d.%d.%d\r\n",
           net_info.gw[0],  net_info.gw[1],  net_info.gw[2],  net_info.gw[3]);
    printf("       SN  : %d.%d.%d.%d\r\n",
           net_info.sn[0],  net_info.sn[1],  net_info.sn[2],  net_info.sn[3]);
    printf("\r\n[WEB]  Server listening on port 80.\r\n");
    printf("[WEB]  Open your browser: http://%d.%d.%d.%d\r\n\r\n",
           net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);

    /* ---- 10. Web Server Loop ---- */
    uint8_t  sn   = 0;
    uint16_t port = 80;
    
    uint32_t last_blink_time = millis();
    uint8_t  pe2_state = 0;

    while (1) {
        /* Non-blocking Blink on PE2 every 200ms (to verify AJAX raw binary OTA update) */
        if ((millis() - last_blink_time) >= 200) {
            last_blink_time = millis();
            pe2_state = !pe2_state;
            if (pe2_state) {
                PE2_On();
            } else {
                PE2_Off();
            }
        }

        switch (getSn_SR(sn)) {

            case SOCK_ESTABLISHED:
                if (getSn_IR(sn) & Sn_IR_CON) {
                    setSn_IR(sn, Sn_IR_CON);
                    printf("[HTTP] Client connected.\r\n");
                }
                {
                    uint16_t size = getSn_RX_RSR(sn);
                    if (size > 0) {
                        if (size > sizeof(rx_buf) - 1)
                            size = (uint16_t)(sizeof(rx_buf) - 1);
                        recv(sn, rx_buf, size);
                        rx_buf[size] = '\0';

                        if (strstr((char *)rx_buf, "POST /update")) {
                            printf("[OTA] Receiving firmware update request...\r\n");

                            /* Parse Content-Length */
                            char *cl_str = strstr((char *)rx_buf, "Content-Length:");
                            uint32_t content_length = 0;
                            if (cl_str) {
                                /* Simple parsing of Content-Length value */
                                char *val = cl_str + 15;
                                while (*val == ' ') val++;
                                while (*val >= '0' && *val <= '9') {
                                    content_length = content_length * 10 + (*val - '0');
                                    val++;
                                }
                            }
                            printf("[OTA] Expected Content-Length: %lu bytes\r\n", (unsigned long)content_length);

                            /* Locate header end \r\n\r\n */
                            char *body = strstr((char *)rx_buf, "\r\n\r\n");
                            uint32_t flash_write_addr = 0x08040000U;
                            uint32_t total_written = 0;

                            if (body) {
                                body += 4;
                                uint32_t header_len = (uint32_t)(body - (char *)rx_buf);
                                uint32_t body_len = size - header_len;

                                if (body_len > 0) {
                                    FLASH_WriteBuffer(flash_write_addr, (uint8_t *)body, body_len);
                                    flash_write_addr += body_len;
                                    total_written += body_len;
                                }
                            }

                            /* Read remaining payload chunks based on Content-Length */
                            uint32_t timeout_cnt = 0;
                            while (total_written < content_length) {
                                uint16_t chunk_size = getSn_RX_RSR(sn);
                                if (chunk_size > 0) {
                                    timeout_cnt = 0;
                                    if (chunk_size > sizeof(rx_buf)) chunk_size = sizeof(rx_buf);
                                    
                                    /* Do not read more than content length remaining */
                                    if (total_written + chunk_size > content_length) {
                                        chunk_size = (uint16_t)(content_length - total_written);
                                    }

                                    recv(sn, rx_buf, chunk_size);
                                    FLASH_WriteBuffer(flash_write_addr, rx_buf, chunk_size);
                                    flash_write_addr += chunk_size;
                                    total_written += chunk_size;
                                } else {
                                    /* Check if client closed connection unexpectedly */
                                    if (getSn_SR(sn) != SOCK_ESTABLISHED) {
                                        printf("[OTA ERROR] Client disconnected early!\r\n");
                                        break;
                                    }
                                    Delay_ms(1);
                                    timeout_cnt++;
                                    if (timeout_cnt > 5000) { /* 5 seconds timeout */
                                        printf("[OTA ERROR] Rx Timeout!\r\n");
                                        break;
                                    }
                                }
                            }

                            printf("[OTA] Total Received & Flashed to Staging: %lu bytes\r\n", (unsigned long)total_written);


                            /* Write Metadata to Sector 1 */
                            printf("[OTA] Writing Metadata...\r\n");
                            FLASH_EraseSector(1);
                            OTA_Meta_t meta = {
                                .magic = OTA_MAGIC_VALUE,
                                .status = OTA_STATUS_PENDING,
                                .size = total_written,
                                .crc32 = 0
                            };
                            FLASH_WriteBuffer(OTA_META_ADDR, (uint8_t *)&meta, sizeof(meta));

                            // Verify written metadata
                            OTA_Meta_t *read_meta = (OTA_Meta_t *)OTA_META_ADDR;
                            printf("[OTA] Verifying written Metadata...\r\n");
                            printf("[OTA] Read Magic: 0x%08lX (Expected: 0x%08lX)\r\n", (unsigned long)read_meta->magic, (unsigned long)OTA_MAGIC_VALUE);
                            printf("[OTA] Read Status: 0x%08lX (Expected: 0x%08lX)\r\n", (unsigned long)read_meta->status, (unsigned long)OTA_STATUS_PENDING);
                            printf("[OTA] Read Size: %lu (Expected: %lu)\r\n", (unsigned long)read_meta->size, (unsigned long)total_written);

                            const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nOTA Received! Rebooting into Bootloader...";
                            send(sn, (uint8_t *)resp, strlen(resp));
                            disconnect(sn);

                            printf("[OTA] Rebooting System now...\r\n");
                            Delay_ms(200);
                            SCB_AIRCR = AIRCR_VECTKEY | AIRCR_SYSRESET;
                        } else if (strstr((char *)rx_buf, "GET /led_on")) {
                            printf("[HTTP] LED ON\r\n");
                            LED_On();
                            send(sn, (uint8_t *)html_page, strlen(html_page));
                            Delay_ms(30);
                            disconnect(sn);
                        } else if (strstr((char *)rx_buf, "GET /led_off")) {
                            printf("[HTTP] LED OFF\r\n");
                            LED_Off();
                            send(sn, (uint8_t *)html_page, strlen(html_page));
                            Delay_ms(30);
                            disconnect(sn);
                        } else if (strstr((char *)rx_buf, "GET /pe2_on")) {
                            printf("[HTTP] PE2 ON\r\n");
                            PE2_On();
                            send(sn, (uint8_t *)html_page, strlen(html_page));
                            Delay_ms(30);
                            disconnect(sn);
                        } else if (strstr((char *)rx_buf, "GET /pe2_off")) {
                            printf("[HTTP] PE2 OFF\r\n");
                            PE2_Off();
                            send(sn, (uint8_t *)html_page, strlen(html_page));
                            Delay_ms(30);
                            disconnect(sn);
                        } else {
                            send(sn, (uint8_t *)html_page, strlen(html_page));
                            Delay_ms(30);
                            disconnect(sn);
                        }

                    }
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
