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

/*
 * display.h — Pimoroni Pico Display Pack driver interface
 *
 * Hardware: ST7789 240×135 RGB332 framebuffer (converted to RGB565 on flush), SPI0.
 * GPIO pinout: SCK=18, MOSI=19, CS=17, DC=16, BL=20, BTN A=12 B=13 X=14 Y=15
 *
 * The display device is exposed at /dev/display through the VFS and is
 * registered in the device table as DEV_DISPLAY.  See dev.h for the full
 * IOCTL_DISP_* command set and argument struct definitions.
 */

#ifndef DRIVERS_DISPLAY_H
#define DRIVERS_DISPLAY_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Panel geometry
 * ------------------------------------------------------------------------- */
#define DISP_WIDTH   240u
#define DISP_HEIGHT  135u

/* -------------------------------------------------------------------------
 * Color helpers — RGB332 (8-bit: R[7:5] G[4:2] B[1:0])
 *
 * Takes full 8-bit r, g, b components and packs the top bits.
 * The framebuffer stores one byte per pixel; the flush expands to RGB565
 * before sending over SPI (the ST7789 only accepts 16-bit color over SPI).
 * Memory cost: 240×135×1 = 32,400 bytes vs 64,800 for RGB565.
 * ------------------------------------------------------------------------- */
#define RGB332(r,g,b) \
    ((uint8_t)(((r) & 0xE0u) | (((g) & 0xE0u) >> 3) | (((b) & 0xC0u) >> 6)))

#define COLOR_BLACK   RGB332(  0,   0,   0)
#define COLOR_WHITE   RGB332(255, 255, 255)
#define COLOR_RED     RGB332(255,   0,   0)
#define COLOR_GREEN   RGB332(  0, 255,   0)
#define COLOR_BLUE    RGB332(  0,   0, 255)
#define COLOR_YELLOW  RGB332(255, 255,   0)
#define COLOR_CYAN    RGB332(  0, 255, 255)
#define COLOR_MAGENTA RGB332(255,   0, 255)

/* -------------------------------------------------------------------------
 * Called by dev_init() to register the 'display' shell command.
 * ------------------------------------------------------------------------- */
void display_shell_register(void);

/* -------------------------------------------------------------------------
 * Device interface — linked into devices[] in dev.c
 * ------------------------------------------------------------------------- */
int  display_open(void);
int  display_read(uint8_t *buf, uint32_t len);
int  display_write(const uint8_t *buf, uint32_t len);
int  display_ioctl(uint32_t cmd, void *arg);
void display_close(void);

#endif /* DRIVERS_DISPLAY_H */
