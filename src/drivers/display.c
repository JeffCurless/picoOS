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
 * display.c — Pimoroni Pico Display Pack / Display Pack 2 (ST7789, RGB332) driver
 *
 * Target display is selected at compile time via PICOOS_DISPLAY_PACK2:
 *   unset / 0 : Display Pack  — ST7789  240×135, ~32 KB framebuffer
 *   1         : Display Pack 2 — ST7789V 320×240, ~75 KB framebuffer
 *
 * Registers as DEV_DISPLAY in the device table and as /dev/display in the VFS.
 * Maintains an RGB332 framebuffer (expanded to RGB565 on flush).
 * Callers draw into the framebuffer and trigger an SPI transfer with
 * IOCTL_DISP_FLUSH.  Only rows that have been modified since the last flush
 * are transmitted (dirty-row tracking).
 */

#include "display.h"
#include "../kernel/arch.h"
#include "../kernel/dev.h"
#ifdef PICOOS_DISPLAY_SHELL
#include "../shell/shell.h"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* =========================================================================
 * Hardware pin definitions
 * ========================================================================= */
#define DISP_PIN_SCK  18u
#define DISP_PIN_MOSI 19u
#define DISP_PIN_CS   17u
#define DISP_PIN_DC   16u
#define DISP_PIN_BL   20u
#define DISP_PIN_BTN_A 12u
#define DISP_PIN_BTN_B 13u
#define DISP_PIN_BTN_X 14u
#define DISP_PIN_BTN_Y 15u

/* Viewport offset inside the ST7789 physical array.
 * Display Pack uses a 240×135 window of the 320×240 physical array.
 * Display Pack 2 uses the full 320×240 array, so both offsets are 0. */
#ifdef PICOOS_DISPLAY_PACK2
#define DISP_COL_OFFSET  0u
#define DISP_ROW_OFFSET  0u
#else
#define DISP_COL_OFFSET 40u   /* physical col 40 = logical col 0 */
#define DISP_ROW_OFFSET 53u   /* physical row 53 = logical row 0 */
#endif

/* ST7789 command bytes */
#define ST7789_SWRESET 0x01u
#define ST7789_SLPOUT  0x11u
#define ST7789_COLMOD  0x3Au
#define ST7789_MADCTL  0x36u
#define ST7789_CASET   0x2Au
#define ST7789_RASET   0x2Bu
#define ST7789_INVON   0x21u
#define ST7789_NORON   0x13u
#define ST7789_DISPON  0x29u
#define ST7789_RAMWR   0x2Cu

/* =========================================================================
 * Static 12×12 bitmap font — 95 printable ASCII characters (0x20–0x7E)
 * Nearest-neighbor upscale of a public-domain 8×8 font.
 * Each row is a uint16_t; bit N = column N (0 = leftmost).
 * ========================================================================= */
static const uint16_t font12x12[95][12] = {
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 }, /* 0x20   */
    { 0x00E0, 0x00E0, 0x01F8, 0x01F8, 0x01F8, 0x00E0, 0x00E0, 0x00E0, 0x0000, 0x00E0, 0x00E0, 0x0000 }, /* 0x21 ! */
    { 0x01DC, 0x01DC, 0x01DC, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 }, /* 0x22 " */
    { 0x01DC, 0x01DC, 0x01DC, 0x07FF, 0x07FF, 0x01DC, 0x07FF, 0x07FF, 0x01DC, 0x01DC, 0x01DC, 0x0000 }, /* 0x23 # */
    { 0x0038, 0x0038, 0x01FC, 0x0007, 0x0007, 0x00FC, 0x01C0, 0x01C0, 0x00FF, 0x0038, 0x0038, 0x0000 }, /* 0x24 $ */
    { 0x0000, 0x0000, 0x0707, 0x01C7, 0x01C7, 0x00E0, 0x0038, 0x0038, 0x071C, 0x0707, 0x0707, 0x0000 }, /* 0x25 % */
    { 0x00F8, 0x00F8, 0x01DC, 0x00F8, 0x00F8, 0x073C, 0x01E7, 0x01E7, 0x01C7, 0x073C, 0x073C, 0x0000 }, /* 0x26 & */
    { 0x001C, 0x001C, 0x001C, 0x0007, 0x0007, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 }, /* 0x27 ' */
    { 0x00E0, 0x00E0, 0x0038, 0x001C, 0x001C, 0x001C, 0x001C, 0x001C, 0x0038, 0x00E0, 0x00E0, 0x0000 }, /* 0x28 ( */
    { 0x001C, 0x001C, 0x0038, 0x00E0, 0x00E0, 0x00E0, 0x00E0, 0x00E0, 0x0038, 0x001C, 0x001C, 0x0000 }, /* 0x29 ) */
    { 0x0000, 0x0000, 0x071C, 0x01F8, 0x01F8, 0x0FFF, 0x01F8, 0x01F8, 0x071C, 0x0000, 0x0000, 0x0000 }, /* 0x2A * */
    { 0x0000, 0x0000, 0x0038, 0x0038, 0x0038, 0x01FF, 0x0038, 0x0038, 0x0038, 0x0000, 0x0000, 0x0000 }, /* 0x2B + */
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0038, 0x0038, 0x0038, 0x001C }, /* 0x2C , */
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x01FF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 }, /* 0x2D - */
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0038, 0x0038, 0x0038, 0x0000 }, /* 0x2E . */
    { 0x0700, 0x0700, 0x01C0, 0x00E0, 0x00E0, 0x0038, 0x001C, 0x001C, 0x0007, 0x0003, 0x0003, 0x0000 }, /* 0x2F / */
    { 0x01FC, 0x01FC, 0x0707, 0x07C7, 0x07C7, 0x07E7, 0x073F, 0x073F, 0x071F, 0x01FC, 0x01FC, 0x0000 }, /* 0x30 0 */
    { 0x0038, 0x0038, 0x003C, 0x0038, 0x0038, 0x0038, 0x0038, 0x0038, 0x0038, 0x01FF, 0x01FF, 0x0000 }, /* 0x31 1 */
    { 0x00FC, 0x00FC, 0x01C7, 0x01C0, 0x01C0, 0x00F8, 0x001C, 0x001C, 0x01C7, 0x01FF, 0x01FF, 0x0000 }, /* 0x32 2 */
    { 0x00FC, 0x00FC, 0x01C7, 0x01C0, 0x01C0, 0x00F8, 0x01C0, 0x01C0, 0x01C7, 0x00FC, 0x00FC, 0x0000 }, /* 0x33 3 */
    { 0x01E0, 0x01E0, 0x01F8, 0x01DC, 0x01DC, 0x01C7, 0x07FF, 0x07FF, 0x01C0, 0x07E0, 0x07E0, 0x0000 }, /* 0x34 4 */
    { 0x01FF, 0x01FF, 0x0007, 0x00FF, 0x00FF, 0x01C0, 0x01C0, 0x01C0, 0x01C7, 0x00FC, 0x00FC, 0x0000 }, /* 0x35 5 */
    { 0x00F8, 0x00F8, 0x001C, 0x0007, 0x0007, 0x00FF, 0x01C7, 0x01C7, 0x01C7, 0x00FC, 0x00FC, 0x0000 }, /* 0x36 6 */
    { 0x01FF, 0x01FF, 0x01C7, 0x01C0, 0x01C0, 0x00E0, 0x0038, 0x0038, 0x0038, 0x0038, 0x0038, 0x0000 }, /* 0x37 7 */
    { 0x00FC, 0x00FC, 0x01C7, 0x01C7, 0x01C7, 0x00FC, 0x01C7, 0x01C7, 0x01C7, 0x00FC, 0x00FC, 0x0000 }, /* 0x38 8 */
    { 0x00FC, 0x00FC, 0x01C7, 0x01C7, 0x01C7, 0x01FC, 0x01C0, 0x01C0, 0x00E0, 0x003C, 0x003C, 0x0000 }, /* 0x39 9 */
    { 0x0000, 0x0000, 0x0038, 0x0038, 0x0038, 0x0000, 0x0000, 0x0000, 0x0038, 0x0038, 0x0038, 0x0000 }, /* 0x3A : */
    { 0x0000, 0x0000, 0x0038, 0x0038, 0x0038, 0x0000, 0x0000, 0x0000, 0x0038, 0x0038, 0x0038, 0x001C }, /* 0x3B ; */
    { 0x00E0, 0x00E0, 0x0038, 0x001C, 0x001C, 0x0007, 0x001C, 0x001C, 0x0038, 0x00E0, 0x00E0, 0x0000 }, /* 0x3C < */
    { 0x0000, 0x0000, 0x0000, 0x01FF, 0x01FF, 0x0000, 0x0000, 0x0000, 0x01FF, 0x0000, 0x0000, 0x0000 }, /* 0x3D = */
    { 0x001C, 0x001C, 0x0038, 0x00E0, 0x00E0, 0x01C0, 0x00E0, 0x00E0, 0x0038, 0x001C, 0x001C, 0x0000 }, /* 0x3E > */
    { 0x00FC, 0x00FC, 0x01C7, 0x01C0, 0x01C0, 0x00E0, 0x0038, 0x0038, 0x0000, 0x0038, 0x0038, 0x0000 }, /* 0x3F ? */
    { 0x01FC, 0x01FC, 0x0707, 0x07E7, 0x07E7, 0x07E7, 0x07E7, 0x07E7, 0x0007, 0x00FC, 0x00FC, 0x0000 }, /* 0x40 @ */
    { 0x0038, 0x0038, 0x00FC, 0x01C7, 0x01C7, 0x01C7, 0x01FF, 0x01FF, 0x01C7, 0x01C7, 0x01C7, 0x0000 }, /* 0x41 A */
    { 0x01FF, 0x01FF, 0x071C, 0x071C, 0x071C, 0x01FC, 0x071C, 0x071C, 0x071C, 0x01FF, 0x01FF, 0x0000 }, /* 0x42 B */
    { 0x01F8, 0x01F8, 0x071C, 0x0007, 0x0007, 0x0007, 0x0007, 0x0007, 0x071C, 0x01F8, 0x01F8, 0x0000 }, /* 0x43 C */
    { 0x00FF, 0x00FF, 0x01DC, 0x071C, 0x071C, 0x071C, 0x071C, 0x071C, 0x01DC, 0x00FF, 0x00FF, 0x0000 }, /* 0x44 D */
    { 0x07FF, 0x07FF, 0x061C, 0x00DC, 0x00DC, 0x00FC, 0x00DC, 0x00DC, 0x061C, 0x07FF, 0x07FF, 0x0000 }, /* 0x45 E */
    { 0x07FF, 0x07FF, 0x061C, 0x00DC, 0x00DC, 0x00FC, 0x00DC, 0x00DC, 0x001C, 0x003F, 0x003F, 0x0000 }, /* 0x46 F */
    { 0x01F8, 0x01F8, 0x071C, 0x0007, 0x0007, 0x0007, 0x07C7, 0x07C7, 0x071C, 0x07F8, 0x07F8, 0x0000 }, /* 0x47 G */
    { 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01FF, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x0000 }, /* 0x48 H */
    { 0x00FC, 0x00FC, 0x0038, 0x0038, 0x0038, 0x0038, 0x0038, 0x0038, 0x0038, 0x00FC, 0x00FC, 0x0000 }, /* 0x49 I */
    { 0x07E0, 0x07E0, 0x01C0, 0x01C0, 0x01C0, 0x01C0, 0x01C7, 0x01C7, 0x01C7, 0x00FC, 0x00FC, 0x0000 }, /* 0x4A J */
    { 0x071F, 0x071F, 0x071C, 0x01DC, 0x01DC, 0x00FC, 0x01DC, 0x01DC, 0x071C, 0x071F, 0x071F, 0x0000 }, /* 0x4B K */
    { 0x003F, 0x003F, 0x001C, 0x001C, 0x001C, 0x001C, 0x061C, 0x061C, 0x071C, 0x07FF, 0x07FF, 0x0000 }, /* 0x4C L */
    { 0x0707, 0x0707, 0x07DF, 0x07FF, 0x07FF, 0x07FF, 0x0727, 0x0727, 0x0707, 0x0707, 0x0707, 0x0000 }, /* 0x4D M */
    { 0x0707, 0x0707, 0x071F, 0x073F, 0x073F, 0x07E7, 0x07C7, 0x07C7, 0x0707, 0x0707, 0x0707, 0x0000 }, /* 0x4E N */
    { 0x00F8, 0x00F8, 0x01DC, 0x0707, 0x0707, 0x0707, 0x0707, 0x0707, 0x01DC, 0x00F8, 0x00F8, 0x0000 }, /* 0x4F O */
    { 0x01FF, 0x01FF, 0x071C, 0x071C, 0x071C, 0x01FC, 0x001C, 0x001C, 0x001C, 0x003F, 0x003F, 0x0000 }, /* 0x50 P */
    { 0x00FC, 0x00FC, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01E7, 0x01E7, 0x00FC, 0x01E0, 0x01E0, 0x0000 }, /* 0x51 Q */
    { 0x01FF, 0x01FF, 0x071C, 0x071C, 0x071C, 0x01FC, 0x01DC, 0x01DC, 0x071C, 0x071F, 0x071F, 0x0000 }, /* 0x52 R */
    { 0x00FC, 0x00FC, 0x01C7, 0x001F, 0x001F, 0x003C, 0x01E0, 0x01E0, 0x01C7, 0x00FC, 0x00FC, 0x0000 }, /* 0x53 S */
    { 0x01FF, 0x01FF, 0x013B, 0x0038, 0x0038, 0x0038, 0x0038, 0x0038, 0x0038, 0x00FC, 0x00FC, 0x0000 }, /* 0x54 T */
    { 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01FF, 0x01FF, 0x0000 }, /* 0x55 U */
    { 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x00FC, 0x0038, 0x0038, 0x0000 }, /* 0x56 V */
    { 0x0707, 0x0707, 0x0707, 0x0707, 0x0707, 0x0727, 0x07FF, 0x07FF, 0x07DF, 0x0707, 0x0707, 0x0000 }, /* 0x57 W */
    { 0x0707, 0x0707, 0x0707, 0x01DC, 0x01DC, 0x00F8, 0x00F8, 0x00F8, 0x01DC, 0x0707, 0x0707, 0x0000 }, /* 0x58 X */
    { 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x00FC, 0x0038, 0x0038, 0x0038, 0x00FC, 0x00FC, 0x0000 }, /* 0x59 Y */
    { 0x07FF, 0x07FF, 0x0707, 0x01C3, 0x01C3, 0x00E0, 0x0638, 0x0638, 0x071C, 0x07FF, 0x07FF, 0x0000 }, /* 0x5A Z */
    { 0x00FC, 0x00FC, 0x001C, 0x001C, 0x001C, 0x001C, 0x001C, 0x001C, 0x001C, 0x00FC, 0x00FC, 0x0000 }, /* 0x5B [ */
    { 0x0007, 0x0007, 0x001C, 0x0038, 0x0038, 0x00E0, 0x01C0, 0x01C0, 0x0700, 0x0600, 0x0600, 0x0000 }, /* 0x5C \ */
    { 0x00FC, 0x00FC, 0x00E0, 0x00E0, 0x00E0, 0x00E0, 0x00E0, 0x00E0, 0x00E0, 0x00FC, 0x00FC, 0x0000 }, /* 0x5D ] */
    { 0x0020, 0x0020, 0x00F8, 0x01DC, 0x01DC, 0x0707, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 }, /* 0x5E ^ */
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0FFF }, /* 0x5F _ */
    { 0x0038, 0x0038, 0x0038, 0x00E0, 0x00E0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 }, /* 0x60 ` */
    { 0x0000, 0x0000, 0x0000, 0x00FC, 0x00FC, 0x01C0, 0x01FC, 0x01FC, 0x01C7, 0x073C, 0x073C, 0x0000 }, /* 0x61 a */
    { 0x001F, 0x001F, 0x001C, 0x001C, 0x001C, 0x01FC, 0x071C, 0x071C, 0x071C, 0x01E7, 0x01E7, 0x0000 }, /* 0x62 b */
    { 0x0000, 0x0000, 0x0000, 0x00FC, 0x00FC, 0x01C7, 0x0007, 0x0007, 0x01C7, 0x00FC, 0x00FC, 0x0000 }, /* 0x63 c */
    { 0x01E0, 0x01E0, 0x01C0, 0x01C0, 0x01C0, 0x01FC, 0x01C7, 0x01C7, 0x01C7, 0x073C, 0x073C, 0x0000 }, /* 0x64 d */
    { 0x0000, 0x0000, 0x0000, 0x00FC, 0x00FC, 0x01C7, 0x01FF, 0x01FF, 0x0007, 0x00FC, 0x00FC, 0x0000 }, /* 0x65 e */
    { 0x00F8, 0x00F8, 0x01DC, 0x001C, 0x001C, 0x003F, 0x001C, 0x001C, 0x001C, 0x003F, 0x003F, 0x0000 }, /* 0x66 f */
    { 0x0000, 0x0000, 0x0000, 0x073C, 0x073C, 0x01C7, 0x01C7, 0x01C7, 0x01FC, 0x01C0, 0x01C0, 0x00FF }, /* 0x67 g */
    { 0x001F, 0x001F, 0x001C, 0x01DC, 0x01DC, 0x073C, 0x071C, 0x071C, 0x071C, 0x071F, 0x071F, 0x0000 }, /* 0x68 h */
    { 0x0038, 0x0038, 0x0000, 0x003C, 0x003C, 0x0038, 0x0038, 0x0038, 0x0038, 0x00FC, 0x00FC, 0x0000 }, /* 0x69 i */
    { 0x01C0, 0x01C0, 0x0000, 0x01C0, 0x01C0, 0x01C0, 0x01C0, 0x01C0, 0x01C7, 0x01C7, 0x01C7, 0x00FC }, /* 0x6A j */
    { 0x001F, 0x001F, 0x001C, 0x071C, 0x071C, 0x01DC, 0x00FC, 0x00FC, 0x01DC, 0x071F, 0x071F, 0x0000 }, /* 0x6B k */
    { 0x003C, 0x003C, 0x0038, 0x0038, 0x0038, 0x0038, 0x0038, 0x0038, 0x0038, 0x00FC, 0x00FC, 0x0000 }, /* 0x6C l */
    { 0x0000, 0x0000, 0x0000, 0x01C7, 0x01C7, 0x07FF, 0x07FF, 0x07FF, 0x0727, 0x0707, 0x0707, 0x0000 }, /* 0x6D m */
    { 0x0000, 0x0000, 0x0000, 0x00FF, 0x00FF, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x0000 }, /* 0x6E n */
    { 0x0000, 0x0000, 0x0000, 0x00FC, 0x00FC, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x00FC, 0x00FC, 0x0000 }, /* 0x6F o */
    { 0x0000, 0x0000, 0x0000, 0x01E7, 0x01E7, 0x071C, 0x071C, 0x071C, 0x01FC, 0x001C, 0x001C, 0x003F }, /* 0x70 p */
    { 0x0000, 0x0000, 0x0000, 0x073C, 0x073C, 0x01C7, 0x01C7, 0x01C7, 0x01FC, 0x01C0, 0x01C0, 0x07E0 }, /* 0x71 q */
    { 0x0000, 0x0000, 0x0000, 0x01E7, 0x01E7, 0x073C, 0x071C, 0x071C, 0x001C, 0x003F, 0x003F, 0x0000 }, /* 0x72 r */
    { 0x0000, 0x0000, 0x0000, 0x01FC, 0x01FC, 0x0007, 0x00FC, 0x00FC, 0x01C0, 0x00FF, 0x00FF, 0x0000 }, /* 0x73 s */
    { 0x0020, 0x0020, 0x0038, 0x01FC, 0x01FC, 0x0038, 0x0038, 0x0038, 0x0138, 0x00E0, 0x00E0, 0x0000 }, /* 0x74 t */
    { 0x0000, 0x0000, 0x0000, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x073C, 0x073C, 0x0000 }, /* 0x75 u */
    { 0x0000, 0x0000, 0x0000, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x00FC, 0x0038, 0x0038, 0x0000 }, /* 0x76 v */
    { 0x0000, 0x0000, 0x0000, 0x0707, 0x0707, 0x0727, 0x07FF, 0x07FF, 0x07FF, 0x01DC, 0x01DC, 0x0000 }, /* 0x77 w */
    { 0x0000, 0x0000, 0x0000, 0x0707, 0x0707, 0x01DC, 0x00F8, 0x00F8, 0x01DC, 0x0707, 0x0707, 0x0000 }, /* 0x78 x */
    { 0x0000, 0x0000, 0x0000, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01C7, 0x01FC, 0x01C0, 0x01C0, 0x00FF }, /* 0x79 y */
    { 0x0000, 0x0000, 0x0000, 0x01FF, 0x01FF, 0x00E3, 0x0038, 0x0038, 0x011C, 0x01FF, 0x01FF, 0x0000 }, /* 0x7A z */
    { 0x01E0, 0x01E0, 0x0038, 0x0038, 0x0038, 0x001F, 0x0038, 0x0038, 0x0038, 0x01E0, 0x01E0, 0x0000 }, /* 0x7B { */
    { 0x00E0, 0x00E0, 0x00E0, 0x00E0, 0x00E0, 0x0000, 0x00E0, 0x00E0, 0x00E0, 0x00E0, 0x00E0, 0x0000 }, /* 0x7C | */
    { 0x001F, 0x001F, 0x0038, 0x0038, 0x0038, 0x01E0, 0x0038, 0x0038, 0x0038, 0x001F, 0x001F, 0x0000 }, /* 0x7D } */
    { 0x073C, 0x073C, 0x01E7, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 }, /* 0x7E ~ */
};

/* =========================================================================
 * Driver state
 * ========================================================================= */
static uint8_t  framebuffer[DISP_WIDTH * DISP_HEIGHT];   /* RGB332, 1 byte/pixel */
static uint8_t  bg_color      = COLOR_BLACK;
static bool     initialized   = false;
static uint32_t write_cursor  = 0u;

/* -------------------------------------------------------------------------
 * Dirty-row tracking
 *
 * Only rows modified since the last flush are retransmitted.  Row granularity
 * (full rows rather than per-pixel columns) keeps the logic simple and still
 * gives 80–90% savings for typical terminal output, where most frames change
 * only a few lines near the bottom of the screen.
 *
 * Sentinel: dirty_min > dirty_max means "nothing dirty".
 * ------------------------------------------------------------------------- */
static uint32_t dirty_min = DISP_HEIGHT;   /* first dirty row */
static uint32_t dirty_max = 0u;            /* last dirty row (inclusive) */

static inline void dirty_mark_row(uint32_t y)
{
    if (y < dirty_min) { dirty_min = y; }
    if (y > dirty_max) { dirty_max = y; }
}

static inline void dirty_mark_all(void)
{
    dirty_min = 0u;
    dirty_max = DISP_HEIGHT - 1u;
}

static inline void dirty_reset(void)
{
    dirty_min = DISP_HEIGHT;
    dirty_max = 0u;
}

/* =========================================================================
 * Low-level SPI helpers
 * ========================================================================= */

static void st7789_cmd(uint8_t cmd)
{
    gpio_put(DISP_PIN_DC, 0);
    gpio_put(DISP_PIN_CS, 0);
    spi_write_blocking(spi0, &cmd, 1u);
    gpio_put(DISP_PIN_CS, 1);
}

static void st7789_data(const uint8_t *data, size_t len)
{
    gpio_put(DISP_PIN_DC, 1);
    gpio_put(DISP_PIN_CS, 0);
    spi_write_blocking(spi0, data, len);
    gpio_put(DISP_PIN_CS, 1);
}

static void st7789_data1(uint8_t byte)
{
    st7789_data(&byte, 1u);
}

/* =========================================================================
 * Backlight
 * ========================================================================= */

static void backlight_set(uint8_t brightness)
{
    uint slice = pwm_gpio_to_slice_num(DISP_PIN_BL);
    uint chan  = pwm_gpio_to_channel(DISP_PIN_BL);
    pwm_set_wrap(slice, 255u);
    pwm_set_chan_level(slice, chan, brightness);
    pwm_set_enabled(slice, true);
}

/* =========================================================================
 * Hardware initialisation
 * ========================================================================= */

static void display_hw_init(void)
{
    /* SPI pin functions */
    gpio_set_function(DISP_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(DISP_PIN_MOSI, GPIO_FUNC_SPI);

    /* CS and DC are software-controlled GPIO outputs */
    gpio_init(DISP_PIN_CS);
    gpio_set_dir(DISP_PIN_CS, GPIO_OUT);
    gpio_put(DISP_PIN_CS, 1);   /* deselect */

    gpio_init(DISP_PIN_DC);
    gpio_set_dir(DISP_PIN_DC, GPIO_OUT);
    gpio_put(DISP_PIN_DC, 1);

    /* Backlight via PWM */
    gpio_set_function(DISP_PIN_BL, GPIO_FUNC_PWM);

    /* Button inputs with internal pull-ups (active-low) */
    gpio_init(DISP_PIN_BTN_A);  gpio_set_dir(DISP_PIN_BTN_A, GPIO_IN);  gpio_pull_up(DISP_PIN_BTN_A);
    gpio_init(DISP_PIN_BTN_B);  gpio_set_dir(DISP_PIN_BTN_B, GPIO_IN);  gpio_pull_up(DISP_PIN_BTN_B);
    gpio_init(DISP_PIN_BTN_X);  gpio_set_dir(DISP_PIN_BTN_X, GPIO_IN);  gpio_pull_up(DISP_PIN_BTN_X);
    gpio_init(DISP_PIN_BTN_Y);  gpio_set_dir(DISP_PIN_BTN_Y, GPIO_IN);  gpio_pull_up(DISP_PIN_BTN_Y);

    /* SPI @ 62.5 MHz, 8-bit, mode 0 */
    spi_init(spi0, 62500000u);
    spi_set_format(spi0, 8u, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    /* ST7789 init sequence */
    st7789_cmd(ST7789_SWRESET);
    sleep_ms(150u);

    st7789_cmd(ST7789_SLPOUT);
    sleep_ms(10u);

    st7789_cmd(ST7789_COLMOD);
    st7789_data1(0x55u);            /* 16-bit RGB565 */

    st7789_cmd(ST7789_MADCTL);
    st7789_data1(0x70u);            /* landscape, BGR */

    /* Column window: physical DISP_COL_OFFSET to DISP_COL_OFFSET + DISP_WIDTH - 1 */
    st7789_cmd(ST7789_CASET);
    {
        uint16_t cs = (uint16_t)DISP_COL_OFFSET;
        uint16_t ce = (uint16_t)(DISP_COL_OFFSET + DISP_WIDTH - 1u);
        uint8_t d[] = { (uint8_t)(cs >> 8), (uint8_t)(cs & 0xFFu),
                        (uint8_t)(ce >> 8), (uint8_t)(ce & 0xFFu) };
        st7789_data(d, 4u);
    }

    /* Row window: physical DISP_ROW_OFFSET to DISP_ROW_OFFSET + DISP_HEIGHT - 1 */
    st7789_cmd(ST7789_RASET);
    {
        uint16_t rs = (uint16_t)DISP_ROW_OFFSET;
        uint16_t re = (uint16_t)(DISP_ROW_OFFSET + DISP_HEIGHT - 1u);
        uint8_t d[] = { (uint8_t)(rs >> 8), (uint8_t)(rs & 0xFFu),
                        (uint8_t)(re >> 8), (uint8_t)(re & 0xFFu) };
        st7789_data(d, 4u);
    }

    st7789_cmd(ST7789_INVON);       /* inversion required for correct colors */
    st7789_cmd(ST7789_NORON);
    st7789_cmd(ST7789_DISPON);
    sleep_ms(10u);

    backlight_set(255u);

    /* Fill framebuffer with black */
    memset(framebuffer, COLOR_BLACK, sizeof(framebuffer));
}

/* =========================================================================
 * Framebuffer flush — dirty-row optimised
 *
 * Only the rows modified since the last flush are transmitted.  For typical
 * terminal use (a few lines of text changing per frame) this reduces the SPI
 * transfer by 80–90% compared to a full-screen flush.
 *
 * Strategy: row granularity.  Columns are always sent full-width because the
 * ST7789 requires a contiguous pixel stream within the active window.
 * ========================================================================= */
static void display_flush_fb(void)
{
    /* Nothing to do if no pixels have changed since the last flush. */
    if (dirty_min > dirty_max) {
        return;
    }

    /* Re-arm SPI0 before every flush.
     *
     * Other SDK subsystems active between flushes (notably the CYW43 WiFi
     * driver's lwIP/PIO send path) can leave SPI0 in a state where
     * spi_write_blocking() busy-waits forever.  Calling spi_init() +
     * spi_set_format() resets the SPI0 peripheral and re-applies the
     * correct settings; the ST7789 panel retains its configuration across
     * an SPI peripheral reset so no display re-init is needed.
     */
    spi_init(spi0, 62500000u);
    spi_set_format(spi0, 8u, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(DISP_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(DISP_PIN_MOSI, GPIO_FUNC_SPI);

    /* Column window: always the full logical width. */
    st7789_cmd(ST7789_CASET);
    {
        uint16_t cs = (uint16_t)DISP_COL_OFFSET;
        uint16_t ce = (uint16_t)(DISP_COL_OFFSET + DISP_WIDTH - 1u);
        uint8_t d[] = { (uint8_t)(cs >> 8), (uint8_t)(cs & 0xFFu),
                        (uint8_t)(ce >> 8), (uint8_t)(ce & 0xFFu) };
        st7789_data(d, 4u);
    }

    /* Row window: dirty rows only.
     * Physical row = logical row + DISP_ROW_OFFSET. */
    uint16_t row_phys_start = (uint16_t)(DISP_ROW_OFFSET + dirty_min);
    uint16_t row_phys_end   = (uint16_t)(DISP_ROW_OFFSET + dirty_max);
    st7789_cmd(ST7789_RASET);
    {
        uint8_t d[] = {
            (uint8_t)(row_phys_start >> 8), (uint8_t)(row_phys_start & 0xFFu),
            (uint8_t)(row_phys_end   >> 8), (uint8_t)(row_phys_end   & 0xFFu)
        };
        st7789_data(d, 4u);
    }

    st7789_cmd(ST7789_RAMWR);

    /* Stream dirty rows, expanding RGB332 → RGB565 for the ST7789.
     * Bit-replication fills the full output range:
     *   R: 3→5 bits  (r3<<2)|(r3>>1)
     *   G: 3→6 bits  (g3<<3)|g3
     *   B: 2→5 bits  (b2<<3)|(b2<<1)|(b2>>1)           */
    gpio_put(DISP_PIN_DC, 1);
    gpio_put(DISP_PIN_CS, 0);

    uint8_t row_buf[DISP_WIDTH * 2u];   /* 480-byte stack buffer */
    for (uint32_t y = dirty_min; y <= dirty_max; y++) {
        for (uint32_t x = 0u; x < DISP_WIDTH; x++) {
            uint8_t  p   = framebuffer[y * DISP_WIDTH + x];
            uint16_t r3  = (p >> 5) & 0x07u;
            uint16_t g3  = (p >> 2) & 0x07u;
            uint16_t b2  =  p       & 0x03u;
            uint16_t r5  = (r3 << 2) | (r3 >> 1);
            uint16_t g6  = (g3 << 3) |  g3;
            uint16_t b5  = (b2 << 3) | (b2 << 1) | (b2 >> 1);
            uint16_t px  = (r5 << 11) | (g6 << 5) | b5;
            row_buf[x * 2u]      = (uint8_t)(px >> 8);
            row_buf[x * 2u + 1u] = (uint8_t)(px & 0xFFu);
        }
        spi_write_blocking(spi0, row_buf, DISP_WIDTH * 2u);
    }

    gpio_put(DISP_PIN_CS, 1);

    /* Mark the framebuffer as clean. */
    dirty_reset();
}

/* =========================================================================
 * Graphics primitives (write to framebuffer; caller must flush)
 * ========================================================================= */

static void draw_pixel_impl(int x, int y, uint8_t color)
{
    if (x < 0 || y < 0 || (uint32_t)x >= DISP_WIDTH || (uint32_t)y >= DISP_HEIGHT) {
        return;
    }
    framebuffer[(uint32_t)y * DISP_WIDTH + (uint32_t)x] = color;
    dirty_mark_row((uint32_t)y);
}

static void draw_line_impl(int x0, int y0, int x1, int y1, uint8_t color)
{
    int dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        draw_pixel_impl(x0, y0, color);
        if (x0 == x1 && y0 == y1) { break; }
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_rect_impl(int x, int y, int w, int h, uint8_t color, bool filled)
{
    if (filled) {
        for (int row = y; row < y + h; row++) {
            for (int col = x; col < x + w; col++) {
                draw_pixel_impl(col, row, color);
            }
        }
    } else {
        draw_line_impl(x,         y,         x + w - 1, y,         color);
        draw_line_impl(x + w - 1, y,         x + w - 1, y + h - 1, color);
        draw_line_impl(x + w - 1, y + h - 1, x,         y + h - 1, color);
        draw_line_impl(x,         y + h - 1, x,         y,         color);
    }
}

/* draw_char — render one character from the 12×12 font at pixel position (px, py).
 * scale:  1 = 12×12 px,  2 = 24×24 px,  3 = 36×36 px,  4 = 48×48 px (clamped). */
static void draw_char_impl(int px, int py, char c, uint8_t fg, uint8_t bg_c, uint8_t scale)
{
    if (c < 0x20 || c > 0x7Eu) { c = '?'; }
    if (scale < 1u) { scale = 1u; }
    if (scale > 4u) { scale = 4u; }

    const uint16_t *glyph = font12x12[(uint8_t)c - 0x20u];
    for (int row = 0; row < (int)FONT_H; row++) {
        uint16_t bits = glyph[row];
        for (int col = 0; col < (int)FONT_W; col++) {
            uint16_t pixel = (bits & (1u << col)) ? fg : bg_c;
            for (int sy = 0; sy < (int)scale; sy++) {
                for (int sx = 0; sx < (int)scale; sx++) {
                    draw_pixel_impl(px + col * scale + sx,
                                    py + row * scale + sy, pixel);
                }
            }
        }
    }
}

/* draw_text — render a NUL-terminated string; wraps at right edge. */
static void draw_text_impl(int px, int py, const char *str,
                           uint8_t fg, uint8_t bg_c, uint8_t scale)
{
    if (str == NULL) { return; }
    if (scale < 1u) { scale = 1u; }
    if (scale > 4u) { scale = 4u; }

    int char_w = (int)FONT_W * (int)scale;
    int char_h = (int)FONT_H * (int)scale;
    int cx = px;
    int cy = py;

    for (; *str != '\0'; str++) {
        if (*str == '\n') {
            cx  = px;
            cy += char_h;
            continue;
        }
        /* Wrap at right edge */
        if (cx + char_w > (int)DISP_WIDTH) {
            cx  = px;
            cy += char_h;
        }
        if (cy + char_h > (int)DISP_HEIGHT) { break; }
        draw_char_impl(cx, cy, *str, fg, bg_c, scale);
        cx += char_w;
    }
}

/* =========================================================================
 * Device interface
 * ========================================================================= */

int display_open(void)
{
    if (!initialized) {
        display_hw_init();
        initialized = true;

        /* Splash: blue fill + centered title, flushed immediately to give the
         * earliest possible visual feedback and assess boot-splash feasibility. */
        {
#ifdef PICOOS_DISPLAY_PACK2
            const uint8_t scale = 3u;
#else
            const uint8_t scale = 2u;
#endif
            int tx = (int)((DISP_WIDTH  - 6u * FONT_W * scale) / 2u);
            int ty = (int)((DISP_HEIGHT - FONT_H * scale) / 2u);
            memset(framebuffer, COLOR_BLUE, sizeof(framebuffer));
            dirty_mark_all();
            draw_text_impl(tx, ty, "picoOS", COLOR_WHITE, COLOR_BLUE, scale);
            display_flush_fb();
        }
    }
    write_cursor = 0u;
    return 0;
}

int display_read(uint8_t *buf, uint32_t len)
{
    (void)buf;
    (void)len;
    return -1;   /* display is write-only */
}

/* display_write — accept RGB332 bytes and write into the framebuffer starting
 * at write_cursor.  One byte per pixel.  Returns bytes consumed. */
int display_write(const uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0u) { return -1; }

    uint32_t fb_size = DISP_WIDTH * DISP_HEIGHT;
    uint32_t written = 0u;
    uint32_t start   = write_cursor;

    while (write_cursor < fb_size && written < len) {
        framebuffer[write_cursor++] = buf[written++];
    }

    /* Mark every row that was touched by this write. */
    if (written > 0u) {
        dirty_mark_row(start / DISP_WIDTH);
        dirty_mark_row((write_cursor - 1u) / DISP_WIDTH);
    }
    return (int)written;
}

int display_ioctl(uint32_t cmd, void *arg)
{
    switch (cmd) {
    case IOCTL_DISP_CLEAR:
        memset(framebuffer, bg_color, sizeof(framebuffer));
        dirty_mark_all();
        return 0;
    case IOCTL_DISP_FLUSH:
        display_flush_fb();
        return 0;

    case IOCTL_DISP_SET_BG:
        if (arg == NULL) { return -1; }
        bg_color = *(uint8_t *)arg;
        return 0;

    case IOCTL_DISP_DRAW_PIXEL: {
        if (arg == NULL) { return -1; }
        disp_pixel_arg_t *a = (disp_pixel_arg_t *)arg;
        draw_pixel_impl(a->x, a->y, a->color);
        return 0;
    }
    case IOCTL_DISP_DRAW_LINE: {
        if (arg == NULL) { return -1; }
        disp_line_arg_t *a = (disp_line_arg_t *)arg;
        draw_line_impl(a->x0, a->y0, a->x1, a->y1, a->color);
        return 0;
    }
    case IOCTL_DISP_DRAW_RECT: {
        if (arg == NULL) { return -1; }
        disp_rect_arg_t *a = (disp_rect_arg_t *)arg;
        draw_rect_impl(a->x, a->y, a->w, a->h, a->color, a->filled != 0u);
        return 0;
    }
    case IOCTL_DISP_DRAW_TEXT: {
        if (arg == NULL) { return -1; }
        disp_text_arg_t *a = (disp_text_arg_t *)arg;
        draw_text_impl(a->x, a->y, a->str, a->color, a->bg,
                       a->scale ? a->scale : 1u);
        return 0;
    }
    case IOCTL_DISP_SET_BL:
        if (arg == NULL) { return -1; }
        backlight_set(*(uint8_t *)arg);
        return 0;

    case IOCTL_DISP_GET_BTNS: {
        if (arg == NULL) { return -1; }
        uint8_t btns = 0u;
        /* Buttons are active-low — invert gpio_get() */
        if (!gpio_get(DISP_PIN_BTN_A)) { btns |= DISP_BTN_A; }
        if (!gpio_get(DISP_PIN_BTN_B)) { btns |= DISP_BTN_B; }
        if (!gpio_get(DISP_PIN_BTN_X)) { btns |= DISP_BTN_X; }
        if (!gpio_get(DISP_PIN_BTN_Y)) { btns |= DISP_BTN_Y; }
        *(uint8_t *)arg = btns;
        return 0;
    }
    case IOCTL_DISP_GET_DIMS: {
        if (arg == NULL) { return -1; }
        disp_dims_arg_t *a = (disp_dims_arg_t *)arg;
        a->width  = DISP_WIDTH;
        a->height = DISP_HEIGHT;
        return 0;
    }
    default:
        return -1;
    }
}

void display_close(void)
{
    /* Nothing to do — display stays active */
}

/* =========================================================================
 * Shell command
 * ========================================================================= */

#ifdef PICOOS_DISPLAY_SHELL

/* Parse a 6-digit RRGGBB hex string into an RGB332 color.
 * Returns COLOR_WHITE on parse error. */
static uint8_t parse_color(const char *hex)
{
    if (hex == NULL || hex[0] == '\0') { return COLOR_WHITE; }
    char *end;
    unsigned long val = strtoul(hex, &end, 16);
    if (end == hex) { return COLOR_WHITE; }
    uint8_t r = (uint8_t)((val >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((val >>  8) & 0xFFu);
    uint8_t b = (uint8_t)( val        & 0xFFu);
    return RGB332(r, g, b);
}

static int cmd_display(int argc, char **argv)
{
    if (argc < 2) {
        shell_println("usage: display <clear|fill|text|line|rect|backlight|buttons|info>");
        return -1;
    }

    const char *sub = argv[1];

    /* Ensure hardware is ready */
    if (!initialized) {
        display_open();
    }

    if (strcmp(sub, "clear") == 0) {
        display_ioctl(IOCTL_DISP_CLEAR, NULL);
        display_flush_fb();

    } else if (strcmp(sub, "fill") == 0) {
        if (argc < 3) { shell_println("usage: display fill RRGGBB"); return -1; }
        uint8_t color = parse_color(argv[2]);
        memset(framebuffer, color, sizeof(framebuffer));
        dirty_mark_all();
        display_flush_fb();

    } else if (strcmp(sub, "text") == 0) {
        if (argc < 5) { shell_println("usage: display text X Y STR"); return -1; }
        char textbuf[128];
        textbuf[0] = '\0';
        for (int i = 4; i < argc; i++) {
            if (i > 4) strncat(textbuf, " ", sizeof(textbuf) - strlen(textbuf) - 1u);
            strncat(textbuf, argv[i], sizeof(textbuf) - strlen(textbuf) - 1u);
        }
        disp_text_arg_t a;
        a.x     = (uint16_t)atoi(argv[2]);
        a.y     = (uint16_t)atoi(argv[3]);
        a.color = COLOR_WHITE;
        a.bg    = bg_color;
        a.scale = 1u;
        a.str   = textbuf;
        display_ioctl(IOCTL_DISP_DRAW_TEXT, &a);
        display_flush_fb();

    } else if (strcmp(sub, "line") == 0) {
        if (argc < 7) {
            shell_println("usage: display line X0 Y0 X1 Y1 RRGGBB");
            return -1;
        }
        disp_line_arg_t a;
        a.x0    = (uint16_t)atoi(argv[2]);
        a.y0    = (uint16_t)atoi(argv[3]);
        a.x1    = (uint16_t)atoi(argv[4]);
        a.y1    = (uint16_t)atoi(argv[5]);
        a.color = parse_color(argv[6]);
        display_ioctl(IOCTL_DISP_DRAW_LINE, &a);
        display_flush_fb();

    } else if (strcmp(sub, "rect") == 0) {
        if (argc < 7) {
            shell_println("usage: display rect X Y W H RRGGBB");
            return -1;
        }
        disp_rect_arg_t a;
        a.x      = (uint16_t)atoi(argv[2]);
        a.y      = (uint16_t)atoi(argv[3]);
        a.w      = (uint16_t)atoi(argv[4]);
        a.h      = (uint16_t)atoi(argv[5]);
        a.color  = parse_color(argv[6]);
        a.filled = 0u;
        a._pad1  = 0u;
        a._pad2  = 0u;
        display_ioctl(IOCTL_DISP_DRAW_RECT, &a);
        display_flush_fb();

    } else if (strcmp(sub, "backlight") == 0) {
        if (argc < 3) { shell_println("usage: display backlight N"); return -1; }
        uint8_t lv = (uint8_t)(atoi(argv[2]) & 0xFFu);
        display_ioctl(IOCTL_DISP_SET_BL, &lv);

    } else if (strcmp(sub, "buttons") == 0) {
        uint8_t btns = 0u;
        display_ioctl(IOCTL_DISP_GET_BTNS, &btns);
        shell_print("buttons: %s%s%s%s\r\n",
                    (btns & DISP_BTN_A) ? "A " : "",
                    (btns & DISP_BTN_B) ? "B " : "",
                    (btns & DISP_BTN_X) ? "X " : "",
                    (btns & DISP_BTN_Y) ? "Y " : "");
        if (btns == 0u) { shell_println("(none pressed)"); }

    } else if (strcmp(sub, "info") == 0) {
        shell_print("display: %ux%u  model=%s  init=%s\r\n",
                    DISP_WIDTH, DISP_HEIGHT,
#ifdef PICOOS_DISPLAY_PACK2
                    "Display Pack 2",
#else
                    "Display Pack",
#endif
                    initialized ? "yes" : "no");

    } else {
        shell_print("unknown sub-command: %s\r\n", sub);
        return -1;
    }
    return 0;
}

static const shell_cmd_t display_cmd = {
    .name    = "display",
#ifdef PICOOS_DISPLAY_PACK2
    .help    = "ST7789V 320x240 display: clear/fill/text/line/rect/backlight/buttons/info",
#else
    .help    = "ST7789 240x135 display: clear/fill/text/line/rect/backlight/buttons/info",
#endif
    .handler = cmd_display,
};

#endif /* PICOOS_DISPLAY_SHELL */

void display_shell_register(void)
{
#ifdef PICOOS_DISPLAY_SHELL
    shell_register_cmd(&display_cmd);
#endif
}
