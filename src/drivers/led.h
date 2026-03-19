/*
 * led.h — Pimoroni Pico Display Pack tri-color RGB LED driver interface
 *
 * Hardware: active-low RGB LED on GPIO 6 (R), 7 (G), 8 (B).
 * PWM slices: GPIO 6 → slice 3 chan A, GPIO 7 → slice 3 chan B,
 *             GPIO 8 → slice 4 chan A.
 *
 * The LED device is exposed at /dev/led through the VFS and is registered
 * in the device table as DEV_LED.  See dev.h for IOCTL_LED_* constants.
 */

#ifndef DRIVERS_LED_H
#define DRIVERS_LED_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Hardware pin definitions
 * ------------------------------------------------------------------------- */
#define LED_PIN_R  6u
#define LED_PIN_G  7u
#define LED_PIN_B  8u

/* -------------------------------------------------------------------------
 * Device driver interface
 * ------------------------------------------------------------------------- */
int  led_open(void);
int  led_read(uint8_t *buf, uint32_t len);
int  led_write(const uint8_t *buf, uint32_t len);
int  led_ioctl(uint32_t cmd, void *arg);
void led_close(void);
void led_shell_register(void);

#endif /* DRIVERS_LED_H */
