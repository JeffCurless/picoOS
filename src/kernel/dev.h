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

#ifndef KERNEL_DEV_H
#define KERNEL_DEV_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Device IDs
 * ------------------------------------------------------------------------- */
typedef enum {
    DEV_CONSOLE = 0,
    DEV_TIMER,
    DEV_FLASH,
    DEV_GPIO,
#ifdef PICOOS_DISPLAY_ENABLE
    DEV_DISPLAY,    /* Pimoroni Pico Display Pack (ST7789, SPI0) */
#endif
#ifdef PICOOS_LED_ENABLE
    DEV_LED,        /* Pimoroni Pico Display Pack tri-color RGB LED */
#endif
    DEV_COUNT       /* sentinel — always last */
} dev_id_t;

/* -------------------------------------------------------------------------
 * ioctl command codes
 * ------------------------------------------------------------------------- */
#define IOCTL_TIMER_GET_TICK    0x0100u   /* arg: uint32_t * — fills tick_count  */
#define IOCTL_TIMER_GET_US      0x0101u   /* arg: uint64_t * — fills time_us_64  */
#define IOCTL_GPIO_SET_DIR      0x0200u   /* arg: uint32_t pin | (dir << 16)     */
#define IOCTL_GPIO_SET_VAL      0x0201u   /* arg: uint32_t pin | (val << 16)     */
#define IOCTL_GPIO_GET_VAL      0x0202u   /* arg: uint32_t * — fills pin value   */

#ifdef PICOOS_DISPLAY_ENABLE
/* Display IOCTL commands (0x0300–0x030F) --------------------------------- */
#define IOCTL_DISP_CLEAR        0x0300u  /* arg: NULL — fill FB with bg color   */
#define IOCTL_DISP_FLUSH        0x0301u  /* arg: NULL — push entire FB to panel */
#define IOCTL_DISP_SET_BG       0x0302u  /* arg: uint8_t *  — new background    */
#define IOCTL_DISP_DRAW_PIXEL   0x0303u  /* arg: disp_pixel_arg_t *             */
#define IOCTL_DISP_DRAW_LINE    0x0304u  /* arg: disp_line_arg_t *              */
#define IOCTL_DISP_DRAW_RECT    0x0305u  /* arg: disp_rect_arg_t *              */
#define IOCTL_DISP_DRAW_TEXT    0x0306u  /* arg: disp_text_arg_t *              */
#define IOCTL_DISP_SET_BL       0x0307u  /* arg: uint8_t * — backlight 0–255   */
#define IOCTL_DISP_GET_BTNS     0x0308u  /* arg: uint8_t * — button bitmask    */
#define IOCTL_DISP_GET_DIMS     0x0309u  /* arg: disp_dims_arg_t *             */

/* Button bitmask bits */
#define DISP_BTN_A  (1u << 0)   /* GPIO 12, active-low */
#define DISP_BTN_B  (1u << 1)   /* GPIO 13 */
#define DISP_BTN_X  (1u << 2)   /* GPIO 14 */
#define DISP_BTN_Y  (1u << 3)   /* GPIO 15 */

/* ioctl argument structs -------------------------------------------------- */
typedef struct { uint16_t x, y; uint8_t color, _pad;                       } disp_pixel_arg_t;
typedef struct { uint16_t x0,y0,x1,y1; uint8_t color, _pad;               } disp_line_arg_t;
typedef struct { uint16_t x,y,w,h; uint8_t color,filled,_pad1,_pad2;      } disp_rect_arg_t;
typedef struct { uint16_t x,y; uint8_t color,bg,scale,_pad; const char *str; } disp_text_arg_t;
typedef struct { uint16_t width, height;                                    } disp_dims_arg_t;
#endif /* PICOOS_DISPLAY_ENABLE */

#ifdef PICOOS_LED_ENABLE
/* LED IOCTL commands (0x0400–0x040F) ------------------------------------- */
#define IOCTL_LED_SET_RGB   0x0400u  /* arg: uint32_t * packed as 0x00RRGGBB */
#define IOCTL_LED_OFF       0x0401u  /* arg: ignored — turns LED off          */
#endif /* PICOOS_LED_ENABLE */

/* -------------------------------------------------------------------------
 * Device descriptor
 * ------------------------------------------------------------------------- */
typedef struct {
    dev_id_t    id;
    const char *name;
    int       (*open)(void);
    int       (*read)(uint8_t *buf, uint32_t len);
    int       (*write)(const uint8_t *buf, uint32_t len);
    int       (*ioctl)(uint32_t cmd, void *arg);
    void      (*close)(void);
} device_t;

/* -------------------------------------------------------------------------
 * Device manager API
 * ------------------------------------------------------------------------- */

void      dev_init(void);
device_t *dev_get(dev_id_t id);
int       dev_open(dev_id_t id);
int       dev_read(dev_id_t id, uint8_t *buf, uint32_t len);
int       dev_write(dev_id_t id, const uint8_t *buf, uint32_t len);
int       dev_ioctl(dev_id_t id, uint32_t cmd, void *arg);
void      dev_close(dev_id_t id);

#endif /* KERNEL_DEV_H */
