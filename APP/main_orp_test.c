#include "modbus.h"
#include "uart_stm32.h"
#include "gpio_stm32.h"
#include "led.h"
#include <stdint.h>
#include <stdio.h>

// Override _write so standard printf() uses our Debug USART (USART1)
int _write(int file, char *ptr, int len) {
    (void)file; // Ignore file descriptor
    for (int i = 0; i < len; i++) {
        UART_Debug_SendByte((uint8_t)ptr[i]);
    }
    return len;
}

// Override _read so standard scanf() / getchar() uses our Debug USART (USART1)
int _read(int file, char *ptr, int len) {
    (void)file;
    for (int i = 0; i < len; i++) {
        ptr[i] = (char)UART_Debug_ReceiveByte();
        UART_Debug_SendByte((uint8_t)ptr[i]); // Echo back
        if (ptr[i] == '\r') {
            UART_Debug_SendByte('\n');
            ptr[i] = '\n'; // Replace CR with LF for scanf
            return i + 1;
        } else if (ptr[i] == '\n') {
            return i + 1;
        }
    }
    return len;
}

// Simple software delay (Calibrated for 16MHz HSI default clock)
void Delay_ms(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 3200; j++) {
            __asm__("nop");
        }
    }
}

int main(void) {
    // 1. Hardware Initialization
    GPIO_Init_USART3_Pins();    // Modbus USART (PB10, PB11)
    GPIO_Init_MAX485_Pins();    // MAX485 Control (PD3, PD2)
    GPIO_Init_USART1_Pins();    // Debug USART (PA9, PA10)
    
    UART_Init();                // Init USART3
    UART_Debug_Init();          // Init USART1
    
    LED_Init();                 // Heartbeat LED (PA6)

    // 2. Dependency Injection Setup
    Modbus_Interface_t modbus_if = {
        .uart_send_byte = UART_Modbus_SendByte,
        .wait_tx_complete = UART_Modbus_WaitTransmissionComplete,
        .uart_receive_byte = UART_Modbus_ReceiveByte,
        .uart_receive_byte_timeout = UART_Modbus_ReceiveByte_Timeout,
        .enable_tx_mode = MAX485_Transmit_Enable,
        .enable_rx_mode = MAX485_Receive_Enable
    };

    // 3. Initialize Modbus Stack
    Modbus_Init(&modbus_if);

    printf("\r\n=== Standalone KWS-590 ORP Sensor Test ===\r\n");
    printf("Starting up. Waiting 2 seconds for sensor boot...\r\n");
    Delay_ms(2000);

    uint32_t loop_counter = 0;

    // 4. Non-blocking Main Loop
    while (1) {
        Delay_ms(1); // 1ms base tick
        loop_counter++;

        // Toggle LED every 500ms
        if (loop_counter % 500 == 0) {
            LED_Toggle();
        }

        // Query Sensor every 500ms (0.5 seconds)
        if (loop_counter >= 500) {
            loop_counter = 0;

            uint16_t registers[2];
            uint8_t slave_id = 2;
            
            // Send Read Holding Registers (Function 0x03) request to Address 0x0000 for 2 registers
            if (Modbus_ReadHoldingRegisters(slave_id, 0x0000, 2, registers)) {
                // Parse ORP as signed integer (mV)
                int16_t orp_mv = (int16_t)registers[0];
                
                // Parse Temperature as unsigned integer (/ 100)
                uint16_t raw_temp = registers[1];
                uint16_t temp_int = raw_temp / 100;
                uint16_t temp_frac = raw_temp % 100;
                
                printf("Sensor ID %02X | ORP: %d mV | Temp: %u.%02u C\r\n", slave_id, orp_mv, temp_int, temp_frac);
            } else {
                printf("Sensor ID %02X | Failed to read data (Timeout/Error).\r\n", slave_id);
            }
        }
    }

    return 0;
}
