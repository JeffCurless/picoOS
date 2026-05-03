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

#include "demo.h"
#include "../kernel/sync.h"
#include "../kernel/syscall.h"

#include <stdio.h>
#include <stdint.h>
#include "../shell/shell.h"

#if defined(PICOOS_WIFI_ENABLE) && defined(PICOOS_DISPLAY_ENABLE)
extern void cray_one(void *arg);
#endif

/* =========================================================================
 * Shared IPC objects for the producer-consumer demo
 *
 * These are global so both threads can access them without passing pointers
 * through the 'arg' parameter (which only carries one void*).  For a more
 * complete design, arg would point to a struct holding both.
 * ========================================================================= */

/* Message format: a single 32-bit counter value. */
typedef struct {
    uint32_t value;
} demo_message_t;

/* Semaphore: count = number of unconsumed items available. */
static ksemaphore_t producer_sem;

/* Message queue: producer sends here, consumer reads here. */
static mqueue_t producer_queue;

/* Flag to track whether the shared IPC objects have been initialised.
 * demo_producer initialises on first run; demo_consumer waits on the
 * semaphore, which will block until the producer has run at least once. */
static bool demo_ipc_ready = false;

/* =========================================================================
 * demo_ipc_init — initialises the shared IPC objects.
 *
 * Called by demo_producer on its first invocation.  Protected by the
 * demo_ipc_ready flag; only the first caller does the work.
 * ========================================================================= */
static void demo_ipc_init(void)
{
    ksemaphore_init(&producer_sem, 0);
    mqueue_init(&producer_queue, sizeof(demo_message_t));
    demo_ipc_ready = true;
}

/* =========================================================================
 * demo_producer
 *
 * Runs at priority 4.  Every 500 ms it increments a counter, packs it into
 * a message, sends the message to the queue, then signals the semaphore so
 * the consumer knows an item is ready.
 * ========================================================================= */
void demo_producer(void *arg)
{
    (void)arg;

    /* Initialise IPC objects on first run. */
    if (!demo_ipc_ready) {
        demo_ipc_init();
    }

    uint32_t counter = 0u;

    for (;;) {
        sys_sleep(500);   /* produce one item every 500 ms */

        demo_message_t msg;
        msg.value = counter;
        counter++;

        mqueue_send(&producer_queue, &msg);
        ksemaphore_signal(&producer_sem);
    }
}

/* =========================================================================
 * demo_consumer
 *
 * Runs at priority 4.  Blocks on the semaphore waiting for the producer,
 * then reads the message from the queue and prints the counter value.
 * ========================================================================= */
void demo_consumer(void *arg)
{
    (void)arg;

    for (;;) {
        ksemaphore_wait(&producer_sem);   /* block until item available */

        demo_message_t msg;
        mqueue_recv(&producer_queue, &msg);

        shell_print("[consumer] received value: %u\r\n", msg.value);
    }
}

/* =========================================================================
 * demo_sensor
 *
 * Simulates a temperature sensor that samples every 2 seconds.  The "reading"
 * is a deterministic sawtooth so output is reproducible without real hardware.
 * ========================================================================= */
void demo_sensor(void *arg)
{
    (void)arg;

    uint32_t tick = 0u;

    for (;;) {
        sys_sleep(2000);   /* sample every 2 seconds */

        /* Fake temperature: 20.0 C + (tick % 50) * 0.2 C
         * Expressed as integer tenths of a degree to avoid floating point. */
        uint32_t temp_tenths = 200u + (tick % 50u) * 2u;
        uint32_t integer_part  = temp_tenths / 10u;
        uint32_t decimal_part  = temp_tenths % 10u;

        printf("[sensor] temp=%u.%u C\r\n", integer_part, decimal_part);

        tick++;
    }
}

/* =========================================================================
 * Application table
 * ========================================================================= */
const app_entry_t app_table[] = {
    { "producer",  demo_producer, 4u },
    { "consumer",  demo_consumer, 4u },
    { "sensor",    demo_sensor,   5u },
#if defined(PICOOS_WIFI_ENABLE) && defined(PICOOS_DISPLAY_ENABLE)
    { "cray-one",  cray_one,      3u },
#endif
};

const int app_table_size = (int)(sizeof(app_table) / sizeof(app_table[0]));
