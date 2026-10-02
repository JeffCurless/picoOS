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

#include "kernel/wifi.h"
#include "kernel/vfs.h"
#include "kernel/syscall.h"
#include "shell/shell.h"
#include <string.h>
#include <stddef.h>
#include <stdio.h>

#define CONFIG_FILE           "config.txt"
#define CONFIG_BUFSZ          256
#define CRED_MAXLEN           64

#define MCAST_GROUP           "239.255.0.1"
#define MCAST_PORT            4210u
#define ANNOUNCE_INTERVAL_MS  2000u

/* ---- parse_config --------------------------------------------------------
 *
 * Scans a NUL-terminated text buffer line-by-line looking for:
 *   SSID=<value>
 *   PASSWORD=<value>
 *
 * Values are written into ssid/password (each sized CRED_MAXLEN).
 * Trailing CR/LF is stripped from values.
 * ------------------------------------------------------------------------- */
static void parse_config(const char *buf,
                          char *ssid,     size_t ssid_sz,
                          char *password, size_t pass_sz)
{
    const char *p = buf;
    while (*p) {
        const char *eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;

        if (strncmp(p, "SSID=", 5) == 0) {
            size_t len = (size_t)(eol - (p + 5));
            if (len >= ssid_sz) len = ssid_sz - 1;
            memcpy(ssid, p + 5, len);
            ssid[len] = '\0';
        } else if (strncmp(p, "PASSWORD=", 9) == 0) {
            size_t len = (size_t)(eol - (p + 9));
            if (len >= pass_sz) len = pass_sz - 1;
            memcpy(password, p + 9, len);
            password[len] = '\0';
        }

        p = eol;
        while (*p == '\n' || *p == '\r') p++;
    }
}

/* ---- mcast_recv_cb -------------------------------------------------------
 *
 * Called on the wifi-poll thread (see wifi_mcast_open()) whenever a UDP
 * datagram arrives on the multicast port.  Prints the sender's address and
 * the message payload.
 * ------------------------------------------------------------------------- */
static volatile uint32_t g_rx_count = 0;

/* Survive a thread kill: keep the socket handle so the next run can close
 * it before opening a new one. */
static int g_sock = -1;

static void mcast_recv_cb(const char *msg, uint16_t len,
                          const char *src_ip, void *ctx)
{
    (void)len;
    (void)ctx;

    uint32_t count = ++g_rx_count;
    shell_print("[wifi-test] #%lu RX from %s: %s\r\n",
                (unsigned long)count, src_ip, msg);
}

/* ---- wifi_test -----------------------------------------------------------
 *
 * Entry point registered in app_table[].  Run via: run wifi-test
 *
 * 1. Opens config.txt from the filesystem.
 * 2. Parses SSID= and PASSWORD= lines.
 * 3. Connects to the network (up to 10 s).
 * 4. Prints the assigned IP address on success.
 * 5. Joins multicast group 239.255.0.1 on UDP port 4210.
 * 6. Loops forever: sends an announcement every 2 s and receives
 *    announcements from other nodes, printing sender IP and message.
 * ------------------------------------------------------------------------- */
void wifi_test(void *arg)
{
    (void)arg;

    /* ---- 1. Open config.txt ---- */
    int fd = vfs_open(CONFIG_FILE, VFS_O_RDONLY);
    if (fd < 0) {
        shell_print("[wifi-test] ERROR: config.txt not found\r\n");
        shell_print("[wifi-test] Create it with: fs write config.txt\r\n");
        shell_print("[wifi-test]   SSID=<network>\r\n");
        shell_print("[wifi-test]   PASSWORD=<password>\r\n");
        return;
    }

    /* ---- 2. Read and parse ---- */
    char buf[CONFIG_BUFSZ];
    int n = vfs_read(fd, (uint8_t *)buf, sizeof(buf) - 1);
    vfs_close(fd);

    if (n <= 0) {
        shell_print("[wifi-test] ERROR: config.txt is empty or unreadable\r\n");
        return;
    }
    buf[n] = '\0';

    char ssid[CRED_MAXLEN]     = {0};
    char password[CRED_MAXLEN] = {0};
    parse_config(buf, ssid, sizeof(ssid), password, sizeof(password));

    if (ssid[0] == '\0') {
        shell_print("[wifi-test] ERROR: SSID= not found in config.txt\r\n");
        return;
    }

    shell_print("[wifi-test] SSID     : %s\r\n", ssid);
    shell_print("[wifi-test] Password : %s\r\n",
                password[0] ? "(set)" : "(none — open network)");

    /* ---- 3. Connect ---- */
    shell_print("[wifi-test] Connecting...\r\n");
    int rc = wifi_connect(ssid, password);
    if (rc != 0) {
        shell_print("[wifi-test] Connection failed (err %d)\r\n", rc);
        return;
    }

    /* ---- 4. Report IP ---- */
    shell_print("[wifi-test] Connected — IP: %s\r\n", wifi_get_ip_str());

    /* ---- 5. Set up multicast UDP ---- */

    /* Release a socket left behind by a previous run that was killed before
     * reaching the normal cleanup path. */
    if (g_sock >= 0) {
        wifi_mcast_close(g_sock);
        g_sock = -1;
    }

    /* mcast_recv_cb is invoked on the wifi-poll thread whenever a datagram
     * arrives. */
    g_sock = wifi_mcast_open(MCAST_GROUP, MCAST_PORT, mcast_recv_cb, NULL);
    if (g_sock < 0) {
        shell_print("[wifi-test] ERROR: multicast setup failed (err %d)\r\n", g_sock);
        g_sock = -1;
        return;
    }

    shell_print("[wifi-test] Joined %s port %u — "
                "advertising every %u ms\r\n",
                MCAST_GROUP, MCAST_PORT, ANNOUNCE_INTERVAL_MS);

    /* ---- 6. Announce / listen loop ---- */
    char msg[48];
    while (wifi_get_state() == WIFI_STATE_UP) {
        snprintf(msg, sizeof(msg), "picoOS@%s", wifi_get_ip_str());
        wifi_mcast_send(g_sock, msg, (uint16_t)strlen(msg));
        sys_sleep(ANNOUNCE_INTERVAL_MS);
    }

    /* ---- 7. Clean up ---- */
    wifi_mcast_close(g_sock);
    g_sock = -1;

    shell_print("[wifi-test] Link lost — multicast stopped\r\n");
}

#endif /* PICOOS_WIFI_ENABLE */
