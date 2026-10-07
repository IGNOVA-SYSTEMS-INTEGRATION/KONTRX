# 05 - Hardware Peripherals & Microcontroller Pinout Reference

## 1. Overview
The STM32F407VET6 on KONTRX interfaces with a rich array of analog, digital, timer, and communication hardware peripherals.

---

## 2. Microcontroller Pin Mapping Table

| Pin | Function | Peripheral | Hardware Circuitry & Description |
|---|---|---|---|
| **PA0** | Relay 7 | GPIO Push-Pull | Optocoupler driver for Relay Channel 7 |
| **PA1** | Relay 6 | GPIO Push-Pull | Optocoupler driver for Relay Channel 6 |
| **PA2** | Relay 8 | GPIO Push-Pull | Optocoupler driver for Relay Channel 8 |
| **PA4** | 4–20mA CH1 | DAC / AFE | Industrial 4–20mA current loop transmitter channel 1 |
| **PA6** | Status LED | GPIO Push-Pull | System heartbeat indicator LED |
| **PA9** | Console TX | USART1 AF7 | Debug console serial transmit @ 115200 8N1 |
| **PA10** | Console RX | USART1 AF7 | Debug console serial receive @ 115200 8N1 |
| **PA13** | SWDIO | Dedicated | ARM Serial Wire Debug data line |
| **PA14** | SWCLK | Dedicated | ARM Serial Wire Debug clock line |
| **PB0** | Flash CS | GPIO Push-Pull | Winbond W25Q16 SPI NOR Flash Chip Select (Protected) |
| **PB3** | SPI1 SCK | SPI1 AF5 | Flash serial clock line (Protected) |
| **PB4** | SPI1 MISO | SPI1 AF5 | Flash MISO line with internal pull-up enabled (Protected) |
| **PB5** | SPI1 MOSI | SPI1 AF5 | Flash MOSI line (Protected) |
| **PB10** | Modbus TX | USART3 AF7 | RS485 Transmit line to MAX485 |
| **PB11** | Modbus RX | USART3 AF7 | RS485 Receive line to MAX485 |
| **PB12** | W5500 CS | GPIO Push-Pull | WIZnet W5500 Ethernet Chip Select (Protected) |
| **PB13** | SPI2 SCK | SPI2 AF5 | W5500 high-speed serial clock line (Protected) |
| **PB14** | SPI2 MISO | SPI2 AF5 | W5500 master-in slave-out line (Protected) |
| **PB15** | SPI2 MOSI | SPI2 AF5 | W5500 master-out slave-in line (Protected) |
| **PC0** | Relay 4 | GPIO Push-Pull | Optocoupler driver for Relay Channel 4 |
| **PC2** | Relay 5 | GPIO Push-Pull | Optocoupler driver for Relay Channel 5 |
| **PC4** | Relay 10 | GPIO Push-Pull | Spare industrial digital relay output |
| **PC10** | SPI3 SCK | SPI3 AF6 | MicroSD Card SPI clock line |
| **PC11** | SPI3 MISO | SPI3 AF6 | MicroSD Card SPI MISO line |
| **PC12** | SPI3 MOSI | SPI3 AF6 | MicroSD Card SPI MOSI line |
| **PC14** | RTC OSC32_IN | Dedicated | 32.768 kHz Low-Speed External quartz input |
| **PC15** | RTC OSC32_OUT| Dedicated | 32.768 kHz Low-Speed External quartz output |
| **PD0** | SD CS | GPIO Push-Pull | MicroSD Card Chip Select |
| **PD2** | RS485 RE# | GPIO Push-Pull | MAX485 Receiver Enable (Active Low) |
| **PD3** | RS485 DE | GPIO Push-Pull | MAX485 Driver Enable (Active High) |
| **PD8** | Relay 11 | GPIO Push-Pull | Spare industrial digital relay output |
| **PD12** | PWM CH1 | TIM4 CH1 AF2 | Industrial PWM output channel 1 (VFD / Fan control) |
| **PD13** | PWM CH2 | TIM4 CH2 AF2 | Industrial PWM output channel 2 (Dosing pump control) |
| **PD14** | 0–10V CH1 | DAC / PWM AFE | Industrial 0–10V DC analog output channel 1 |
| **PD15** | 0–10V CH2 | DAC / PWM AFE | Industrial 0–10V DC analog output channel 2 |
| **PE0** | Limit SW 1 | EXTI0 Input | Hardware limit switch Axis 1 (Interrupt on falling edge) |
| **PE1** | Limit SW 2 | EXTI1 Input | Hardware limit switch Axis 2 (Interrupt on falling edge) |
| **PE2** | Relay 1 | GPIO Push-Pull | Optocoupler driver for Relay Channel 1 |
| **PE3** | Limit SW 3 | EXTI3 Input | Hardware limit switch Axis 3 (Interrupt on falling edge) |
| **PE4** | Relay 2 | GPIO Push-Pull | Optocoupler driver for Relay Channel 2 |
| **PE6** | Relay 3 | GPIO Push-Pull | Optocoupler driver for Relay Channel 3 |
| **PE7** | Limit SW 4 | EXTI7 Input | Hardware limit switch Axis 4 (Emergency stop) |
| **PE8** | PTO1 DIR | GPIO Push-Pull | Stepper Axis 1 direction control |
| **PE9** | PTO1 PUL | TIM1 CH1 AF1 | Stepper Axis 1 high-speed pulse train output |
| **PE10** | PTO2 DIR | GPIO Push-Pull | Stepper Axis 2 direction control |
| **PE11** | PTO2 PUL | TIM1 CH2 AF1 | Stepper Axis 2 high-speed pulse train output |
| **PE12** | PTO3 DIR | GPIO Push-Pull | Stepper Axis 3 direction control |
| **PE13** | PTO3 PUL | TIM1 CH3 AF1 | Stepper Axis 3 high-speed pulse train output |
| **PE14** | PTO4 PUL | TIM1 CH4 AF1 | Stepper Axis 4 high-speed pulse train output |
| **PE15** | PTO4 DIR | GPIO Push-Pull | Stepper Axis 4 direction control |

---

## 3. Peripheral Blacklist Protection
To ensure operational safety, the actuator configuration parser enforces an absolute blacklist:
* Any attempt to configure an actuator output on `PB0`, `PB3–PB5` (Flash bus) or `PB12–PB15` (W5500 bus) or `PB10–PB11` (Modbus bus) is rejected at the API layer and blocked in the RTOS task loop.
