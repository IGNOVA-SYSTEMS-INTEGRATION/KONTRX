# 01 - KONTRX System Overview & Architectural Topology

## 1. Executive Summary
The **KONTRX Universal Industrial Controller & Edge Gateway** is an industrial-grade embedded automation computer built on the **STMicroelectronics STM32F407VET6** (ARM Cortex-M4F with hardware single-precision floating point unit running at 168 MHz).
It functions as a unified OT/IT bridge combining high-speed motion actuation (4-axis PTO), industrial analog I/O (0–10V / 4–20mA), multi-channel digital relays, RS485 Modbus RTU environmental instrumentation, local autonomous rule logic, and industrial cloud streaming (Sparkplug B / Modbus TCP / HTTP).

---

## 2. Multi-Layer Software Architecture

The software architecture strictly adheres to a layered embedded design pattern:

```text
┌────────────────────────────────────────────────────────────────────────┐
│                        APPLICATION LAYER (APP)                         │
│  main_kontrx.c · freertos_tasks.c · http_server_task.c · ota_task.c    │
│  sparkplug_b_enc.c · dsp_filter.c · bootloader.c · web_assets.h        │
├────────────────────────────────────────────────────────────────────────┤
│                 REAL-TIME OPERATING SYSTEM (RTOS)                      │
│        FreeRTOS v10 Kernel (CMSIS-RTOS2) · Priority Preemption         │
├────────────────────────────────────────────────────────────────────────┤
│                    HARDWARE ABSTRACTION LAYER (HAL)                    │
│  modbus_dma.c · modbus_tcp_server.c · pto_motion.c · pwm_controller.c  │
│  analog_010v.c · dac_420ma.c · sdcard.c · flash_partition.c · w25q16.c│
├────────────────────────────────────────────────────────────────────────┤
│               MICROCONTROLLER ABSTRACTION LAYER (MCAL)                 │
│  gpio_stm32.c · uart_stm32.c · spi_stm32.c · timer_stm32.c · exti_stm32│
│  flash_stm32.c · rtc_stm32.c · rcc_stm32.c (Register-Direct Access)   │
├────────────────────────────────────────────────────────────────────────┤
│                          HARDWARE PLATFORM                             │
│  STM32F407VET6 · W5500 Ethernet · W25Q16 Flash · MAX485 · MicroSD     │
└────────────────────────────────────────────────────────────────────────┘
```

### Layer Descriptions:
1. **Microcontroller Abstraction Layer (MCAL)**:
   * Direct bare-metal CMSIS register drivers without third-party HAL bloat.
   * Direct bit-manipulation of GPIO `BSRR`, TIM registers, UART baud dividers, and SPI control words for maximum determinism and lowest latency.
2. **Hardware Abstraction Layer (HAL)**:
   * Encapsulates board-level peripherals: W25Q16 NOR Flash, W5500 SPI Ethernet, MAX485 half-duplex transceiver, and external stepper motor drives.
   * Provides thread-safe, mutex-guarded APIs consumed by application tasks.
3. **FreeRTOS Kernel (CMSIS-RTOS2)**:
   * Manages task scheduling, binary/counting semaphores, recursive mutexes, and millisecond timekeeping.
4. **Application Layer (APP)**:
   * Implements high-level industrial logic: Modbus round-robin sensor polling, DSP digital filtering, hierarchical automation rule execution, 3-socket HTTP web services, and Sparkplug B MQTT serialization.

---

## 3. Bus & Clock Tree Architecture
* **Core Frequency**: 168 MHz driven by an 8 MHz High-Speed External (HSE) quartz crystal via PLL multipliers.
* **AHB Bus**: Runs at 168 MHz supplying Core, DMA controllers, and Flash memory accelerator.
* **APB1 Bus**: 42 MHz (TIM2, TIM3, TIM4 timers run at 84 MHz via internal PLL multiplier).
* **APB2 Bus**: 84 MHz (TIM1 advanced motion timer runs at 168 MHz).
* **RTC**: 32.768 kHz Low-Speed External (LSE) crystal for battery-backed Unix timestamp retention.
