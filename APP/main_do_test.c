#include "modbus.h"
#include "uart_stm32.h"
#include "gpio_stm32.h"
#include "led.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

// Decode DCBA float format from registers
float decodeFloat_DCBA(uint16_t reg0, uint16_t reg1) {
    float value;
    uint8_t *ptr = (uint8_t*)&value;
    // DCBA format means Little Endian.
    // reg0 contains D (High Byte) and C (Low Byte).
    // reg1 contains B (High Byte) and A (Low Byte).
    ptr[0] = (reg0 >> 8) & 0xFF; // D (Least Significant Byte)
    ptr[1] = reg0 & 0xFF;        // C
    ptr[2] = (reg1 >> 8) & 0xFF; // B
    ptr[3] = reg1 & 0xFF;        // A (Most Significant Byte)
    return value;
}

// Helper: Print float using whole/frac to avoid printf float issues on bare-metal
void print_float(float value, int precision) {
    int whole = (int)value;
    int multiplier = 1;
    for (int i=0; i<precision; i++) multiplier *= 10;
    int frac = (int)((value - whole) * multiplier);
    if (frac < 0) frac = -frac;
    
    if (precision == 1) printf("%d.%01d", whole, frac);
    else if (precision == 2) printf("%d.%02d", whole, frac);
    else if (precision == 3) printf("%d.%03d", whole, frac);
    else printf("%d.%d", whole, frac);
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

    printf("\r\n=== Standalone KWS-630 DO Sensor Test ===\r\n");
    printf("Starting up. Scanning for DO Sensor (IDs 0x01 to 0x10)...\r\n");
    Delay_ms(1000);

    uint8_t slave_id = 4; // Default fallback
    uint8_t sensor_found = 0;

    for (uint8_t id = 1; id <= 0x10; id++) {
        if (Modbus_ScanSensor(id, 0x2600)) {
            slave_id = id;
            sensor_found = 1;
            printf(">>> SUCCESS: Found DO Sensor at ID 0x%02X <<<\r\n", slave_id);
            break;
        }
        Delay_ms(10);
    }

    if (!sensor_found) {
        printf("--- DO Sensor not found! Defaulting to ID 0x%02X ---\r\n", slave_id);
    }
    
    printf("\r\nStarting continuous read every 0.5 seconds...\r\n");

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

            uint16_t registers[6];
            
            // Send Read Holding Registers (Function 0x03) request to Address 0x2600 for 6 registers
            if (Modbus_ReadHoldingRegisters(slave_id, 0x2600, 6, registers)) {
                float temp_val = decodeFloat_DCBA(registers[0], registers[1]);
                float do_val = decodeFloat_DCBA(registers[4], registers[5]);
                
                printf("Sensor ID %02X | DO: ", slave_id);
                print_float(do_val, 3);
                printf(" mg/L | Temp: ");
                print_float(temp_val, 2);
                printf(" C\r\n");
            } else {
                printf("Sensor ID %02X | Failed to read data (Timeout/Error).\r\n", slave_id);
            }
        }
    }

    return 0;
}
