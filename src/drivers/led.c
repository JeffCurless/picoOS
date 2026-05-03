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
 * led.c — Pimoroni Pico Display Pack tri-color RGB LED driver
 *
 * Active-low RGB LED: GPIO 6 (R), 7 (G), 8 (B).  PWM-driven so brightness
 * can be set 0–255 per channel.  Active-low means duty=255 → LED off,
 * duty=0 → LED full brightness; led_pwm_set() performs the inversion so
 * callers always pass 0=off, 255=full-on.
 *
 * Registered as DEV_LED and mounted at /dev/led.
 */

#include "led.h"
#include "../kernel/arch.h"
#include "../kernel/dev.h"
#ifdef PICOOS_LED_SHELL
#include "../shell/shell.h"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Internal state
 * ========================================================================= */
static uint8_t s_r = 0u, s_g = 0u, s_b = 0u;   /* current brightness per channel */

/* =========================================================================
 * PWM helper
 * ========================================================================= */

/* led_pwm_set — drive one LED channel via PWM.
 * brightness 0 = off, 255 = full on.  Active-low inversion applied here. */
static void led_pwm_set(uint gpio, uint8_t brightness)
{
    uint8_t duty  = 255u - brightness;   /* active-low: invert */
    uint    slice = pwm_gpio_to_slice_num(gpio);
    uint    chan  = pwm_gpio_to_channel(gpio);
    pwm_set_wrap(slice, 255u);
    pwm_set_chan_level(slice, chan, duty);
    pwm_set_enabled(slice, true);
}

/* =========================================================================
 * Device interface
 * ========================================================================= */

/* led_open — configure GPIO 6, 7, 8 as PWM outputs and turn LED off. */
int led_open(void)
{
    gpio_set_function(LED_PIN_R, GPIO_FUNC_PWM);
    gpio_set_function(LED_PIN_G, GPIO_FUNC_PWM);
    gpio_set_function(LED_PIN_B, GPIO_FUNC_PWM);

    led_pwm_set(LED_PIN_R, 0u);
    led_pwm_set(LED_PIN_G, 0u);
    led_pwm_set(LED_PIN_B, 0u);

    s_r = 0u; s_g = 0u; s_b = 0u;
    return 0;
}

/* led_read — not meaningful for an LED; returns -1. */
int led_read(uint8_t *buf, uint32_t len)
{
    (void)buf;
    (void)len;
    return -1;
}

/* led_write — accept exactly 3 bytes {R, G, B} (0–255 each) and set the LED.
 * Returns 3 on success, -1 if buf is NULL or len < 3. */
int led_write(const uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len < 3u) {
        return -1;
    }
    s_r = buf[0]; s_g = buf[1]; s_b = buf[2];
    led_pwm_set(LED_PIN_R, s_r);
    led_pwm_set(LED_PIN_G, s_g);
    led_pwm_set(LED_PIN_B, s_b);
    return 3;
}

/* led_ioctl — LED control commands.
 *   IOCTL_LED_SET_RGB : arg = uint32_t * packed as 0x00RRGGBB
 *   IOCTL_LED_OFF     : arg = ignored; turns all channels off
 * Returns 0 on success, -1 for unknown commands. */
int led_ioctl(uint32_t cmd, void *arg)
{
    if (cmd == IOCTL_LED_SET_RGB) {
        if (arg == NULL) { return -1; }
        uint32_t rgb = *(uint32_t *)arg;
        s_r = (uint8_t)((rgb >> 16) & 0xFFu);
        s_g = (uint8_t)((rgb >>  8) & 0xFFu);
        s_b = (uint8_t)( rgb        & 0xFFu);
        led_pwm_set(LED_PIN_R, s_r);
        led_pwm_set(LED_PIN_G, s_g);
        led_pwm_set(LED_PIN_B, s_b);
        return 0;
    }
    if (cmd == IOCTL_LED_OFF) {
        s_r = 0u; s_g = 0u; s_b = 0u;
        led_pwm_set(LED_PIN_R, 0u);
        led_pwm_set(LED_PIN_G, 0u);
        led_pwm_set(LED_PIN_B, 0u);
        return 0;
    }
    return -1;
}

/* led_close — disable PWM slices and reset GPIOs to digital output high
 * (active-low: high = LED off).  Frees the PWM slices for other uses. */
void led_close(void)
{
    /* Drive all channels off before disabling PWM */
    led_pwm_set(LED_PIN_R, 0u);
    led_pwm_set(LED_PIN_G, 0u);
    led_pwm_set(LED_PIN_B, 0u);

    /* Disable the two PWM slices used by the LED pins */
    pwm_set_enabled(pwm_gpio_to_slice_num(LED_PIN_R), false);  /* slice 3: R+G */
    pwm_set_enabled(pwm_gpio_to_slice_num(LED_PIN_B), false);  /* slice 4: B   */

    /* Return GPIOs to digital output, driven high (active-low: high = off) */
    gpio_set_function(LED_PIN_R, GPIO_FUNC_SIO);
    gpio_set_function(LED_PIN_G, GPIO_FUNC_SIO);
    gpio_set_function(LED_PIN_B, GPIO_FUNC_SIO);
    gpio_set_dir(LED_PIN_R, GPIO_OUT);
    gpio_set_dir(LED_PIN_G, GPIO_OUT);
    gpio_set_dir(LED_PIN_B, GPIO_OUT);
    gpio_put(LED_PIN_R, 1u);
    gpio_put(LED_PIN_G, 1u);
    gpio_put(LED_PIN_B, 1u);

    s_r = 0u; s_g = 0u; s_b = 0u;
}

/* =========================================================================
 * Shell command
 * ========================================================================= */

#ifdef PICOOS_LED_SHELL

static int cmd_led(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: led on\n");
        printf("       led off\n");
        printf("       led RRGGBB         (hex, e.g. FF0000 = red)\n");
        printf("       led red|green|blue|white|cyan|magenta|yellow\n");
        return -1;
    }

    if (strcmp(argv[1], "on") == 0) {
        led_open();
        return 0;
    }

    if (strcmp(argv[1], "off") == 0) {
        uint8_t buf[3] = { 0u, 0u, 0u };
        led_write(buf, 3u);
        led_close();
        return 0;
    }

    uint8_t r = 0u, g = 0u, b = 0u;

    if (strcmp(argv[1], "red") == 0) {
        r = 255u;
    } else if (strcmp(argv[1], "green") == 0) {
        g = 255u;
    } else if (strcmp(argv[1], "blue") == 0) {
        b = 255u;
    } else if (strcmp(argv[1], "white") == 0) {
        r = 255u; g = 255u; b = 255u;
    } else if (strcmp(argv[1], "cyan") == 0) {
        g = 255u; b = 255u;
    } else if (strcmp(argv[1], "magenta") == 0) {
        r = 255u; b = 255u;
    } else if (strcmp(argv[1], "yellow") == 0) {
        r = 255u; g = 255u;
    } else if (strlen(argv[1]) == 6u) {
        /* RRGGBB hex string */
        char *end;
        unsigned long rgb = strtoul(argv[1], &end, 16);
        if (*end != '\0') {
            printf("Invalid hex color: %s\n", argv[1]);
            return -1;
        }
        r = (uint8_t)((rgb >> 16) & 0xFFu);
        g = (uint8_t)((rgb >>  8) & 0xFFu);
        b = (uint8_t)( rgb        & 0xFFu);
    } else {
        printf("Unknown color: %s\n", argv[1]);
        return -1;
    }

    uint8_t buf[3] = { r, g, b };
    led_write(buf, 3u);
    return 0;
}

static shell_cmd_t led_cmd = {
    .name    = "led",
    .help    = "Control RGB LED: led on|off | led RRGGBB | led <color>",
    .handler = cmd_led,
};

#endif /* PICOOS_LED_SHELL */

void led_shell_register(void)
{
#ifdef PICOOS_LED_SHELL
    shell_register_cmd(&led_cmd);
#endif
}
