# ---------------------------------------------------------------------------
# Version
# ---------------------------------------------------------------------------
set(PICOOS_VERSION_MAJOR 0)
set(PICOOS_VERSION_MINOR 1)
set(PICOOS_VERSION_EDIT  6)

message(STATUS "picoOS version        : ${PICOOS_VERSION_MAJOR}.${PICOOS_VERSION_MINOR}.${PICOOS_VERSION_EDIT}")

# ---------------------------------------------------------------------------
# Display Pack feature flags  (all four share the same physical hardware)
#
# Default: all ON.  Override on the cmake command line, e.g.:
#   cmake -B build -DPICOOS_DISPLAY_ENABLE=OFF   # disable the whole pack
#   cmake -B build -DPICOOS_LED_SHELL=OFF        # keep driver, drop shell cmd
#
# Dependency rules (enforced below after the option() declarations):
#   Pull-up  — turning any sub-feature ON automatically enables
#              PICOOS_DISPLAY_ENABLE (the hardware adapter must be present).
#   Push-down — setting PICOOS_DISPLAY_ENABLE=OFF disables all sub-features
#              (PICOOS_DISPLAY_SHELL, PICOOS_LED_ENABLE, PICOOS_LED_SHELL).
#   LED shell requires LED driver: PICOOS_LED_SHELL=ON forces PICOOS_LED_ENABLE=ON.
# ---------------------------------------------------------------------------
option(PICOOS_DISPLAY_ENABLE "Include Pimoroni Pico Display Pack driver"         ON)
option(PICOOS_DISPLAY_SHELL  "Register 'display' shell command"                  ON)
option(PICOOS_LED_ENABLE     "Include Pico Display Pack tri-color LED driver"    ON)
option(PICOOS_LED_SHELL      "Register 'led' shell command"                      ON)

# Push-down runs first: PICOOS_DISPLAY_ENABLE=OFF disables every sub-feature.
# This gives the master switch priority over individual sub-feature defaults.
if(NOT PICOOS_DISPLAY_ENABLE)
    foreach(_feat PICOOS_DISPLAY_SHELL PICOOS_LED_ENABLE PICOOS_LED_SHELL)
        if(${_feat})
            message(STATUS "PICOOS_DISPLAY_ENABLE=OFF — disabling ${_feat}")
            set(${_feat} OFF)
        endif()
    endforeach()
endif()

# Pull-up runs second: any sub-feature explicitly ON enables the hardware driver.
# (After push-down, sub-features are only ON if the user set them explicitly AND
#  PICOOS_DISPLAY_ENABLE was not set to OFF.)
if((PICOOS_DISPLAY_SHELL OR PICOOS_LED_ENABLE OR PICOOS_LED_SHELL) AND NOT PICOOS_DISPLAY_ENABLE)
    message(STATUS "Display Pack sub-feature enabled — forcing PICOOS_DISPLAY_ENABLE=ON")
    set(PICOOS_DISPLAY_ENABLE ON)
endif()

# LED shell requires LED driver.
if(PICOOS_LED_SHELL AND NOT PICOOS_LED_ENABLE)
    message(STATUS "PICOOS_LED_ENABLE=OFF — disabling PICOOS_LED_SHELL")
    set(PICOOS_LED_SHELL OFF)
endif()

message(STATUS "picoOS display driver : ${PICOOS_DISPLAY_ENABLE}")
message(STATUS "picoOS display shell  : ${PICOOS_DISPLAY_SHELL}")
message(STATUS "picoOS LED driver     : ${PICOOS_LED_ENABLE}")
message(STATUS "picoOS LED shell      : ${PICOOS_LED_SHELL}")

# ---------------------------------------------------------------------------
# Board / chip / WiFi detection
# (pico_sdk_init() must have run before PICO_RP2350/PICO_CYW43_SUPPORTED exist;
#  both are set in the root CMakeLists.txt before add_subdirectory(src))
# ---------------------------------------------------------------------------
if(PICO_RP2350)
    set(PICOOS_PLATFORM_STR "RP2350, dual ARM Cortex-M33 (150 MHz max)")
    set(PICOOS_SRAM_STR     "520 KB")
    set(PICOOS_FLASH_STR    "4 MB QSPI (XIP)")
    set(PICOOS_FS_FLASH_SIZE "(3u * 1024u * 1024u)")   # 3 MB (4 MB total - 1 MB firmware)
    set(PICOOS_FS_MAX_FILES  127u)
else()
    set(PICOOS_PLATFORM_STR "RP2040, dual ARM Cortex-M0+ (133 MHz max)")
    set(PICOOS_SRAM_STR     "264 KB")
    set(PICOOS_FLASH_STR    "2 MB QSPI (XIP)")
    set(PICOOS_FS_FLASH_SIZE "(1u * 1024u * 1024u)")   # 1 MB (2 MB total - 1 MB firmware)
    set(PICOOS_FS_MAX_FILES  64u)
endif()

# PICO_CYW43_SUPPORTED is set by the SDK when the selected board has a CYW43 chip.
if(PICO_CYW43_SUPPORTED)
    set(PICOOS_HAS_WIFI ON)
else()
    set(PICOOS_HAS_WIFI OFF)
endif()

message(STATUS "picoOS board          : ${PICO_BOARD}")
message(STATUS "picoOS chip           : ${PICO_PLATFORM}")
message(STATUS "picoOS WiFi           : ${PICOOS_HAS_WIFI}")

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

if(PICOOS_HAS_WIFI)
    list(APPEND PICOOS_SOURCES kernel/wifi.c)
    if(PICOOS_DISPLAY_ENABLE)
        list(APPEND PICOOS_SOURCES apps/cray_one.c)
    endif()
endif()

add_executable(picoos ${PICOOS_SOURCES})

# ---------------------------------------------------------------------------
# Output file naming — board-specific with version suffix.
#
# Strip underscores from PICO_BOARD so that pico_w → picow, pico2_w → pico2w,
# then append "os" and the version number.
#
# Examples:
#   pico    → picoos-v0.1.4
#   pico2   → pico2os-v0.1.4
#   pico_w  → picowos-v0.1.4
#   pico2_w → pico2wos-v0.1.4
# ---------------------------------------------------------------------------
set(_board_clean "${PICO_BOARD}")
string(REPLACE "_" "" _board_clean "${_board_clean}")
set(_output_name
    "${_board_clean}os-v${PICOOS_VERSION_MAJOR}.${PICOOS_VERSION_MINOR}.${PICOOS_VERSION_EDIT}")
message(STATUS "picoOS output name    : ${_output_name}")
set_target_properties(picoos PROPERTIES OUTPUT_NAME "${_output_name}")

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
    PICOOS_PLATFORM_STR="${PICOOS_PLATFORM_STR}"
    PICOOS_SRAM_STR="${PICOOS_SRAM_STR}"
    PICOOS_FLASH_STR="${PICOOS_FLASH_STR}"
    FS_FLASH_SIZE=${PICOOS_FS_FLASH_SIZE}
    FS_MAX_FILES=${PICOOS_FS_MAX_FILES}
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

if(PICOOS_HAS_WIFI)
    target_compile_definitions(picoos PRIVATE PICOOS_WIFI_ENABLE=1)
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
    hardware_clocks
    pico_multicore
    pico_stdio_usb
)

if(PICOOS_DISPLAY_ENABLE)
    list(APPEND PICOOS_LIBS hardware_spi hardware_pwm)
elseif(PICOOS_LED_ENABLE)
    list(APPEND PICOOS_LIBS hardware_pwm)
endif()

if(PICOOS_HAS_WIFI)
    list(APPEND PICOOS_LIBS pico_cyw43_arch_lwip_poll)
endif()

target_link_libraries(picoos ${PICOOS_LIBS})

# Route stdio over USB CDC; disable UART stdio.
pico_enable_stdio_usb(picoos 1)
pico_enable_stdio_uart(picoos 0)

# Generate .uf2, .bin, .hex, .map, .dis output files.
pico_add_extra_outputs(picoos)
