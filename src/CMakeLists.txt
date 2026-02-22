cmake_minimum_required(VERSION 3.13)

# Pull in the Pico SDK (must have PICO_SDK_PATH set)
include(pico_sdk_import.cmake)

project(pico_minishell C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Create a reusable static library target that exposes governor/overclock APIs
add_library(pico_gov STATIC
    dmesg.c
    system.c
    governors.c
    governors_ondemand.c
    governors_schedutil.c
    governors_performance.c
    governors_rp2040_perf.c
    benchmark.c
    persist.c
    metrics.c
    uart_log.c
    pio_idle.c          # PIO idle-time / jitter subsystem
)

target_include_directories(pico_gov PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Generate the C header from pio_idle.pio.
# pico_generate_pio_header() runs pioasm, produces pio_idle.pio.h in the
# build directory, and adds it to pico_gov's include path automatically.
pico_generate_pio_header(pico_gov ${CMAKE_CURRENT_SOURCE_DIR}/pio_idle.pio)

# Link dependencies required by the library so consumers inherit them
target_link_libraries(pico_gov PUBLIC
    pico_stdlib
    pico_multicore
    hardware_adc
    hardware_flash
    hardware_dma
    hardware_uart
    hardware_clocks
    hardware_watchdog
    hardware_vreg
    hardware_pll
    hardware_pio          # SM0/SM1 for idle measurement + jitter detection
)

# Main executable is now a thin application linking the library
add_executable(pico_minishell
    main.c
    commands.c
)

target_link_libraries(pico_minishell PRIVATE pico_gov)

# Enable USB serial, disable UART for the final executable
pico_enable_stdio_usb(pico_minishell 1)
pico_enable_stdio_uart(pico_minishell 0)

# Generate UF2, ELF, BIN, MAP files
pico_add_extra_outputs(pico_minishell)
