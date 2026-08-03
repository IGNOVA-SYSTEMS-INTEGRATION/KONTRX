cmake_minimum_required(VERSION 3.10)

project(Kontrx C CXX ASM)

set(TARGET_MCU "STM32F4" CACHE STRING "Target Microcontroller")

# CPU Specific Flags
set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CPU_FLAGS} -O0 -g -Wall -Wextra -D_WIZCHIP_=W5500")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CPU_FLAGS} -O0 -g -Wall -Wextra -D_WIZCHIP_=W5500")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${CPU_FLAGS} -g")

# Include Directories
include_directories(HAL APP MCAL/STM32F4 ioLibrary_Driver/Ethernet ioLibrary_Driver/Ethernet/W5500 ioLibrary_Driver/Internet/DHCP)
include_directories(. FreeRTOS/include FreeRTOS/portable/GCC/ARM_CM4F)

# Common MCAL Sources
set(MCAL_SOURCES
    startup_stm32f407xx.c
    HAL/led.c
    MCAL/STM32F4/gpio_stm32.c
    MCAL/STM32F4/uart_stm32.c
    MCAL/STM32F4/spi_stm32.c
    MCAL/STM32F4/flash_stm32.c
    MCAL/STM32F4/rtc_stm32.c
)

# Ethernet Drivers
set(ETHERNET_SOURCES
    ioLibrary_Driver/Ethernet/wizchip_conf.c
    ioLibrary_Driver/Ethernet/socket.c
    ioLibrary_Driver/Ethernet/W5500/w5500.c
    ioLibrary_Driver/Internet/DHCP/dhcp.c
)

# -------------------------------------------------------------------
# FreeRTOS Kernel Sources
# -------------------------------------------------------------------
set(FREERTOS_SOURCES
    FreeRTOS/tasks.c
    FreeRTOS/queue.c
    FreeRTOS/list.c
    FreeRTOS/timers.c
    FreeRTOS/event_groups.c
    FreeRTOS/stream_buffer.c
    FreeRTOS/portable/GCC/ARM_CM4F/port.c
    FreeRTOS/portable/MemMang/heap_4.c
)

# -------------------------------------------------------------------
# Kontrx RTOS Application Sources
# -------------------------------------------------------------------
set(RTOS_APP_SOURCES
    APP/cmsis_os2.c
    APP/freertos_tasks.c
    APP/http_server_task.c
    APP/ota_task.c
    HAL/modbus_dma.c
)

# -------------------------------------------------------------------
# Target 1: Bootloader (Built with bootloader.ld, flash @ 0x08000000)
# -------------------------------------------------------------------
set(BOOTLOADER_LINKER "${CMAKE_SOURCE_DIR}/bootloader.ld")
add_executable(Bootloader APP/bootloader.c ${MCAL_SOURCES})
set_target_properties(Bootloader PROPERTIES LINK_FLAGS "-T${BOOTLOADER_LINKER} ${CPU_FLAGS} -Wl,-Map=Bootloader.map -Wl,--gc-sections --specs=nosys.specs --specs=nano.specs")

add_custom_command(TARGET Bootloader POST_BUILD
    COMMAND arm-none-eabi-objcopy -O binary $<TARGET_FILE:Bootloader> Bootloader.bin
    COMMAND arm-none-eabi-size $<TARGET_FILE:Bootloader>
    COMMENT "Generating Bootloader.bin"
)

# -------------------------------------------------------------------
# Target 2: Kontrx Application — legacy test build (no RTOS)
#            Built with app_ota.ld, flash @ 0x08008000
# -------------------------------------------------------------------
set(APP_LINKER "${CMAKE_SOURCE_DIR}/app_ota.ld")
add_executable(Kontrx APP/main_w5500_test.c ${MCAL_SOURCES} ${ETHERNET_SOURCES})
set_target_properties(Kontrx PROPERTIES LINK_FLAGS "-T${APP_LINKER} ${CPU_FLAGS} -Wl,-Map=Kontrx.map -Wl,--gc-sections --specs=nosys.specs --specs=nano.specs -u _printf_float")

add_custom_command(TARGET Kontrx POST_BUILD
    COMMAND arm-none-eabi-objcopy -O binary $<TARGET_FILE:Kontrx> Kontrx.bin
    COMMAND arm-none-eabi-size $<TARGET_FILE:Kontrx>
    COMMENT "Generating Kontrx.bin (Application for OTA)"
)

# -------------------------------------------------------------------
# Target 2b: RS485_Diag — standalone raw byte-arrival diagnostic
#            Built with app_ota.ld, flash @ 0x08008000
#            Does NOT touch HAL/modbus.c or HAL/modbus_dma.c — safe to
#            build/flash/iterate without risking the production stack.
# -------------------------------------------------------------------
add_executable(RS485_Diag APP/main_rs485_diag_test.c ${MCAL_SOURCES})
set_target_properties(RS485_Diag PROPERTIES LINK_FLAGS "-T${APP_LINKER} ${CPU_FLAGS} -Wl,-Map=RS485_Diag.map -Wl,--gc-sections --specs=nosys.specs --specs=nano.specs")

add_custom_command(TARGET RS485_Diag POST_BUILD
    COMMAND arm-none-eabi-objcopy -O binary $<TARGET_FILE:RS485_Diag> RS485_Diag.bin
    COMMAND arm-none-eabi-size $<TARGET_FILE:RS485_Diag>
    COMMENT "Generating RS485_Diag.bin (standalone RS485 raw-byte diagnostic)"
)

# -------------------------------------------------------------------
# Target 3: KontrxRTOS — Full RTOS application with FreeRTOS CMSIS-OS v2
#            Entry:     APP/main_kontrx.c
#            Linker:    app_ota.ld  (flash origin 0x08008000)
#            Features:  1ms Control Engine, Modbus DMA, HTTP REST Server, OTA
# -------------------------------------------------------------------
add_executable(KontrxRTOS
    APP/main_kontrx.c
    ${MCAL_SOURCES}
    ${ETHERNET_SOURCES}
    ${FREERTOS_SOURCES}
    ${RTOS_APP_SOURCES}
)

set_target_properties(KontrxRTOS PROPERTIES
    LINK_FLAGS "-T${APP_LINKER} ${CPU_FLAGS} -Wl,-Map=KontrxRTOS.map -Wl,--gc-sections --specs=nosys.specs --specs=nano.specs -u _printf_float"
)

add_custom_command(TARGET KontrxRTOS POST_BUILD
    COMMAND arm-none-eabi-objcopy -O binary $<TARGET_FILE:KontrxRTOS> KontrxRTOS.bin
    COMMAND arm-none-eabi-objcopy -O ihex $<TARGET_FILE:KontrxRTOS> KontrxRTOS.hex
    COMMAND arm-none-eabi-size $<TARGET_FILE:KontrxRTOS>
    COMMENT "Generating KontrxRTOS.bin and KontrxRTOS.hex (RTOS Application)"
)
