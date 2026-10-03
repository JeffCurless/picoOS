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
#include "kernel/task.h"
#include "kernel/syscall.h"
#include "shell/shell.h"
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

/* ---- scan callback (called from cyw43 poll context) ---------------------- */
static int scan_result_cb(void *env, const cyw43_ev_scan_result_t *r)
{
    (void)env;
    int i = g_scan_count;

    /* The chip reports each BSS many times per scan (every beacon / probe
     * response it hears).  Refresh the existing slot instead of taking a new
     * one, or the buffer fills with repeats and later networks are lost.
     * rssi >= 0 is a bogus reading the firmware sometimes sends; skip it. */
    for (int k = 0; k < i; k++) {
        if (memcmp(g_scan[k].bssid, r->bssid, 6) == 0) {
            if (r->rssi < 0) g_scan[k].rssi = r->rssi;
            return 0;
        }
    }
    if (i >= WIFI_MAX_SCAN_RESULTS) return 0;

    wifi_scan_result_t *e = &g_scan[i];
#ifdef PICOOS_SCAN_RACE_INJECT
    /* TEST ONLY: the old bug, widened.  Publish the slot, poison it
     * (unterminated SSID, channel 255), and hold it there for a while. */
    g_scan_count = i + 1;
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
#ifndef PICOOS_SCAN_RACE_INJECT
    __dmb();
    g_scan_count = i + 1;
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
    if (cyw43_wifi_scan_active(&cyw43_state)) {
        cyw43_arch_lwip_end();
        return WIFI_ERR_BUSY;
    }

    g_scan_count = 0;
    g_scan_done  = false;
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
        shell_print("WiFi state: %s\r\n", state_str(g_state));
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
            shell_print("%-32s  %5s  Ch  Auth\r\n", "SSID", "RSSI");
            for (int i = 0; i < n; i++) {
                shell_print("%-32s  %5d  %2u  %u\r\n",
                    res[i].ssid, (int)res[i].rssi,
                    res[i].channel, res[i].auth_mode);
            }
        }
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

    shell_print("Usage: wifi [status|scan|connect <ssid> [pw]|disconnect]\r\n");
    return -1;
}

static const shell_cmd_t wifi_cmd = {
    "wifi",
    "wifi [status|scan|connect <ssid> [pw]|disconnect]",
    cmd_wifi
};

/* ---- wifi_init ----------------------------------------------------------- */
void wifi_init(void)
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

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
