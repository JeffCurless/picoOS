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
 * arch.h — Architecture-specific definitions for picoOS
 *
 * When building with the Pico SDK (the normal case, target = ARM Cortex-M0+),
 * this header just pulls in the SDK's own headers which supply CMSIS intrinsics,
 * hardware register access, and timer functions.
 *
 * When the code is analysed by a host-side LSP that does not have the Pico SDK
 * in its include path (e.g. clangd running on a desktop Linux machine), the
 * #else branch provides minimal stubs so that editor diagnostics remain
 * meaningful rather than cascading from a single missing header.
 *
 * Do NOT add business logic here — only include directives and stub definitions.
 */

#ifndef KERNEL_ARCH_H
#define KERNEL_ARCH_H

#include <stdint.h>
#include <stdbool.h>

#if defined(__arm__) || defined(__thumb__)

/* ── Real ARM / Pico SDK build ─────────────────────────────────────────────
 *
 * The CMSIS device header (RP2040.h / RP2350.h) must be included explicitly
 * to get SCB, NVIC, IRQn_Type, SysTick_Config, __set_PSP, etc.
 * pico/stdlib.h alone does not pull in the device header.
 * We select the correct one via the compiler's architecture predefined macro:
 *   __ARM_ARCH_8M_MAIN__ is set by arm-none-eabi-gcc for Cortex-M33 (RP2350).
 *   Nothing matching is set for Cortex-M0+ (RP2040).
 * hardware/timer.h defines time_us_64 / time_us_32.
 * hardware/sync.h defines __disable_irq / __enable_irq / __dmb / __nop.
 * hardware/gpio.h defines gpio_init / gpio_set_dir / gpio_put / gpio_get.
 * pico/multicore.h defines multicore_launch_core1.
 */
#if defined(__ARM_ARCH_8M_MAIN__)
#include "RP2350.h"           /* IRQn_Type, SCB, NVIC, SysTick_Config, etc. (Cortex-M33) */
#else
#include "RP2040.h"           /* IRQn_Type, SCB, NVIC, SysTick_Config, etc. (Cortex-M0+) */
#endif
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "tusb.h"               /* tud_task() — explicit USB stack servicing */
#include "pico/multicore.h"
#include "hardware/timer.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "hardware/clocks.h"   /* clock_get_hz(clk_sys) */
#include "pico/multicore.h"    /* multicore_lockout_start/end_blocking */
#ifdef PICOOS_DISPLAY_ENABLE
#include "hardware/spi.h"
#include "hardware/pwm.h"
#endif
#ifdef PICOOS_WIFI_ENABLE
#include "pico/cyw43_arch.h"
#endif

#else /* ── Host / LSP stub definitions ──────────────────────────────────── */

#include <stdio.h>  /* for printf in stubs that just return */
typedef unsigned int uint;

/* CMSIS-style register intrinsics ---------------------------------------- */
static inline void     __disable_irq(void)          {}
static inline void     __enable_irq(void)            {}

/* Pico SDK interrupt save/restore (hardware/sync.h on ARM) ---------------- */
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void     restore_interrupts(uint32_t s)    { (void)s; }
static inline void     __dmb(void)                   {}
static inline void     __dsb(void)                   {}
static inline void     __isb(void)                   {}
static inline void     __nop(void)                   {}
static inline void     __wfi(void)                   {}
static inline uint32_t __get_CONTROL(void)           { return 0; }
static inline void     __set_CONTROL(uint32_t v)     { (void)v; }
static inline void     __set_PSP(uint32_t v)         { (void)v; }
static inline uint32_t __get_PSP(void)               { return 0; }

/* SCB — System Control Block --------------------------------------------- */
typedef struct {
    volatile uint32_t ICSR;     /* Interrupt Control and State Register */
} SCB_Type;

static SCB_Type scb_stub_instance;
#define SCB                    (&scb_stub_instance)
#define SCB_ICSR_PENDSVSET_Msk (1u << 28)

/* NVIC / interrupt priorities -------------------------------------------- */
typedef enum IRQn {
    PendSV_IRQn  = -2,
    SysTick_IRQn = -1
} IRQn_Type;

static inline void     NVIC_SetPriority(IRQn_Type irq, uint32_t p) { (void)irq; (void)p; }
static inline uint32_t SysTick_Config(uint32_t ticks)              { (void)ticks; return 0; }

/* Pico SDK timer ---------------------------------------------------------- */
static inline uint64_t time_us_64(void)  { return 0; }
static inline uint32_t time_us_32(void)  { return 0; }

/* Pico SDK stdio / boot --------------------------------------------------- */
static inline void stdio_usb_init(void)  {}
static inline void stdio_init_all(void)  {}
static inline int  getchar_timeout_us(uint32_t us) { (void)us; return -1; }
static inline int  putchar_raw(int c)    { return c; }
static inline void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t ms)
                                         { (void)pc; (void)sp; (void)ms; }
static inline void reset_usb_boot(uint32_t gpio, uint32_t gpio2)
                                         { (void)gpio; (void)gpio2; }

/* Pico SDK misc ----------------------------------------------------------- */
static inline void tight_loop_contents(void) {}

/* Pico SDK multicore ------------------------------------------------------ */
static inline void multicore_launch_core1(void (*entry)(void)) { (void)entry; }
static inline void multicore_lockout_start_blocking(void)      {}
static inline void multicore_lockout_end_blocking(void)        {}
static inline void multicore_lockout_victim_init(void)         {}

/* Pico SDK flash ---------------------------------------------------------- */
#define XIP_BASE  ((uintptr_t)0x10000000u)
static inline void     flash_range_erase(uint32_t offset, size_t count)                    { (void)offset; (void)count; }
static inline void     flash_range_program(uint32_t offset, const uint8_t *data, size_t count) { (void)offset; (void)data; (void)count; }
static inline uint32_t save_and_disable_interrupts(void)                                   { return 0; }
static inline void     restore_interrupts(uint32_t status)                                 { (void)status; }

/* TinyUSB stub ------------------------------------------------------------ */
static inline void tud_task(void) {}

/* Pico SDK stdio helpers -------------------------------------------------- */
static inline bool stdio_usb_connected(void) { return false; }
static inline void sleep_ms(uint32_t ms)     { (void)ms; }
static inline void stdio_flush(void)         {}

/* hardware/clocks stubs ---------------------------------------------------*/
typedef int clock_handle_t;
#define clk_sys 6
static inline uint32_t clock_get_hz(clock_handle_t h) { (void)h; return 125000000u; }

/* CYW43 stubs (only needed when building with PICOOS_WIFI_ENABLE) --------- */
#ifdef PICOOS_WIFI_ENABLE
static inline int  cyw43_arch_init(void)   { return 0; }
static inline void cyw43_arch_deinit(void) {}
#endif

/* Pico SDK error codes ---------------------------------------------------- */
#define PICO_ERROR_TIMEOUT (-1)

/* Pico SDK GPIO ----------------------------------------------------------- */
#define GPIO_IN  0u
#define GPIO_OUT 1u
static inline void    gpio_init(uint32_t gpio)                        { (void)gpio; }
static inline void    gpio_set_dir(uint32_t gpio, bool out)           { (void)gpio; (void)out; }
static inline void    gpio_put(uint32_t gpio, bool v)                 { (void)gpio; (void)v; }
static inline bool    gpio_get(uint32_t gpio)                         { (void)gpio; return false; }

/* SPI stubs --------------------------------------------------------------- */
typedef uint8_t spi_inst_t;
static spi_inst_t spi0_inst;
#define spi0 (&spi0_inst)
static inline uint     spi_init(spi_inst_t *s, uint baud)                          { (void)s;(void)baud; return 0; }
static inline void     spi_set_format(spi_inst_t *s,uint d,uint cp,uint ch,uint o) { (void)s;(void)d;(void)cp;(void)ch;(void)o; }
static inline int      spi_write_blocking(spi_inst_t *s,const uint8_t *b,size_t n) { (void)s;(void)b;(void)n; return (int)n; }
static inline void     gpio_set_function(uint32_t gpio, uint fn)                   { (void)gpio;(void)fn; }
static inline void     gpio_pull_up(uint32_t gpio)                                 { (void)gpio; }
#define GPIO_FUNC_SPI  1u
#define SPI_CPOL_0     0u
#define SPI_CPHA_0     0u
#define SPI_MSB_FIRST  0u

/* PWM stubs --------------------------------------------------------------- */
static inline uint     pwm_gpio_to_slice_num(uint32_t gpio)                        { (void)gpio; return 0; }
static inline uint     pwm_gpio_to_channel(uint32_t gpio)                          { (void)gpio; return 0; }
static inline void     pwm_set_wrap(uint slice, uint16_t wrap)                     { (void)slice;(void)wrap; }
static inline void     pwm_set_chan_level(uint slice, uint chan, uint16_t lv)       { (void)slice;(void)chan;(void)lv; }
static inline void     pwm_set_enabled(uint slice, bool en)                        { (void)slice;(void)en; }
#define GPIO_FUNC_PWM  5u

#endif /* defined(__arm__) || defined(__thumb__) */

#endif /* KERNEL_ARCH_H */
