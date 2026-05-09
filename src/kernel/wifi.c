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

/* ---- state --------------------------------------------------------------- */
static volatile wifi_state_t  g_state      = WIFI_STATE_DOWN;
static wifi_scan_result_t     g_scan[WIFI_MAX_SCAN_RESULTS];
static volatile int           g_scan_count = 0;
static volatile bool          g_scan_done  = false;

/* ---- scan callback (called from cyw43 poll context) ---------------------- */
static int scan_result_cb(void *env, const cyw43_ev_scan_result_t *r)
{
    (void)env;
    if (g_scan_count >= WIFI_MAX_SCAN_RESULTS) return 0;
    int i = g_scan_count++;
    int len = r->ssid_len < 32 ? r->ssid_len : 32;
    memcpy(g_scan[i].ssid, r->ssid, len);
    g_scan[i].ssid[len] = '\0';
    g_scan[i].rssi      = r->rssi;
    g_scan[i].channel   = r->channel;
    g_scan[i].auth_mode = (uint8_t)r->auth_mode;
    return 0;
}

/* ---- public API ---------------------------------------------------------- */
wifi_state_t wifi_get_state(void) { return g_state; }

int wifi_scan(void)
{
    g_scan_count = 0;
    g_scan_done  = false;
    g_state = WIFI_STATE_SCANNING;
    cyw43_wifi_scan_options_t opts = {0};
    return cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result_cb);
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
        int rc = cyw43_wifi_join(&cyw43_state,
                                 ssid_len, (const uint8_t *)ssid,
                                 key_len,  (const uint8_t *)password,
                                 auth, NULL, 0);
        if (rc != 0) {
            g_state = WIFI_STATE_ERROR;
            return rc;
        }

        /* Wait for lwIP to complete association and DHCP (CYW43_LINK_UP).
         * 200 iterations × 50 ms = 10 s per attempt. */
        int status = CYW43_LINK_DOWN;
        for (int i = 0; i < 200; i++) {
            status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
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
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        sys_sleep(500);
    }

    g_state = WIFI_STATE_ERROR;
    return -1;
}

int wifi_disconnect(void)
{
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    g_state = WIFI_STATE_DOWN;
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

/* ---- poll thread --------------------------------------------------------- */
static void wifi_poll_thread(void *arg)
{
    (void)arg;
    for (;;) {
        cyw43_arch_poll();

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
        if (wifi_scan() != 0) {
            shell_print("Scan failed\r\n");
            return -1;
        }
        /* Wait for scan to complete (poll thread updates g_scan_done). */
        while (!g_scan_done) { sys_sleep(50); }
        if (g_scan_count == 0) {
            shell_print("No networks found\r\n");
        } else {
            shell_print("%-32s  %5s  Ch  Auth\r\n", "SSID", "RSSI");
            for (int i = 0; i < g_scan_count; i++) {
                shell_print("%-32s  %5d  %2u  %u\r\n",
                    g_scan[i].ssid, (int)g_scan[i].rssi,
                    g_scan[i].channel, g_scan[i].auth_mode);
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
