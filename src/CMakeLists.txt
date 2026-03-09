add_executable(picoteachos
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

target_include_directories(picoteachos PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(picoteachos
    pico_stdlib
    cmsis_core
    hardware_timer
    hardware_irq
    hardware_flash
    hardware_sync
    pico_multicore
    pico_stdio_usb
)

# Route stdio over USB CDC; disable UART stdio.
pico_enable_stdio_usb(picoteachos 1)
pico_enable_stdio_uart(picoteachos 0)

# Generate .uf2, .bin, .hex, .map, .dis output files.
pico_add_extra_outputs(picoteachos)
