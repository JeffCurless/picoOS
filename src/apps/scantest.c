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
 * scantest.c — on-device stress test for the WiFi / BT scan-result buffers
 *
 *   run scantest        test the copy-out API (wifi/bt_copy_scan_results)
 *   run scantest raw    same checks against the deprecated pointer getters
 *
 * The scan buffers are written by the CYW43 async context (a low-priority
 * IRQ on core 0) while threads read them.  This app starts real scans and
 * runs one reader thread pinned to each core.  Each reader takes snapshots
 * in a tight loop and checks them:
 *
 *   - every entry is well formed (strings terminated, fields in range)
 *   - within one scan the count never goes down
 *   - within one scan an entry never changes once seen, except that RSSI is
 *     refreshed by repeat reports and a BT name or other AD field (TX power,
 *     flags, company ID, service UUID) may go from unknown to set, once
 *
 * A torn read breaks one of those rules.  The coordinator brackets every
 * scan start with a generation counter (odd while a reset is in progress)
 * so readers only compare snapshots taken within the same scan.
 *
 * It also checks the API contract: bad arguments, `max` truncation, and that
 * starting a second scan while one is running is refused without wiping
 * the buffer.
 *
 * A last phase per radio exercises continuous mode (*_scan_start/wait/stop):
 * window numbers must increase, every entry must be well formed, WiFi
 * windows must not exceed WIFI_WINDOW_MAX_MS, the BUSY/STOPPED/ARG contract
 * must hold, and the callback mode must deliver windows on its own thread.
 *
 * Proving the test can fail: a firmware built with PICOOS_SCAN_RACE_INJECT
 * (the *_INJ images from "./build wifi") restores the old publish-before-fill
 * order and poisons each slot while it is published but unfilled.  There,
 * "run scantest raw" must FAIL and "run scantest" must still PASS.
 *
 * Do not start other scans (shell "wifi scan"/"bt scan", netmon) while it
 * runs — the readers cannot tell those resets from a race.
 */

#include "scantest.h"
#include "../kernel/wifi.h"
#ifdef PICOOS_BT_ENABLE
#include "../kernel/bluetooth.h"
#endif
#include "../kernel/sync.h"
#include "../kernel/syscall.h"
#include "../kernel/task.h"   /* CURRENT_TCB, THREAD_AFFINITY_*, task_create_thread */
#include "../kernel/mem.h"
#include "../shell/shell.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ST_WIFI_ROUNDS        5u
#define ST_BT_ROUNDS          3u
#define ST_WIFI_TIMEOUT_MS    15000u
#define ST_BT_TIMEOUT_MS      12000u
#define ST_READER_PRIO        6u     /* same as wifi-poll; below the shell */
#define ST_SNAPS_PER_YIELD    32u    /* yield every N snapshots */
#define ST_CONT_WINDOWS       10u    /* windows checked in continuous mode */
#define ST_CB_RUN_MS          3000u  /* how long callback mode runs        */
#define ST_WINDOW_SLACK_MS    50u    /* poll period + scheduling jitter    */

typedef enum { PHASE_WIFI, PHASE_BT } st_phase_t;

typedef struct {
    int8_t   affinity;
    uint32_t snaps;       /* snapshots taken                        */
    uint32_t skipped;     /* discarded: a scan reset overlapped     */
    uint32_t bad_entry;   /* entry failed the well-formed check     */
    uint32_t regressed;   /* count went down within one scan        */
    uint32_t changed;     /* a seen entry changed within one scan   */
    int      max_count;   /* largest count seen                     */
} st_reader_t;

/* Snapshot buffers live here, not on the 2 KB reader stacks. */
typedef union {
    struct { wifi_scan_result_t a[WIFI_MAX_SCAN_RESULTS], b[WIFI_MAX_SCAN_RESULTS]; } wifi;
#ifdef PICOOS_BT_ENABLE
    struct { bt_scan_result_t   a[BT_MAX_SCAN_RESULTS],   b[BT_MAX_SCAN_RESULTS];   } bt;
#endif
} st_bufs_t;

static st_reader_t       s_rd[2];
static st_bufs_t         s_buf[2];
static ksemaphore_t      s_done;
static volatile bool     s_stop;
static volatile uint32_t s_gen;       /* odd while a scan start is in progress */
static st_phase_t        s_phase;
static bool              s_raw;
static uint32_t          s_api_fail;

/* ---- snapshots ----------------------------------------------------------- */
static int snap_wifi(wifi_scan_result_t *dst)
{
    if (!s_raw) return wifi_copy_scan_results(dst, WIFI_MAX_SCAN_RESULTS);

    const wifi_scan_result_t *src = NULL;
    int n = 0;
    wifi_get_scan_results(&src, &n);
    if (n > WIFI_MAX_SCAN_RESULTS) n = WIFI_MAX_SCAN_RESULTS;
    memcpy(dst, src, (size_t)n * sizeof(dst[0]));
    return n;
}

static bool wifi_entry_ok(const wifi_scan_result_t *e)
{
    if (memchr(e->ssid, '\0', sizeof(e->ssid)) == NULL) return false;
    if (e->channel < 1u || e->channel > 14u)            return false;
    if (e->rssi < -127 || e->rssi > 0)                  return false;
    return true;
}

/* Same network, same fields — RSSI is refreshed in place by repeat reports. */
static bool wifi_entry_same(const wifi_scan_result_t *old, const wifi_scan_result_t *cur)
{
    return memcmp(old->ssid,  cur->ssid,  sizeof(cur->ssid))  == 0 &&
           memcmp(old->bssid, cur->bssid, sizeof(cur->bssid)) == 0 &&
           old->channel   == cur->channel &&
           old->auth_mode == cur->auth_mode;
}

#ifdef PICOOS_BT_ENABLE
static int snap_bt(bt_scan_result_t *dst)
{
    if (!s_raw) return bt_copy_scan_results(dst, BT_MAX_SCAN_RESULTS);

    const bt_scan_result_t *src = NULL;
    int n = 0;
    bt_get_scan_results(&src, &n);
    if (n > BT_MAX_SCAN_RESULTS) n = BT_MAX_SCAN_RESULTS;
    memcpy(dst, src, (size_t)n * sizeof(dst[0]));
    return n;
}

static bool bt_entry_ok(const bt_scan_result_t *e)
{
    static const uint8_t zero[BT_ADDR_LEN] = {0};
    if (memchr(e->name, '\0', sizeof(e->name)) == NULL)          return false;
    if (memcmp(e->addr, zero, BT_ADDR_LEN) == 0)                 return false;
    if (e->type != BT_DEVTYPE_CLASSIC && e->type != BT_DEVTYPE_BLE) return false;
    if ((int)e->dev_class < (int)BT_CLASS_UNKNOWN ||
        (int)e->dev_class > (int)BT_CLASS_OTHER)                 return false;
    return true;
}

/* Same device, same fields — except RSSI (refreshed by repeat reports).  A
 * BLE device spreads its AD fields over several packets, so the name, TX
 * power, flags, company ID and service UUID may each appear once
 * (unknown → set); once set they must not change. */
#define FILLED_ONCE(o, c, none)  ((o) == (c) || (o) == (none))

static bool bt_entry_same(const bt_scan_result_t *old, const bt_scan_result_t *cur)
{
    if (memcmp(old->addr, cur->addr, BT_ADDR_LEN) != 0) return false;
    if (old->type            != cur->type            ||
        old->dev_class       != cur->dev_class       ||
        old->class_of_device != cur->class_of_device) return false;
    if (!FILLED_ONCE(old->tx_power,     cur->tx_power,     BT_TX_POWER_UNKNOWN) ||
        !FILLED_ONCE(old->flags,        cur->flags,        BT_FLAGS_NONE)       ||
        !FILLED_ONCE(old->company_id,   cur->company_id,   BT_COMPANY_NONE)     ||
        !FILLED_ONCE(old->service_uuid, cur->service_uuid, BT_SERVICE_NONE))
        return false;
    if (old->name[0] != '\0' && strcmp(old->name, cur->name) != 0) return false;
    return true;
}
#endif

/* ---- reader thread ------------------------------------------------------- */
static void st_reader(void *arg)
{
    st_reader_t *r   = (st_reader_t *)arg;
    st_bufs_t   *buf = &s_buf[r == &s_rd[0] ? 0 : 1];
    CURRENT_TCB->affinity = r->affinity;

    bool     flip      = false;
    int      prev_n    = 0;
    uint32_t prev_gen  = 1u;       /* odd: no valid previous snapshot */

    while (!s_stop) {
        uint32_t g1 = s_gen;
        if (g1 & 1u) { sys_yield(); continue; }

        int n;
        if (s_phase == PHASE_WIFI) {
            n = snap_wifi(flip ? buf->wifi.b : buf->wifi.a);
        } else {
#ifdef PICOOS_BT_ENABLE
            n = snap_bt(flip ? buf->bt.b : buf->bt.a);
#else
            n = 0;
#endif
        }
        uint32_t g2 = s_gen;
        r->snaps++;
        if (g1 != g2) { r->skipped++; prev_gen = 1u; continue; }
        if (n > r->max_count) r->max_count = n;

        bool same_scan = (prev_gen == g1);
        if (same_scan && n < prev_n) r->regressed++;
        int common = (same_scan && prev_n < n) ? prev_n : (same_scan ? n : 0);

        if (s_phase == PHASE_WIFI) {
            const wifi_scan_result_t *cur = flip ? buf->wifi.b : buf->wifi.a;
            const wifi_scan_result_t *old = flip ? buf->wifi.a : buf->wifi.b;
            for (int i = 0; i < n; i++)
                if (!wifi_entry_ok(&cur[i])) r->bad_entry++;
            for (int i = 0; i < common; i++)
                if (!wifi_entry_same(&old[i], &cur[i])) r->changed++;
        } else {
#ifdef PICOOS_BT_ENABLE
            const bt_scan_result_t *cur = flip ? buf->bt.b : buf->bt.a;
            const bt_scan_result_t *old = flip ? buf->bt.a : buf->bt.b;
            for (int i = 0; i < n; i++)
                if (!bt_entry_ok(&cur[i])) r->bad_entry++;
            for (int i = 0; i < common; i++)
                if (!bt_entry_same(&old[i], &cur[i])) r->changed++;
#endif
        }

        prev_n   = n;
        prev_gen = g1;
        flip     = !flip;
        /* Yield, don't sleep: a sleeping thread only runs again at the next
         * 10 ms time-slice boundary, which left the readers idle >99% of
         * the time and blind to the race. */
        if ((r->snaps % ST_SNAPS_PER_YIELD) == 0u) sys_yield();
    }
    ksemaphore_signal(&s_done);
}

/* ---- helpers ------------------------------------------------------------- */
static void api_check(bool ok, const char *what)
{
    if (!ok) s_api_fail++;
    shell_print("[scantest]   %-44s %s\r\n", what, ok ? "ok" : "FAIL");
}

static bool start_readers(pcb_t *proc, st_phase_t phase)
{
    s_phase = phase;
    s_stop  = false;
    ksemaphore_init(&s_done, 0);
    memset(s_rd, 0, sizeof(s_rd));
    s_rd[0].affinity = THREAD_AFFINITY_C0;
    s_rd[1].affinity = THREAD_AFFINITY_C1;

    for (int i = 0; i < 2; i++) {
        if (task_create_thread(proc, "scantest-rd", st_reader, &s_rd[i],
                               ST_READER_PRIO, DEFAULT_STACK_SIZE) == NULL) {
            shell_print("[scantest] ERROR: could not create reader %d\r\n", i);
            s_stop = true;
            for (int j = 0; j < i; j++) ksemaphore_wait(&s_done);
            return false;
        }
    }
    return true;
}

/* Stop the readers, print their counters, return the number of errors. */
static uint32_t stop_readers(const char *label)
{
    s_stop = true;
    ksemaphore_wait(&s_done);
    ksemaphore_wait(&s_done);

    uint32_t errs = 0u;
    for (int i = 0; i < 2; i++) {
        const st_reader_t *r = &s_rd[i];
        shell_print("[scantest]   %s reader core %d: %u snaps, %u skipped, "
                    "max %d entries | bad %u, regressed %u, changed %u\r\n",
                    label, (int)r->affinity, (unsigned)r->snaps,
                    (unsigned)r->skipped, r->max_count, (unsigned)r->bad_entry,
                    (unsigned)r->regressed, (unsigned)r->changed);
        errs += r->bad_entry + r->regressed + r->changed;
    }
    return errs;
}

static bool wait_done(int (*is_done)(void), uint32_t timeout_ms)
{
    for (uint32_t t = 0u; t < timeout_ms; t += 50u) {
        if (is_done()) return true;
        sys_sleep(50);
    }
    return false;
}

/* ---- phases -------------------------------------------------------------- */
static uint32_t run_wifi_phase(pcb_t *proc, int *seen)
{
    shell_print("[scantest] WiFi: %u scans, readers on core 0 and core 1\r\n",
                (unsigned)ST_WIFI_ROUNDS);
    if (!start_readers(proc, PHASE_WIFI)) return 1u;

    for (uint32_t round = 0u; round < ST_WIFI_ROUNDS; round++) {
        s_gen++;
        int rc = wifi_scan();
        s_gen++;
        if (rc != 0) {
            shell_print("[scantest]   round %u: wifi_scan failed (%d)\r\n",
                        (unsigned)round, rc);
            s_api_fail++;
            continue;
        }
        if (round == 0u) {
            /* Not bracketed by s_gen: if this wrongly reset the buffer,
             * the readers would see the count drop. */
            api_check(wifi_scan() == WIFI_ERR_BUSY,
                      "second wifi_scan() while busy -> BUSY");
        }
        if (!wait_done(wifi_scan_is_done, ST_WIFI_TIMEOUT_MS)) {
            shell_print("[scantest]   round %u: scan timed out\r\n", (unsigned)round);
        }
        sys_sleep(200);   /* let readers see the final state */
    }

    uint32_t errs = stop_readers("wifi");
    for (int i = 0; i < 2; i++)
        if (s_rd[i].max_count > *seen) *seen = s_rd[i].max_count;

    static wifi_scan_result_t tmp[WIFI_MAX_SCAN_RESULTS];
    int all = wifi_copy_scan_results(tmp, WIFI_MAX_SCAN_RESULTS);
    api_check(wifi_copy_scan_results(NULL, 4) == WIFI_ERR_ARG,
              "wifi_copy_scan_results(NULL, 4) -> ARG");
    api_check(wifi_copy_scan_results(tmp, -1) == WIFI_ERR_ARG,
              "wifi_copy_scan_results(buf, -1) -> ARG");
    api_check(wifi_copy_scan_results(tmp, 0) == 0,
              "wifi_copy_scan_results(buf, 0) -> 0");
    api_check(wifi_copy_scan_results(tmp, 1) == (all > 0 ? 1 : 0),
              "wifi_copy_scan_results(buf, 1) truncates");
    return errs;
}


/* ---- continuous mode ----------------------------------------------------- */
static volatile uint32_t s_cb_windows;
static volatile uint32_t s_cb_bad;

static void wifi_cont_cb(const wifi_scan_list_t *l, void *ctx)
{
    (void)ctx;
    s_cb_windows++;
    if (l->count < 0 || l->count > WIFI_MAX_SCAN_RESULTS) s_cb_bad++;
}

/* Wait (up to ~2 s) for a listener thread to finish after stop, so the next
 * start is not refused as BUSY. */
static bool restart_ok(int (*start)(void))
{
    for (int t = 0; t < 40; t++) {
        if (start() == 0) return true;
        sys_sleep(50);
    }
    return false;
}

static int wifi_start_pull(void) { return wifi_scan_start(NULL, NULL); }

static uint32_t run_wifi_cont_phase(void)
{
    shell_print("[scantest] WiFi continuous: %u windows\r\n",
                (unsigned)ST_CONT_WINDOWS);
    uint32_t errs = 0u;

    if (wifi_scan_start(NULL, NULL) != 0) {
        shell_print("[scantest]   wifi_scan_start failed\r\n");
        return 1u;
    }
    api_check(wifi_scan_start(NULL, NULL) == WIFI_ERR_BUSY,
              "second wifi_scan_start() -> BUSY");
    api_check(wifi_scan() == WIFI_ERR_BUSY,
              "one-shot wifi_scan() while continuous -> BUSY");
    api_check(wifi_scan_wait(NULL) == WIFI_ERR_ARG,
              "wifi_scan_wait(NULL) -> ARG");

    uint32_t prev_seq = 0u, max_ms = 0u, total = 0u;
    for (uint32_t w = 0u; w < ST_CONT_WINDOWS; w++) {
        const wifi_scan_list_t *l;
        if (wifi_scan_wait(&l) != 0) {
            shell_print("[scantest]   wifi_scan_wait stopped early\r\n");
            errs++;
            break;
        }
        if (l->seq <= prev_seq) errs++;
        prev_seq = l->seq;
        if (l->count < 0 || l->count > WIFI_MAX_SCAN_RESULTS) { errs++; continue; }
        for (int i = 0; i < l->count; i++)
            if (!wifi_entry_ok(&l->items[i])) errs++;
        if (l->window_ms > max_ms) max_ms = l->window_ms;
        total += (uint32_t)l->count;
    }
    shell_print("[scantest]   last seq %u, longest window %u ms, avg %u networks\r\n",
                (unsigned)prev_seq, (unsigned)max_ms,
                (unsigned)(total / ST_CONT_WINDOWS));
    api_check(max_ms <= WIFI_WINDOW_MAX_MS + ST_WINDOW_SLACK_MS,
              "every window <= WIFI_WINDOW_MAX_MS");
    wifi_scan_stop();
    const wifi_scan_list_t *l;
    api_check(wifi_scan_wait(&l) == WIFI_ERR_STOPPED,
              "wifi_scan_wait() after stop -> STOPPED");

    /* Callback mode. */
    s_cb_windows = 0u;
    s_cb_bad     = 0u;
    if (wifi_scan_start(wifi_cont_cb, NULL) != 0) {
        api_check(false, "wifi_scan_start(cb) -> 0");
        return errs;
    }
    sys_sleep(ST_CB_RUN_MS);
    wifi_scan_stop();
    api_check(s_cb_windows > 0u && s_cb_bad == 0u,
              "callback mode delivered windows");
    shell_print("[scantest]   callback saw %u windows in %u ms\r\n",
                (unsigned)s_cb_windows, (unsigned)ST_CB_RUN_MS);
    api_check(restart_ok(wifi_start_pull), "restart after callback mode");
    wifi_scan_stop();
    return errs;
}

#ifdef PICOOS_BT_ENABLE
static uint32_t run_bt_phase(pcb_t *proc, int *seen)
{
    if (bt_get_state() == BT_STATE_OFF || bt_get_state() == BT_STATE_ERROR) {
        shell_print("[scantest] BT: radio not ready, skipping\r\n");
        return 0u;
    }
    shell_print("[scantest] BT: %u scans (~7 s each), readers on core 0 and core 1\r\n",
                (unsigned)ST_BT_ROUNDS);
    if (!start_readers(proc, PHASE_BT)) return 1u;

    for (uint32_t round = 0u; round < ST_BT_ROUNDS; round++) {
        s_gen++;
        int rc = bt_scan();
        s_gen++;
        if (rc != 0) {
            shell_print("[scantest]   round %u: bt_scan failed (%d)\r\n",
                        (unsigned)round, rc);
            s_api_fail++;
            continue;
        }
        if (round == 0u) {
            api_check(bt_scan() == -1, "second bt_scan() while busy -> -1");
        }
        if (!wait_done(bt_scan_is_done, ST_BT_TIMEOUT_MS)) {
            shell_print("[scantest]   round %u: scan timed out\r\n", (unsigned)round);
        }
        sys_sleep(200);
    }

    uint32_t errs = stop_readers("bt");
    for (int i = 0; i < 2; i++)
        if (s_rd[i].max_count > *seen) *seen = s_rd[i].max_count;

    static bt_scan_result_t tmp[BT_MAX_SCAN_RESULTS];
    int all = bt_copy_scan_results(tmp, BT_MAX_SCAN_RESULTS);
    api_check(bt_copy_scan_results(NULL, 4) == -1,
              "bt_copy_scan_results(NULL, 4) -> -1");
    api_check(bt_copy_scan_results(tmp, 0) == 0,
              "bt_copy_scan_results(buf, 0) -> 0");
    api_check(bt_copy_scan_results(tmp, 1) == (all > 0 ? 1 : 0),
              "bt_copy_scan_results(buf, 1) truncates");
    return errs;
}

static void bt_cont_cb(const bt_scan_list_t *l, void *ctx)
{
    (void)ctx;
    s_cb_windows++;
    if (l->count < 0 || l->count > BT_MAX_SCAN_RESULTS) s_cb_bad++;
}

static int bt_start_pull(void) { return bt_scan_start(NULL, NULL); }

static uint32_t run_bt_cont_phase(void)
{
    if (bt_get_state() == BT_STATE_OFF || bt_get_state() == BT_STATE_ERROR) {
        shell_print("[scantest] BT continuous: radio not ready, skipping\r\n");
        return 0u;
    }
    shell_print("[scantest] BT continuous: %u windows\r\n",
                (unsigned)ST_CONT_WINDOWS);
    uint32_t errs = 0u;

    if (bt_scan_start(NULL, NULL) != 0) {
        shell_print("[scantest]   bt_scan_start failed\r\n");
        return 1u;
    }
    api_check(bt_scan_start(NULL, NULL) == BT_ERR_BUSY,
              "second bt_scan_start() -> BUSY");
    api_check(bt_scan() == -1, "one-shot bt_scan() while continuous -> -1");
    api_check(bt_scan_wait(NULL) == BT_ERR_ARG, "bt_scan_wait(NULL) -> ARG");

    uint32_t prev_seq = 0u, max_ms = 0u, total = 0u, named = 0u;
    for (uint32_t w = 0u; w < ST_CONT_WINDOWS; w++) {
        const bt_scan_list_t *l;
        if (bt_scan_wait(&l) != 0) {
            shell_print("[scantest]   bt_scan_wait stopped early\r\n");
            errs++;
            break;
        }
        if (l->seq <= prev_seq) errs++;
        prev_seq = l->seq;
        if (l->count < 0 || l->count > BT_MAX_SCAN_RESULTS) { errs++; continue; }
        for (int i = 0; i < l->count; i++) {
            if (!bt_entry_ok(&l->items[i])) errs++;
            if (l->items[i].name[0] != '\0') named++;
        }
        if (l->window_ms > max_ms) max_ms = l->window_ms;
        total += (uint32_t)l->count;
    }
    shell_print("[scantest]   last seq %u, longest window %u ms, avg %u devices, "
                "%u named sightings\r\n", (unsigned)prev_seq, (unsigned)max_ms,
                (unsigned)(total / ST_CONT_WINDOWS), (unsigned)named);
    api_check(max_ms <= BT_WINDOW_MS + ST_WINDOW_SLACK_MS,
              "every window <= BT_WINDOW_MS");
    bt_scan_stop();
    const bt_scan_list_t *l;
    api_check(bt_scan_wait(&l) == BT_ERR_STOPPED,
              "bt_scan_wait() after stop -> STOPPED");

    s_cb_windows = 0u;
    s_cb_bad     = 0u;
    if (bt_scan_start(bt_cont_cb, NULL) != 0) {
        api_check(false, "bt_scan_start(cb) -> 0");
        return errs;
    }
    sys_sleep(ST_CB_RUN_MS);
    bt_scan_stop();
    api_check(s_cb_windows > 0u && s_cb_bad == 0u,
              "callback mode delivered windows");
    shell_print("[scantest]   callback saw %u windows in %u ms\r\n",
                (unsigned)s_cb_windows, (unsigned)ST_CB_RUN_MS);
    api_check(restart_ok(bt_start_pull), "restart after callback mode");
    bt_scan_stop();
    return errs;
}
#endif

/* ---- entry point --------------------------------------------------------- */
void scantest(void *arg)
{
    s_raw = false;
    if (arg != NULL) {
        s_raw = (strcmp((const char *)arg, "raw") == 0);
        kfree(arg);
    }
    s_api_fail = 0u;

    shell_print("[scantest] mode: %s\r\n", s_raw
                ? "raw (deprecated *_get_scan_results, no lock)"
                : "copy (*_copy_scan_results)");
    shell_print("[scantest] do not start other scans while this runs\r\n");
#ifdef PICOOS_SCAN_RACE_INJECT
    shell_print("[scantest] TEST BUILD: PICOOS_SCAN_RACE_INJECT is on — expect "
                "raw mode to FAIL and copy mode to PASS\r\n");
#endif

    pcb_t *proc = task_find_process((uint32_t)sys_getpid());
    int wifi_seen = 0;
    int bt_seen   = 0;
    (void)bt_seen;

    uint32_t wifi_errs = run_wifi_phase(proc, &wifi_seen);
    uint32_t bt_errs   = 0u;
#ifdef PICOOS_BT_ENABLE
    bt_errs = run_bt_phase(proc, &bt_seen);
    bt_scan_stop();   /* bt_scan() leaves the BLE scan running */
#endif
    /* Continuous mode does not use the raw getters, so run it only once. */
    uint32_t cont_errs = 0u;
    if (!s_raw) {
        cont_errs += run_wifi_cont_phase();
#ifdef PICOOS_BT_ENABLE
        cont_errs += run_bt_cont_phase();
#endif
    }

    shell_print("[scantest] ----------------------------------------\r\n");
    shell_print("[scantest] wifi: %u data errors, max %d networks\r\n",
                (unsigned)wifi_errs, wifi_seen);
#ifdef PICOOS_BT_ENABLE
    shell_print("[scantest] bt  : %u data errors, max %d devices\r\n",
                (unsigned)bt_errs, bt_seen);
#endif
    if (!s_raw) {
        shell_print("[scantest] cont: %u data errors\r\n", (unsigned)cont_errs);
    }
    shell_print("[scantest] api : %u failures\r\n", (unsigned)s_api_fail);
    bool idle = (wifi_seen == 0);
#ifdef PICOOS_BT_ENABLE
    idle = idle || (bt_seen == 0);
#endif
    if (idle) {
        shell_print("[scantest] WARNING: a phase saw no results, so it could "
                    "not exercise the race\r\n");
    }
    bool pass = (wifi_errs + bt_errs + cont_errs + s_api_fail) == 0u;
    shell_print("[scantest] RESULT: %s\r\n", pass ? "PASS" : "FAIL");
}
