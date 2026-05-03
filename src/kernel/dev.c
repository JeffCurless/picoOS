/*
 * MIT License with Commons Clause
 *
 * Copyright (c) 2026 Jeff Curless
 *
 * Required Notice: Copyright (c) 2026 Jeff Curless.
 *
 * This software is licensed under the MIT License, subject to the Commons Clause
 * License Condition v1.0. You may use, copy, modify, and distribute this software,
 * but you may not sell the software itself, offer it as a paid service, or use it
 * in a product or service whose value derives substantially from the software
 * without prior written permission from the copyright holder.
 */

#include "dev.h"
#include "sched.h"   /* tick_count */
#include "arch.h"
#ifdef PICOOS_DISPLAY_ENABLE
#include "../drivers/display.h"
#endif
#ifdef PICOOS_LED_ENABLE
#include "../drivers/led.h"
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Console device
 *
 * Backed by the Pico SDK's stdio layer, which routes to USB CDC when
 * pico_enable_stdio_usb() is set in CMakeLists.txt.
 * ========================================================================= */

/* console_open — open the console device.
 * The USB CDC port is always available after stdio_usb_init(); no setup
 * is needed here. */
static int console_open(void)
{
    return 0;   /* always available */
}

/* console_read — read up to len bytes from the USB CDC input buffer.
 * Uses getchar_timeout_us(0) (non-blocking) so it returns immediately with
 * however many bytes are available.  Returns the byte count (0 = no data). */
static int console_read(uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0u) {
        return -1;
    }
    uint32_t received = 0u;
    while (received < len) {
        int ch = getchar_timeout_us(0u);   /* non-blocking */
        if (ch == PICO_ERROR_TIMEOUT) {
            break;
        }
        buf[received++] = (uint8_t)ch;
    }
    return (int)received;
}

/* console_write — write len bytes to the USB CDC output.
 * Calls putchar_raw() for each byte, bypassing the stdio buffering layer so
 * characters appear immediately on the host terminal. */
static int console_write(const uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0u) {
        return -1;
    }
    for (uint32_t i = 0u; i < len; i++) {
        putchar_raw((int)buf[i]);
    }
    return (int)len;
}

/* console_ioctl — not supported; the console has no configurable parameters
 * exposed through the device abstraction layer (baud rate, flow control, etc.
 * are managed by the Pico SDK / TinyUSB internally). */
static int console_ioctl(uint32_t cmd, void *arg)
{
    (void)cmd;
    (void)arg;
    return -1;
}

/* console_close — no-op; the USB CDC port remains open for the lifetime of
 * the firmware. */
static void console_close(void)
{
    /* nothing to do */
}

/* =========================================================================
 * Timer device
 * ========================================================================= */

/* timer_open — open the timer device.  No hardware initialisation is required
 * here because the RP2040 timer peripheral is always running after boot. */
static int timer_open(void)
{
    return 0;
}

/* timer_read — copy the current 64-bit microsecond timestamp into buf.
 * buf must point to at least sizeof(uint64_t) = 8 bytes.  Returns 8 on
 * success or -1 if buf is too small. */
static int timer_read(uint8_t *buf, uint32_t len)
{
    /* Read the 64-bit microsecond timestamp into buf. */
    if (buf == NULL || len < sizeof(uint64_t)) {
        return -1;
    }
    uint64_t us = time_us_64();
    memcpy(buf, &us, sizeof(uint64_t));
    return (int)sizeof(uint64_t);
}

/* timer_write — not supported; the hardware timer counter is read-only from
 * software on the RP2040. */
static int timer_write(const uint8_t *buf, uint32_t len)
{
    (void)buf;
    (void)len;
    return -1;
}

/* timer_ioctl — query timer values by command code.
 *   IOCTL_TIMER_GET_TICK : writes the 32-bit 1 ms scheduler tick count into
 *                          *(uint32_t *)arg.
 *   IOCTL_TIMER_GET_US   : writes the 64-bit microsecond hardware timestamp
 *                          into *(uint64_t *)arg.
 * Returns 0 on success, -1 for unknown commands or NULL arg. */
static int timer_ioctl(uint32_t cmd, void *arg)
{
    if (cmd == IOCTL_TIMER_GET_TICK) {
        if (arg == NULL) { return -1; }
        *(uint32_t *)arg = tick_count;
        return 0;
    }
    if (cmd == IOCTL_TIMER_GET_US) {
        if (arg == NULL) { return -1; }
        *(uint64_t *)arg = time_us_64();
        return 0;
    }
    return -1;
}

/* timer_close — no-op; the hardware timer cannot be stopped. */
static void timer_close(void)
{
    /* nothing to do */
}

/* =========================================================================
 * Flash device
 *
 * Phase 4: replace stubs with actual flash_range_erase / flash_range_program
 * calls, wrapping them in flash_safe_execute() to pause Core 1 and disable
 * the XIP cache before programming.
 * ========================================================================= */

/* flash_open — open the flash device.  No initialisation is required here;
 * actual flash I/O is managed by the filesystem layer (fs.c) which handles
 * the XIP cache and multicore lockout protocol directly. */
static int flash_open(void)
{
    return 0;
}

/* flash_read — stub.  Raw flash reads via this device node are not yet
 * implemented.  User-facing file I/O goes through fs.c which reads directly
 * from the XIP-mapped address space.
 * TODO Phase 5: expose a region of flash as a readable byte stream here. */
static int flash_read(uint8_t *buf, uint32_t len)
{
    (void)buf;
    (void)len;
    return -1;
}

/* flash_write — stub.  Direct flash writes via this device node are not
 * implemented.  All flash programming goes through fs.c, which correctly
 * pauses Core 1 (multicore_lockout_start_blocking) and disables interrupts
 * around flash_range_erase / flash_range_program.
 * TODO Phase 5: implement sector-aligned buffered write with lockout. */
static int flash_write(const uint8_t *buf, uint32_t len)
{
    (void)buf;
    (void)len;
    return -1;
}

/* flash_ioctl — stub.  Future commands might include:
 *   IOCTL_FLASH_ERASE_SECTOR  — erase one 4 KB sector by offset
 *   IOCTL_FLASH_GET_INFO      — return flash size and page/sector geometry
 * TODO Phase 5: implement these commands with proper XIP lockout. */
static int flash_ioctl(uint32_t cmd, void *arg)
{
    (void)cmd;
    (void)arg;
    return -1;
}

/* flash_close — no-op; flash is always accessible via XIP. */
static void flash_close(void)
{
    /* nothing to do */
}

/* =========================================================================
 * GPIO device (stub)
 * ========================================================================= */

/* gpio_open — open the GPIO device.  Individual pins must be configured via
 * IOCTL_GPIO_SET_DIR before they can be read or written. */
static int gpio_open(void)
{
    return 0;
}

/* gpio_read — stub.  Bulk GPIO pin reads are not yet implemented.  Use
 * IOCTL_GPIO_GET_VAL to read a single pin value by pin number.
 * TODO Phase 5: encode a set of pin values into buf. */
static int gpio_read(uint8_t *buf, uint32_t len)
{
    (void)buf;
    (void)len;
    return -1;
}

/* gpio_write — stub.  Bulk GPIO pin writes are not yet implemented.  Use
 * IOCTL_GPIO_SET_VAL to drive a single pin.
 * TODO Phase 5: decode pin values from buf and apply them. */
static int gpio_write(const uint8_t *buf, uint32_t len)
{
    (void)buf;
    (void)len;
    return -1;
}

/* gpio_ioctl — configure and read/write individual GPIO pins.
 *
 * All commands take a uint32_t *arg with the following encoding:
 *   bits  7:0  — GPIO pin number (0-based)
 *   bits 23:16 — direction (IOCTL_GPIO_SET_DIR: 0=in, 1=out) or
 *                output value (IOCTL_GPIO_SET_VAL: 0=low, 1=high)
 *
 *   IOCTL_GPIO_SET_DIR — call gpio_set_dir() for the specified pin.
 *   IOCTL_GPIO_SET_VAL — call gpio_put() for the specified pin.
 *   IOCTL_GPIO_GET_VAL — read the pin level; overwrites *arg with 0 or 1.
 *
 * Returns 0 on success, -1 for unknown commands. */
static int gpio_ioctl(uint32_t cmd, void *arg)
{
    if (cmd == IOCTL_GPIO_SET_DIR) {
        uint32_t val = *(uint32_t *)arg;
        uint32_t pin = val & 0xFFu;
        uint32_t dir = (val >> 16) & 0x1u;
        gpio_set_dir(pin, dir ? GPIO_OUT : GPIO_IN);
        return 0;
    }
    if (cmd == IOCTL_GPIO_SET_VAL) {
        uint32_t val = *(uint32_t *)arg;
        uint32_t pin = val & 0xFFu;
        uint32_t out = (val >> 16) & 0x1u;
        gpio_put(pin, out);
        return 0;
    }
    if (cmd == IOCTL_GPIO_GET_VAL) {
        /* arg points to uint32_t encoding: lower 8 bits = pin number.
         * On return the same word is overwritten with the pin value.    */
        uint32_t *word = (uint32_t *)arg;
        uint32_t  pin  = *word & 0xFFu;
        *word = gpio_get(pin) ? 1u : 0u;
        return 0;
    }
    return -1;
}

/* gpio_close — no-op; GPIO pin state is preserved after the device is closed
 * so peripherals connected to pins continue to operate. */
static void gpio_close(void)
{
    /* nothing to do */
}

/* =========================================================================
 * Device registry
 * ========================================================================= */

static device_t devices[DEV_COUNT] = {
    [DEV_CONSOLE] = {
        .id    = DEV_CONSOLE,
        .name  = "console",
        .open  = console_open,
        .read  = console_read,
        .write = console_write,
        .ioctl = console_ioctl,
        .close = console_close,
    },
    [DEV_TIMER] = {
        .id    = DEV_TIMER,
        .name  = "timer",
        .open  = timer_open,
        .read  = timer_read,
        .write = timer_write,
        .ioctl = timer_ioctl,
        .close = timer_close,
    },
    [DEV_FLASH] = {
        .id    = DEV_FLASH,
        .name  = "flash",
        .open  = flash_open,
        .read  = flash_read,
        .write = flash_write,
        .ioctl = flash_ioctl,
        .close = flash_close,
    },
    [DEV_GPIO] = {
        .id    = DEV_GPIO,
        .name  = "gpio",
        .open  = gpio_open,
        .read  = gpio_read,
        .write = gpio_write,
        .ioctl = gpio_ioctl,
        .close = gpio_close,
    },
#ifdef PICOOS_DISPLAY_ENABLE
    [DEV_DISPLAY] = {
        .id    = DEV_DISPLAY,
        .name  = "display",
        .open  = display_open,
        .read  = display_read,
        .write = display_write,
        .ioctl = display_ioctl,
        .close = display_close,
    },
#endif
#ifdef PICOOS_LED_ENABLE
    [DEV_LED] = {
        .id    = DEV_LED,
        .name  = "led",
        .open  = led_open,
        .read  = led_read,
        .write = led_write,
        .ioctl = led_ioctl,
        .close = led_close,
    },
#endif
};

/* dev_init — initialise the device layer.
 *
 * The device table is statically initialised at compile time so no runtime
 * work is strictly necessary.  This function exists as an explicit hook in
 * the boot sequence so that future phases can register dynamic devices
 * (e.g. I2C, SPI peripherals) here without changing main.c. */
void dev_init(void)
{
#ifdef PICOOS_DISPLAY_ENABLE
    display_shell_register();   /* register 'display' shell command (no-op if PICOOS_DISPLAY_SHELL unset) */
#endif
#ifdef PICOOS_LED_ENABLE
    led_shell_register();       /* register 'led' shell command (no-op if PICOOS_LED_SHELL unset) */
#endif
}

device_t *dev_get(dev_id_t id)
{
    if (id >= DEV_COUNT) {
        return NULL;
    }
    return &devices[id];
}

int dev_open(dev_id_t id)
{
    device_t *d = dev_get(id);
    if (d == NULL || d->open == NULL) { return -1; }
    return d->open();
}

int dev_read(dev_id_t id, uint8_t *buf, uint32_t len)
{
    device_t *d = dev_get(id);
    if (d == NULL || d->read == NULL) { return -1; }
    return d->read(buf, len);
}

int dev_write(dev_id_t id, const uint8_t *buf, uint32_t len)
{
    device_t *d = dev_get(id);
    if (d == NULL || d->write == NULL) { return -1; }
    return d->write(buf, len);
}

int dev_ioctl(dev_id_t id, uint32_t cmd, void *arg)
{
    device_t *d = dev_get(id);
    if (d == NULL || d->ioctl == NULL) { return -1; }
    return d->ioctl(cmd, arg);
}

void dev_close(dev_id_t id)
{
    device_t *d = dev_get(id);
    if (d == NULL || d->close == NULL) { return; }
    d->close();
}
