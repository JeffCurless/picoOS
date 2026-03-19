# ---------------------------------------------------------------------------
# Version
# ---------------------------------------------------------------------------
set(PICOOS_VERSION_MAJOR 0)
set(PICOOS_VERSION_MINOR 1)
set(PICOOS_VERSION_EDIT  3)

message(STATUS "picoOS version        : ${PICOOS_VERSION_MAJOR}.${PICOOS_VERSION_MINOR}.${PICOOS_VERSION_EDIT}")

# ---------------------------------------------------------------------------
# Feature flags
#
# Pass on the cmake command line, e.g.:
#   cmake -B build -DPICOOS_DISPLAY_ENABLE=ON -DPICOOS_DISPLAY_SHELL=ON
#
# PICOOS_DISPLAY_ENABLE — compile the ST7789 driver and mount /dev/display.
#                         Requires Pimoroni Pico Display Pack hardware.
#                         Adds ~32 KB static SRAM (framebuffer).
#
# PICOOS_DISPLAY_SHELL  — register the interactive 'display' shell command.
#                         Requires PICOOS_DISPLAY_ENABLE.
#                         Enabling PICOOS_DISPLAY_SHELL automatically enables
#                         PICOOS_DISPLAY_ENABLE.
# ---------------------------------------------------------------------------
option(PICOOS_DISPLAY_ENABLE "Include Pimoroni Pico Display Pack driver" ON)
option(PICOOS_DISPLAY_SHELL  "Register 'display' shell command"          ON)

# Enforce dependency: shell command requires the driver.
# If the driver is disabled, silently disable the shell command too.
if(PICOOS_DISPLAY_SHELL AND NOT PICOOS_DISPLAY_ENABLE)
    message(STATUS "PICOOS_DISPLAY_ENABLE=OFF — disabling PICOOS_DISPLAY_SHELL")
    set(PICOOS_DISPLAY_SHELL OFF)
endif()

message(STATUS "picoOS display driver : ${PICOOS_DISPLAY_ENABLE}")
message(STATUS "picoOS display shell  : ${PICOOS_DISPLAY_SHELL}")

# PICOOS_LED_ENABLE — compile the tri-color RGB LED driver and mount /dev/led.
#                     Requires Pimoroni Pico Display Pack hardware (GPIO 6/7/8).
#                     Defaults ON when display is enabled (same board).
# PICOOS_LED_SHELL  — register the interactive 'led' shell command.
#                     Requires PICOOS_LED_ENABLE.
if(PICOOS_DISPLAY_ENABLE)
    option(PICOOS_LED_ENABLE "Include Pico Display Pack tri-color LED driver" ON)
else()
    option(PICOOS_LED_ENABLE "Include Pico Display Pack tri-color LED driver" OFF)
endif()
option(PICOOS_LED_SHELL "Register 'led' shell command" ON)

if(PICOOS_LED_SHELL AND NOT PICOOS_LED_ENABLE)
    message(STATUS "PICOOS_LED_ENABLE=OFF — disabling PICOOS_LED_SHELL")
    set(PICOOS_LED_SHELL OFF)
endif()

message(STATUS "picoOS LED driver     : ${PICOOS_LED_ENABLE}")
message(STATUS "picoOS LED shell      : ${PICOOS_LED_SHELL}")

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(PICOOS_SOURCES
    main.c
    kernel/task.c
    kernel/sched.c
    kernel/sched_asm.S
    kernel/mem.c
    kernel/sync.c
    kernel/syscall.c
    kernel/dev.c
    kernel/vfs.c
    kernel/fs.c
    shell/shell.c
    apps/demo.c
)

if(PICOOS_DISPLAY_ENABLE)
    list(APPEND PICOOS_SOURCES drivers/display.c)
endif()

if(PICOOS_LED_ENABLE)
    list(APPEND PICOOS_SOURCES drivers/led.c)
endif()

add_executable(picoos ${PICOOS_SOURCES})

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
target_include_directories(picoos PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---------------------------------------------------------------------------
# Compile-time definitions — version and feature flags
# ---------------------------------------------------------------------------
target_compile_definitions(picoos PRIVATE
    PICOOS_VERSION_MAJOR=${PICOOS_VERSION_MAJOR}
    PICOOS_VERSION_MINOR=${PICOOS_VERSION_MINOR}
    PICOOS_VERSION_EDIT=${PICOOS_VERSION_EDIT}
)

if(PICOOS_DISPLAY_ENABLE)
    target_compile_definitions(picoos PRIVATE PICOOS_DISPLAY_ENABLE=1)
endif()

if(PICOOS_DISPLAY_SHELL)
    target_compile_definitions(picoos PRIVATE PICOOS_DISPLAY_SHELL=1)
endif()

if(PICOOS_LED_ENABLE)
    target_compile_definitions(picoos PRIVATE PICOOS_LED_ENABLE=1)
endif()

if(PICOOS_LED_SHELL)
    target_compile_definitions(picoos PRIVATE PICOOS_LED_SHELL=1)
endif()

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
set(PICOOS_LIBS
    pico_stdlib
    cmsis_core
    hardware_timer
    hardware_irq
    hardware_flash
    hardware_sync
    pico_multicore
    pico_stdio_usb
)

if(PICOOS_DISPLAY_ENABLE)
    list(APPEND PICOOS_LIBS hardware_spi hardware_pwm)
elseif(PICOOS_LED_ENABLE)
    list(APPEND PICOOS_LIBS hardware_pwm)
endif()

target_link_libraries(picoos ${PICOOS_LIBS})

# Route stdio over USB CDC; disable UART stdio.
pico_enable_stdio_usb(picoos 1)
pico_enable_stdio_uart(picoos 0)

# Generate .uf2, .bin, .hex, .map, .dis output files.
pico_add_extra_outputs(picoos)
