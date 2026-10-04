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

#ifdef PICOOS_WIFI_ENABLE

#include "kernel/arch.h"
#include "kernel/wifi.h"
#include "kernel/scanbuf.h"
#include "kernel/sync.h"
#include "kernel/task.h"
#include "kernel/syscall.h"
#include "shell/shell.h"
#ifdef PICOOS_BT_ENABLE
#include "kernel/bluetooth.h"   /* bt_scan_poll() */
#endif
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __arm__
#include "lwip/udp.h"
#include "lwip/igmp.h"
#include "lwip/pbuf.h"
#endif

/* ---- state --------------------------------------------------------------- *
 * g_scan[] and g_scan_count are written by scan_result_cb(), which runs in the
 * CYW43 async context: a low-priority IRQ on core 0 (this build links
 * pico_cyw43_arch_lwip_threadsafe_background, so cyw43_arch_poll() does
 * nothing).  Thread-side code touches them only while holding the async
 * context lock (cyw43_arch_lwip_begin/end), which defers the callback until
 * the lock is released.  The lock's owner is the core, not the thread, so it
 * does not order two threads on the same core against each other. */
static volatile wifi_state_t  g_state      = WIFI_STATE_DOWN;
static wifi_scan_result_t     g_scan[WIFI_MAX_SCAN_RESULTS];
static volatile int           g_scan_count = 0;
static volatile bool          g_scan_done  = false;

/* ---- continuous scanning state ------------------------------------------- *
 * Three lists rotate through the fill / ready / held roles tracked by g_sb
 * (see scanbuf.h).  scan_result_cb() reads g_sb.fill and writes that list in
 * IRQ context.  Threads change g_sb only while holding g_cont_lock, and
 * publish (the only operation that moves `fill`) also holds the async context
 * lock so the IRQ cannot run mid-swap.  The kmutex is what orders threads:
 * the async context lock is owned by a core, so on its own it would let two
 * threads on the same core into the critical section together. */
#define CONT_EV_READY  (1u << 0)   /* a window was published   */
#define CONT_EV_STOP   (1u << 1)   /* continuous mode stopped  */

static wifi_scan_list_t g_lists[3];
static scanbuf_t        g_sb;
static kmutex_t         g_cont_lock;
static event_flags_t    g_cont_ev;
static volatile bool    g_cont         = false;  /* continuous mode on     */
static volatile bool    g_pass_cont    = false;  /* IRQ target: g_lists     */
static bool             g_pass_running = false;  /* a pass we started      */
static uint64_t         g_window_start_us;
static uint32_t         g_owner_pid;             /* subscriber's process   */
static wifi_scan_cb_t   g_cb;
static void            *g_cb_ctx;
static uint32_t         g_listen_tid;            /* 0 = no listener thread */

/* ---- firmware BSS record -------------------------------------------------- *
 * cyw43_ev_scan_result_t is a view onto the raw scan event: 12 bytes of event
 * header, then the firmware's bss_info record.  The driver names only a few of
 * its fields, so this mirrors the fixed part (cyw43_scan_result_internal_t in
 * cyw43_ll.c) to read the rest.  The asserts tie it to the SDK's view, so a
 * driver whose layout differs fails to build instead of returning garbage. */
typedef struct {
    uint32_t version;
    uint32_t length;          /* bytes in the record, IEs included */
    uint8_t  bssid[6];
    uint16_t beacon_period;   /* TU */
    uint16_t capability;
    uint8_t  ssid_len;
    uint8_t  ssid[32];
    uint32_t rateset_count;
    uint8_t  rateset_rates[16];
    uint16_t chanspec;
    uint16_t atim_window;
    uint8_t  dtim_period;     /* overwritten by the driver with auth_mode */
    int16_t  rssi;
    int8_t   phy_noise;
    uint8_t  n_cap;
    uint32_t nbss_cap;
    uint8_t  ctl_ch;
    uint32_t reserved32[1];
    uint8_t  flags;
    uint8_t  reserved[3];
    uint8_t  basic_mcs[16];
    uint16_t ie_offset;
    uint32_t ie_length;
    int16_t  snr;
} wifi_bss_info_t;

#define BSS_INFO_OFS    12u   /* buflen, version, sync_id, bss_count */
#define BSS_HT_CAP_40MHZ 0x0002u

#ifdef __arm__
#define BSS_ASSERT_AT(f, g) \
    _Static_assert(BSS_INFO_OFS + offsetof(wifi_bss_info_t, f) == \
                   offsetof(cyw43_ev_scan_result_t, g), \
                   "CYW43 bss_info layout changed: " #f)
BSS_ASSERT_AT(bssid,       bssid);
BSS_ASSERT_AT(ssid_len,    ssid_len);
BSS_ASSERT_AT(ssid,        ssid);
BSS_ASSERT_AT(chanspec,    channel);
BSS_ASSERT_AT(dtim_period, auth_mode);
BSS_ASSERT_AT(rssi,        rssi);
#endif

/* Copy the fields cyw43_ev_scan_result_t does not name.  Records too short to
 * hold them (none seen in practice) leave the fields at 0, "unknown". */
static void fill_extra(wifi_scan_result_t *e, const cyw43_ev_scan_result_t *r)
{
    const wifi_bss_info_t *bss =
        (const wifi_bss_info_t *)((const uint8_t *)r + BSS_INFO_OFS);

    e->beacon_tu  = bss->beacon_period;
    e->capability = bss->capability;

    uint8_t max = 0;
    uint32_t n = bss->rateset_count < 16u ? bss->rateset_count : 16u;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t rate = bss->rateset_rates[i] & 0x7fu;   /* bit 7 = basic */
        if (rate > max) max = rate;
    }
    e->max_rate = max;
    e->phy      = max > 22u ? WIFI_PHY_OFDM : 0u;       /* above 11 Mb/s */

    if (bss->length < offsetof(wifi_bss_info_t, snr) + sizeof(bss->snr)) {
        e->noise = 0;
        e->snr   = 0;
        return;
    }
    e->noise = bss->phy_noise;
    e->snr   = (int8_t)(bss->snr > 127 ? 127 : bss->snr < 0 ? 0 : bss->snr);
    if (bss->n_cap) {
        e->phy |= WIFI_PHY_HT;
        if (bss->nbss_cap & BSS_HT_CAP_40MHZ) e->phy |= WIFI_PHY_HT40;
    }
}

/* ---- scan callback (called from cyw43 poll context) ---------------------- */
static int scan_result_cb(void *env, const cyw43_ev_scan_result_t *r)
{
    (void)env;

    /* A one-shot scan fills g_scan; a continuous pass fills the current
     * fill list.  The target is chosen when the pass starts. */
    wifi_scan_result_t *buf = g_scan;
    volatile int       *cnt = &g_scan_count;
    if (g_pass_cont) {
        wifi_scan_list_t *l = &g_lists[g_sb.fill];
        buf = l->items;
        cnt = (volatile int *)&l->count;
    }
    int i = *cnt;

    /* The chip reports each BSS many times per scan (every beacon / probe
     * response it hears).  Refresh the existing slot instead of taking a new
     * one, or the buffer fills with repeats and later networks are lost.
     * rssi >= 0 is a bogus reading the firmware sometimes sends; skip it. */
    for (int k = 0; k < i; k++) {
        if (memcmp(buf[k].bssid, r->bssid, 6) == 0) {
            if (r->rssi < 0) {
                buf[k].rssi = r->rssi;
                fill_extra(&buf[k], r);
            }
            return 0;
        }
    }
    if (i >= WIFI_MAX_SCAN_RESULTS) return 0;

    wifi_scan_result_t *e = &buf[i];
#ifdef PICOOS_SCAN_RACE_INJECT
    /* TEST ONLY: the old bug, widened.  Publish the slot, poison it
     * (unterminated SSID, channel 255), and hold it there for a while. */
    *cnt = i + 1;
    memset(e, 0xFF, sizeof(*e));
    busy_wait_us_32(50);
#endif

    /* Fill the slot first, then publish it by bumping the count. */
    int len = r->ssid_len < 32 ? r->ssid_len : 32;
    memcpy(e->ssid, r->ssid, len);
    e->ssid[len] = '\0';
    memcpy(e->bssid, r->bssid, 6);
    e->rssi      = r->rssi;
    e->channel   = r->channel;
    e->auth_mode = (uint8_t)r->auth_mode;
    fill_extra(e, r);
#ifndef PICOOS_SCAN_RACE_INJECT
    __dmb();
    *cnt = i + 1;
#endif
    return 0;
}

/* ---- public API ---------------------------------------------------------- */
wifi_state_t wifi_get_state(void) { return g_state; }

int wifi_scan(void)
{
    cyw43_wifi_scan_options_t opts = {0};
    cyw43_arch_lwip_begin();

    /* Refuse rather than reset the buffer under a scan someone else started
     * (e.g. "wifi scan" at the shell while an app is scanning). */
    if (g_cont || cyw43_wifi_scan_active(&cyw43_state)) {
        cyw43_arch_lwip_end();
        return WIFI_ERR_BUSY;
    }

    g_scan_count = 0;
    g_scan_done  = false;
    g_pass_cont  = false;
    int rc = cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result_cb);

    /* Enter SCANNING only once the scan is active, so wifi-poll never sees
     * SCANNING with no scan running and reports it done too early. */
    g_state = (rc == 0) ? WIFI_STATE_SCANNING : WIFI_STATE_DOWN;
    cyw43_arch_lwip_end();
    return rc;
}

int wifi_connect(const char *ssid, const char *password)
{
    g_state = WIFI_STATE_CONNECTING;
    uint32_t auth = (password && *password) ? CYW43_AUTH_WPA2_AES_PSK
                                             : CYW43_AUTH_OPEN;
    size_t ssid_len = strlen(ssid);
    size_t key_len  = (password && *password) ? strlen(password) : 0;

    /* The CYW43 chip occasionally returns CYW43_LINK_BADAUTH (-3) on the
     * first join attempt even with correct credentials — a known transient
     * in the driver.  Retry up to 3 times with a short back-off. */
    for (int attempt = 1; attempt <= 3; attempt++) {
        cyw43_arch_lwip_begin();
        int rc = cyw43_wifi_join(&cyw43_state,
                                 ssid_len, (const uint8_t *)ssid,
                                 key_len,  (const uint8_t *)password,
                                 auth, NULL, 0);
        cyw43_arch_lwip_end();
        if (rc != 0) {
            g_state = WIFI_STATE_ERROR;
            return rc;
        }

        /* Wait for lwIP to complete association and DHCP (CYW43_LINK_UP).
         * 200 iterations × 50 ms = 10 s per attempt. */
        int status = CYW43_LINK_DOWN;
        for (int i = 0; i < 200; i++) {
            cyw43_arch_lwip_begin();
            status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            cyw43_arch_lwip_end();
            if (status == CYW43_LINK_UP) {
                g_state = WIFI_STATE_UP;
                return 0;
            }
            if (status < 0) break;
            sys_sleep(50);
        }

        if (status == CYW43_LINK_UP) break;   /* connected — exit retry loop */

        printf("[wifi] connect attempt %d failed (status %d)%s\r\n",
               attempt, status, attempt < 3 ? " — retrying..." : "");

        /* Leave the network before re-joining to reset chip state. */
        cyw43_arch_lwip_begin();
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        cyw43_arch_lwip_end();
        sys_sleep(500);
    }

    g_state = WIFI_STATE_ERROR;
    return -1;
}

int wifi_disconnect(void)
{
    cyw43_arch_lwip_begin();
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    cyw43_arch_lwip_end();
    g_state = WIFI_STATE_DOWN;
    return 0;
}

int wifi_scan_is_done(void)
{
    return g_scan_done ? 1 : 0;
}

int wifi_copy_scan_results(wifi_scan_result_t *buf, int max)
{
    if (buf == NULL || max < 0) return WIFI_ERR_ARG;
    cyw43_arch_lwip_begin();
    int n = g_scan_count < max ? g_scan_count : max;
    memcpy(buf, g_scan, (size_t)n * sizeof(buf[0]));
    cyw43_arch_lwip_end();
    return n;
}

/* Deprecated — see wifi.h.  Kept as a teaching example of a buffer shared
 * with an IRQ-context writer and no lock. */
int wifi_get_scan_results(const wifi_scan_result_t **out_results, int *out_count)
{
    *out_results = g_scan;
    *out_count   = (int)g_scan_count;
    return 0;
}

/* ---- continuous scanning ------------------------------------------------- */

/* Firmware scan dwell times.  cyw43_wifi_scan() ignores its options and asks
 * the firmware for its defaults (-1), so the only way to shorten a pass is to
 * change those defaults with WLC ioctls.  A pass is roughly
 *   11 active channels x ACTIVE + 2 passive channels x PASSIVE + overhead.
 * The old values are read at start and put back at stop, so one-shot scans
 * keep their behaviour.  The ioctl numbers are Broadcom's (wlioctl.h). */
#define WLC_GET_SCAN_CHANNEL_TIME   184u   /* active dwell, associated   */
#define WLC_GET_SCAN_UNASSOC_TIME   186u   /* active dwell, unassociated */
#define WLC_GET_SCAN_PASSIVE_TIME   257u   /* passive dwell              */
#define WIFI_DWELL_ACTIVE_MS        25u
#define WIFI_DWELL_PASSIVE_MS       60u

typedef struct {
    uint32_t get;      /* GET ioctl; SET is get + 1 */
    uint32_t fast;     /* value used in continuous mode */
    uint32_t saved;    /* firmware value before start   */
    bool     have;     /* saved is valid                */
} wifi_dwell_t;

static wifi_dwell_t g_dwell[] = {
    { WLC_GET_SCAN_CHANNEL_TIME, WIFI_DWELL_ACTIVE_MS,  0u, false },
    { WLC_GET_SCAN_UNASSOC_TIME, WIFI_DWELL_ACTIVE_MS,  0u, false },
    { WLC_GET_SCAN_PASSIVE_TIME, WIFI_DWELL_PASSIVE_MS, 0u, false },
};
#define WIFI_N_DWELL  (sizeof(g_dwell) / sizeof(g_dwell[0]))

/* cyw43_ioctl() encodes the direction in bit 0 of cmd: (wlc << 1) | set. */
static int wlc_get_u32(uint32_t wlc, uint32_t *val)
{
    uint8_t b[4] = {0};
    int rc = cyw43_ioctl(&cyw43_state, wlc << 1, sizeof(b), b, CYW43_ITF_STA);
    *val = (uint32_t)b[0] | (uint32_t)b[1] << 8 |
           (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return rc;
}

static void wlc_set_u32(uint32_t wlc, uint32_t val)
{
    uint8_t b[4] = { (uint8_t)val, (uint8_t)(val >> 8),
                     (uint8_t)(val >> 16), (uint8_t)(val >> 24) };
    cyw43_ioctl(&cyw43_state, (wlc << 1) | 1u, sizeof(b), b, CYW43_ITF_STA);
}

/* Caller holds the async context lock. */
static void dwell_fast(void)
{
    for (unsigned i = 0; i < WIFI_N_DWELL; i++) {
        g_dwell[i].have = (wlc_get_u32(g_dwell[i].get, &g_dwell[i].saved) == 0);
        if (g_dwell[i].have) wlc_set_u32(g_dwell[i].get + 1u, g_dwell[i].fast);
    }
}

static void dwell_restore(void)
{
    for (unsigned i = 0; i < WIFI_N_DWELL; i++) {
        if (g_dwell[i].have) wlc_set_u32(g_dwell[i].get + 1u, g_dwell[i].saved);
        g_dwell[i].have = false;
    }
}

static bool owner_gone(void)
{
    pcb_t *p = task_find_process(g_owner_pid);
    return p == NULL || !p->alive;
}

static bool listener_alive(void)
{
    if (g_listen_tid == 0u) return false;
    tcb_t *t = task_find_thread(g_listen_tid);
    return t != NULL && t->state != THREAD_ZOMBIE;
}

/* Caller holds g_cont_lock.  The pass under way (if any) keeps writing the
 * fill list, which nobody reads until the next start resets it. */
static void cont_stop_locked(void)
{
    cyw43_arch_lwip_begin();
    if (g_cont) {
        g_cont = false;
        dwell_restore();
    }
    cyw43_arch_lwip_end();
    event_flags_set(&g_cont_ev, CONT_EV_STOP);
}

/* Callback mode: one of these runs in the subscriber's process. */
static void wifi_listen_thread(void *arg)
{
    (void)arg;
    const wifi_scan_list_t *l;
    while (wifi_scan_wait(&l) == 0) {
        g_cb(l, g_cb_ctx);
    }
    kmutex_lock(&g_cont_lock);
    if (g_listen_tid == CURRENT_TCB->tid) g_listen_tid = 0u;
    kmutex_unlock(&g_cont_lock);
}

int wifi_scan_start(wifi_scan_cb_t cb, void *ctx)
{
    uint32_t me = (uint32_t)sys_getpid();

    kmutex_lock(&g_cont_lock);
    if (g_cont && owner_gone()) cont_stop_locked();   /* reclaim a dead owner */
    if (g_cont || listener_alive()) {
        kmutex_unlock(&g_cont_lock);
        return WIFI_ERR_BUSY;
    }

    cyw43_arch_lwip_begin();
    /* A one-shot scan owns the radio until it finishes.  A continuous pass
     * left over from an earlier stop is fine: it runs into the new lists. */
    if (g_state == WIFI_STATE_SCANNING ||
        (cyw43_wifi_scan_active(&cyw43_state) && !g_pass_cont)) {
        cyw43_arch_lwip_end();
        kmutex_unlock(&g_cont_lock);
        return WIFI_ERR_BUSY;
    }

    scanbuf_init(&g_sb);
    for (int i = 0; i < 3; i++) g_lists[i].count = 0;
    g_pass_cont = true;
    dwell_fast();
    if (!cyw43_wifi_scan_active(&cyw43_state)) {
        cyw43_wifi_scan_options_t opts = {0};
        int rc = cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result_cb);
        if (rc != 0) {
            dwell_restore();
            cyw43_arch_lwip_end();
            kmutex_unlock(&g_cont_lock);
            return rc;
        }
        g_pass_running = true;
    }
    g_window_start_us = time_us_64();
    g_owner_pid       = me;
    g_cb              = cb;
    g_cb_ctx          = ctx;
    g_cont            = true;
    event_flags_clear(&g_cont_ev, CONT_EV_READY | CONT_EV_STOP);
    cyw43_arch_lwip_end();

    if (cb != NULL) {
        pcb_t *proc = task_find_process(me);
        tcb_t *t = proc ? task_create_thread(proc, "wifi-listen",
                                             wifi_listen_thread, NULL,
                                             CURRENT_TCB->priority,
                                             DEFAULT_STACK_SIZE)
                        : NULL;
        if (t == NULL) {
            cont_stop_locked();
            kmutex_unlock(&g_cont_lock);
            return WIFI_ERR_NOMEM;
        }
        g_listen_tid = t->tid;
    }
    kmutex_unlock(&g_cont_lock);
    return 0;
}

void wifi_scan_stop(void)
{
    kmutex_lock(&g_cont_lock);
    cont_stop_locked();
    kmutex_unlock(&g_cont_lock);
}

bool wifi_scan_running(void) { return g_cont; }

int wifi_scan_wait(const wifi_scan_list_t **list)
{
    if (list == NULL) return WIFI_ERR_ARG;

    for (;;) {
        event_flags_wait(&g_cont_ev, CONT_EV_READY | CONT_EV_STOP, false);

        /* Clear before taking: a publish that lands after the take sets the
         * flag again, so it is never missed.  A wake-up that finds nothing
         * fresh just waits again. */
        kmutex_lock(&g_cont_lock);
        event_flags_clear(&g_cont_ev, CONT_EV_READY);
        bool running = g_cont;
        /* take moves only ready/held, never fill, so the IRQ writer does
         * not care and the async context lock is not needed. */
        bool took = running && scanbuf_take(&g_sb);
        uint8_t held = g_sb.held;
        kmutex_unlock(&g_cont_lock);

        if (!running) return WIFI_ERR_STOPPED;
        if (took) {
            *list = &g_lists[held];
            return 0;
        }
    }
}

/* Called by wifi-poll every 10 ms: notice the end of a pass, publish the
 * window, start the next pass, and stop if the subscriber has died. */
static void cont_poll(void)
{
    if (!g_cont && !g_pass_running) return;

    bool published = false;
    kmutex_lock(&g_cont_lock);
    if (g_cont && owner_gone()) cont_stop_locked();

    uint64_t now = time_us_64();
    cyw43_arch_lwip_begin();
    bool active = cyw43_wifi_scan_active(&cyw43_state);
    bool ended  = g_pass_running && !active;
    if (ended) g_pass_running = false;

    if (g_cont) {
        uint32_t ms = (uint32_t)((now - g_window_start_us) / 1000u);
        if (ended || ms >= WIFI_WINDOW_MAX_MS) {
            g_lists[g_sb.fill].window_ms = ms;
            scanbuf_publish(&g_sb);
            g_lists[g_sb.ready].seq     = g_sb.seq;
            g_lists[g_sb.ready].dropped = g_sb.dropped;
            g_lists[g_sb.fill].count    = 0;
            g_window_start_us = now;
            published = true;
        }
        if (!active) {
            cyw43_wifi_scan_options_t opts = {0};
            g_pass_cont = true;
            if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result_cb) == 0)
                g_pass_running = true;
        }
    }
    cyw43_arch_lwip_end();
    kmutex_unlock(&g_cont_lock);

    if (published) event_flags_set(&g_cont_ev, CONT_EV_READY);
}

const char *wifi_get_ip_str(void)
{
    static char buf[16];
#ifdef __arm__
    const ip4_addr_t *ip = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    snprintf(buf, sizeof(buf), "%s", ip4addr_ntoa(ip));
#else
    strncpy(buf, "0.0.0.0", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
#endif
    return buf;
}

int wifi_get_mac(uint8_t mac[6])
{
    if (mac == NULL) return WIFI_ERR_ARG;
#ifdef __arm__
    cyw43_arch_lwip_begin();
    int rc = cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    cyw43_arch_lwip_end();
    return rc == 0 ? 0 : WIFI_ERR_ARG;
#else
    memset(mac, 0, 6);
    return 0;
#endif
}

/* ---- multicast UDP ------------------------------------------------------- */
#ifdef __arm__
typedef struct {
    struct udp_pcb     *pcb;      /* NULL = slot free                  */
    ip4_addr_t          group;
    uint16_t            port;
    wifi_mcast_rx_cb_t  cb;
    void               *ctx;
} mcast_sock_t;

/* Guarded by the lwIP lock (cyw43_arch_lwip_begin/end). */
static mcast_sock_t g_mcast[WIFI_MCAST_MAX_SOCKETS];

/* lwIP receive hook — runs in the CYW43 async context (low-priority IRQ on
 * core 0), so it must not block.  Copies the pbuf chain into a flat NUL-terminated buffer and hands it to
 * the application callback. */
static void mcast_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                       const ip_addr_t *src, u16_t port)
{
    (void)pcb;
    (void)port;
    mcast_sock_t *ms = (mcast_sock_t *)arg;

    if (p == NULL) return;

    char buf[WIFI_MCAST_MAX_PAYLOAD + 1];
    u16_t len = p->tot_len < WIFI_MCAST_MAX_PAYLOAD
                    ? p->tot_len : (u16_t)WIFI_MCAST_MAX_PAYLOAD;
    pbuf_copy_partial(p, buf, len, 0);
    buf[len] = '\0';
    pbuf_free(p);

    if (ms->cb != NULL) {
        ms->cb(buf, len, ipaddr_ntoa(src), ms->ctx);
    }
}

int wifi_mcast_open(const char *group, uint16_t port,
                    wifi_mcast_rx_cb_t cb, void *ctx)
{
    ip4_addr_t grp;
    if (group == NULL || !ip4addr_aton(group, &grp)) return WIFI_ERR_ARG;
    if (g_state != WIFI_STATE_UP) return WIFI_ERR_ARG;

    cyw43_arch_lwip_begin();

    int sock = -1;
    for (int i = 0; i < WIFI_MCAST_MAX_SOCKETS; i++) {
        if (g_mcast[i].pcb == NULL) { sock = i; break; }
    }
    if (sock < 0) {
        cyw43_arch_lwip_end();
        return WIFI_ERR_NOSOCK;
    }

    struct udp_pcb *pcb = udp_new();
    if (pcb == NULL) {
        cyw43_arch_lwip_end();
        return WIFI_ERR_NOMEM;
    }
    if (udp_bind(pcb, IP_ADDR_ANY, port) != ERR_OK) {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        return WIFI_ERR_BIND;
    }
    const ip4_addr_t *me = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    if (igmp_joingroup(me, &grp) != ERR_OK) {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        return WIFI_ERR_JOIN;
    }

    mcast_sock_t *ms = &g_mcast[sock];
    ms->pcb   = pcb;
    ms->group = grp;
    ms->port  = port;
    ms->cb    = cb;
    ms->ctx   = ctx;
    udp_recv(pcb, mcast_recv, ms);

    cyw43_arch_lwip_end();
    return sock;
}

int wifi_mcast_send(int sock, const void *data, uint16_t len)
{
    if (sock < 0 || sock >= WIFI_MCAST_MAX_SOCKETS || data == NULL) {
        return WIFI_ERR_ARG;
    }

    cyw43_arch_lwip_begin();
    mcast_sock_t *ms = &g_mcast[sock];
    if (ms->pcb == NULL) {
        cyw43_arch_lwip_end();
        return WIFI_ERR_ARG;
    }
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (p == NULL) {
        cyw43_arch_lwip_end();
        return WIFI_ERR_NOMEM;
    }
    memcpy(p->payload, data, len);
    err_t err = udp_sendto(ms->pcb, p, (const ip_addr_t *)&ms->group, ms->port);
    pbuf_free(p);
    cyw43_arch_lwip_end();

    return err == ERR_OK ? 0 : WIFI_ERR_SEND;
}

void wifi_mcast_close(int sock)
{
    if (sock < 0 || sock >= WIFI_MCAST_MAX_SOCKETS) return;

    cyw43_arch_lwip_begin();
    mcast_sock_t *ms = &g_mcast[sock];
    if (ms->pcb != NULL) {
        const ip4_addr_t *me = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
        igmp_leavegroup(me, &ms->group);
        udp_remove(ms->pcb);
        ms->pcb = NULL;
        ms->cb  = NULL;
        ms->ctx = NULL;
    }
    cyw43_arch_lwip_end();
}
#else  /* host / LSP build — no network stack */
int wifi_mcast_open(const char *group, uint16_t port,
                    wifi_mcast_rx_cb_t cb, void *ctx)
{
    (void)group; (void)port; (void)cb; (void)ctx;
    return WIFI_ERR_ARG;
}
int wifi_mcast_send(int sock, const void *data, uint16_t len)
{
    (void)sock; (void)data; (void)len;
    return WIFI_ERR_ARG;
}
void wifi_mcast_close(int sock) { (void)sock; }
#endif /* __arm__ */

/* ---- poll thread --------------------------------------------------------- */
static void wifi_poll_thread(void *arg)
{
    (void)arg;
    for (;;) {
        cyw43_arch_poll();

        cyw43_arch_lwip_begin();
        /* If scan was active and is now done, update state. */
        if (g_state == WIFI_STATE_SCANNING &&
            !cyw43_wifi_scan_active(&cyw43_state)) {
            g_scan_done = true;
            g_state = WIFI_STATE_DOWN;
        }
        /* Detect unexpected link drop when connected. */
        if (g_state == WIFI_STATE_UP &&
            cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
            g_state = WIFI_STATE_DOWN;
        }
        cyw43_arch_lwip_end();

        cont_poll();
#ifdef PICOOS_BT_ENABLE
        bt_scan_poll();
#endif

        sys_sleep(10);   /* 10 ms between polls */
    }
}

/* ---- shell command ------------------------------------------------------- */
static const char *state_str(wifi_state_t s)
{
    switch (s) {
        case WIFI_STATE_DOWN:       return "down";
        case WIFI_STATE_SCANNING:   return "scanning";
        case WIFI_STATE_CONNECTING: return "connecting";
        case WIFI_STATE_UP:         return "up";
        case WIFI_STATE_ERROR:      return "error";
        default:                    return "unknown";
    }
}

static int cmd_wifi(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : "status";

    if (strcmp(sub, "status") == 0) {
        shell_print("WiFi state: %s%s\r\n", state_str(g_state),
                    g_cont ? " (continuous scan on)" : "");
        if (g_state == WIFI_STATE_UP) {
            shell_print("IP address: %s\r\n", wifi_get_ip_str());
        }
        return 0;
    }

    if (strcmp(sub, "scan") == 0) {
        shell_print("Scanning...\r\n");
        int rc = wifi_scan();
        if (rc == WIFI_ERR_BUSY) {
            shell_print("A scan is already running\r\n");
            return -1;
        }
        if (rc != 0) {
            shell_print("Scan failed\r\n");
            return -1;
        }
        /* Wait for scan to complete (poll thread updates g_scan_done). */
        for (int t = 0; !g_scan_done && t < 100; t++)
            sys_sleep(100);
        if (!g_scan_done) {
            g_state = WIFI_STATE_DOWN;
            shell_print("Scan timed out\r\n");
            return -1;
        }
        /* Print from a snapshot: shell_print can block on USB, and the
         * async context lock must not be held that long. */
        static wifi_scan_result_t res[WIFI_MAX_SCAN_RESULTS];
        int n = wifi_copy_scan_results(res, WIFI_MAX_SCAN_RESULTS);
        if (n <= 0) {
            shell_print("No networks found\r\n");
        } else {
            shell_print("%-32s  %5s  %5s  %3s  Ch  Auth  Phy  Bcn  Rate\r\n",
                        "SSID", "RSSI", "Noise", "SNR");
            for (int i = 0; i < n; i++) {
                shell_print("%-32s  %5d  %5d  %3d  %2u  %4u  %3s  %3u  %4u\r\n",
                    res[i].ssid, (int)res[i].rssi, (int)res[i].noise,
                    (int)res[i].snr, res[i].channel, res[i].auth_mode,
                    (res[i].phy & WIFI_PHY_HT)   ? "n" :
                    (res[i].phy & WIFI_PHY_OFDM) ? "g" : "b",
                    res[i].beacon_tu, (unsigned)(res[i].max_rate / 2u));
            }
        }
        return 0;
    }

    if (strcmp(sub, "watch") == 0) {
        int windows = (argc >= 3) ? atoi(argv[2]) : 5;
        if (windows < 1) windows = 1;
        int rc = wifi_scan_start(NULL, NULL);
        if (rc == WIFI_ERR_BUSY) {
            shell_print("A scan is already running\r\n");
            return -1;
        }
        if (rc != 0) {
            shell_print("Scan failed (%d)\r\n", rc);
            return -1;
        }
        for (int w = 0; w < windows; w++) {
            const wifi_scan_list_t *l;
            if (wifi_scan_wait(&l) != 0) break;
            shell_print("-- window %u: %u ms, %d networks, %u dropped\r\n",
                        (unsigned)l->seq, (unsigned)l->window_ms,
                        l->count, (unsigned)l->dropped);
            for (int i = 0; i < l->count; i++) {
                shell_print("   %-32s  %5d  %2u\r\n", l->items[i].ssid,
                            (int)l->items[i].rssi, l->items[i].channel);
            }
        }
        wifi_scan_stop();
        return 0;
    }

    if (strcmp(sub, "connect") == 0) {
        if (argc < 3) {
            shell_print("Usage: wifi connect <ssid> [password]\r\n");
            return -1;
        }
        const char *pw = (argc >= 4) ? argv[3] : "";
        shell_print("Connecting to \"%s\"...\r\n", argv[2]);
        int rc = wifi_connect(argv[2], pw);
        if (rc == 0) {
            shell_print("Connected — IP: %s\r\n", wifi_get_ip_str());
        } else {
            shell_print("Failed (%d)\r\n", rc);
        }
        return rc;
    }

    if (strcmp(sub, "disconnect") == 0) {
        wifi_disconnect();
        shell_print("Disconnected\r\n");
        return 0;
    }

    shell_print("Usage: wifi [status|scan|watch [n]|connect <ssid> [pw]|disconnect]\r\n");
    return -1;
}

static const shell_cmd_t wifi_cmd = {
    "wifi",
    "wifi [status|scan|watch [n]|connect <ssid> [pw]|disconnect]",
    cmd_wifi
};

/* ---- wifi_init ----------------------------------------------------------- */
void wifi_init(void)
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    kmutex_init(&g_cont_lock);
    event_flags_init(&g_cont_ev);
    event_flags_set(&g_cont_ev, CONT_EV_STOP);   /* not running yet */

    if (cyw43_arch_init() != 0) {
        printf("[wifi] cyw43_arch_init failed\r\n");
        return;
    }
    cyw43_arch_enable_sta_mode();
    g_state = WIFI_STATE_DOWN;

    /* Create the poll thread in the kernel process at low priority (6). */
    pcb_t *kproc = task_get_kernel_proc();
    task_create_thread(kproc, "wifi-poll",
                       wifi_poll_thread, NULL,
                       6u, DEFAULT_STACK_SIZE);

    shell_register_cmd(&wifi_cmd);
    printf("[wifi] CYW43 initialized (STA mode)\r\n");
}

#endif /* PICOOS_WIFI_ENABLE */
