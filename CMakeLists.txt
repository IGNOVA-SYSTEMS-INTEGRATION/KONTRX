cmake_minimum_required(VERSION 3.10)

project(Kontrx C CXX ASM)

# Target MCU variable (Default to STM32F4)
set(TARGET_MCU "STM32F4" CACHE STRING "Target Microcontroller")

# CPU Specific Flags
set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CPU_FLAGS} -O0 -g -Wall -Wextra -D_WIZCHIP_=W5500")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CPU_FLAGS} -O0 -g -Wall -Wextra -D_WIZCHIP_=W5500")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${CPU_FLAGS} -g")

set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/stm32f407vet6.ld")
set(CMAKE_EXE_LINKER_FLAGS "-T${LINKER_SCRIPT} ${CPU_FLAGS} -Wl,-Map=${PROJECT_NAME}.map -Wl,--gc-sections --specs=nosys.specs --specs=nano.specs -u _printf_float")

# Core Include Directories
include_directories(HAL APP ioLibrary_Driver/Ethernet ioLibrary_Driver/Ethernet/W5500 ioLibrary_Driver/Internet/DHCP ioLibrary_Driver/Internet/MQTT ioLibrary_Driver/Internet/MQTT/MQTTPacket/src)
include_directories(. FreeRTOS/include FreeRTOS/portable/GCC/ARM_CM4F)

# Common Sources
set(SOURCES
    startup_stm32f407xx.c
    APP/main.c
    HAL/led.c
    HAL/modbus.c
    FreeRTOS/tasks.c
    FreeRTOS/queue.c
    FreeRTOS/list.c
    FreeRTOS/timers.c
    FreeRTOS/event_groups.c
    FreeRTOS/portable/MemMang/heap_4.c
    FreeRTOS/portable/GCC/ARM_CM4F/port.c
    ioLibrary_Driver/Ethernet/wizchip_conf.c
    ioLibrary_Driver/Ethernet/socket.c
    ioLibrary_Driver/Ethernet/W5500/w5500.c
    ioLibrary_Driver/Internet/DHCP/dhcp.c
    ioLibrary_Driver/Internet/MQTT/MQTTClient.c
    ioLibrary_Driver/Internet/MQTT/mqtt_interface.c
    ioLibrary_Driver/Internet/MQTT/MQTTPacket/src/MQTTConnectClient.c
    ioLibrary_Driver/Internet/MQTT/MQTTPacket/src/MQTTConnectServer.c
    ioLibrary_Driver/Internet/MQTT/MQTTPacket/src/MQTTDeserializePublish.c
    ioLibrary_Driver/Internet/MQTT/MQTTPacket/src/MQTTFormat.c
    ioLibrary_Driver/Internet/MQTT/MQTTPacket/src/MQTTPacket.c
    ioLibrary_Driver/Internet/MQTT/MQTTPacket/src/MQTTSerializePublish.c
    ioLibrary_Driver/Internet/MQTT/MQTTPacket/src/MQTTSubscribeClient.c
    ioLibrary_Driver/Internet/MQTT/MQTTPacket/src/MQTTSubscribeServer.c
    ioLibrary_Driver/Internet/MQTT/MQTTPacket/src/MQTTUnsubscribeClient.c
    ioLibrary_Driver/Internet/MQTT/MQTTPacket/src/MQTTUnsubscribeServer.c
)

# Hardware Specific files based on TARGET_MCU
if(TARGET_MCU STREQUAL "STM32F4")
    include_directories(MCAL/STM32F4)
    list(APPEND SOURCES 
        MCAL/STM32F4/gpio_stm32.c
        MCAL/STM32F4/uart_stm32.c
        MCAL/STM32F4/spi_stm32.c
    )
else()
    message(FATAL_ERROR "Unsupported TARGET_MCU: ${TARGET_MCU}")
endif()

add_executable(${PROJECT_NAME} ${SOURCES})

# Create .bin file and print size
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND arm-none-eabi-objcopy -O binary $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin
    COMMAND arm-none-eabi-size $<TARGET_FILE:${PROJECT_NAME}>
    COMMENT "Generating .bin and printing size"
)