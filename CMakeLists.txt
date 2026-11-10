cmake_minimum_required(VERSION 3.13)

# Default board to Raspberry Pi Pico W / WH
set(PICO_BOARD pico_w CACHE STRING "Board type")

# Pull in Pico SDK (automatically finds ./pico-sdk if PICO_SDK_PATH is not set)
include(pico_sdk_import.cmake)

project(pico_uac_i2s_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(pico_uac_i2s_bridge
    src/main.c
    src/usb_descriptors.c
    src/uac1_device.c
    src/i2s_bridge.c
    src/audio_ring.c
)

# Generate PIO header for full-duplex I2S master and slave
pico_generate_pio_header(pico_uac_i2s_bridge ${CMAKE_CURRENT_LIST_DIR}/src/i2s_bridge.pio)

target_include_directories(pico_uac_i2s_bridge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_link_libraries(pico_uac_i2s_bridge PRIVATE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_clocks
    hardware_irq
    tinyusb_device
    tinyusb_board
)

# Enable UART0 output on GPIO 0 (TX) / GPIO 1 (RX) for Pico Debug Probe,
# and disable USB CDC stdio because USB is dedicated to UAC 1.0 Headset
pico_enable_stdio_uart(pico_uac_i2s_bridge 1)
pico_enable_stdio_usb(pico_uac_i2s_bridge 0)

# Produce UF2, ELF, BIN, MAP, DIS outputs
pico_add_extra_outputs(pico_uac_i2s_bridge)
