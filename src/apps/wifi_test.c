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

#ifdef __arm__
#include "lwip/udp.h"
#include "lwip/igmp.h"
#include "lwip/pbuf.h"
#include "pico/cyw43_arch.h"
#endif

#define CONFIG_FILE           "config.txt"
#define CONFIG_BUFSZ          256
#define CRED_MAXLEN           64

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
 * Called from the wifi-poll thread during cyw43_arch_poll() whenever a UDP
 * datagram arrives on the multicast port.  Prints the sender's address and
 * the message payload.
 * ------------------------------------------------------------------------- */
#ifdef __arm__
static volatile uint32_t g_rx_count = 0;

static void mcast_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                           const ip_addr_t *src, u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)port;

    if (p == NULL) {
        return;
    }

    char msg[64];
    u16_t copy_len = p->tot_len < (u16_t)(sizeof(msg) - 1u)
                         ? p->tot_len
                         : (u16_t)(sizeof(msg) - 1u);
    pbuf_copy_partial(p, msg, copy_len, 0);
    msg[copy_len] = '\0';
    pbuf_free(p);

    uint32_t count = ++g_rx_count;
    printf("[wifi-test] #%lu RX from %s: %s\r\n",
           (unsigned long)count, ipaddr_ntoa(src), msg);
}
#endif /* __arm__ */

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

#ifdef __arm__
    /* ---- 5. Set up multicast UDP ---- */
    ip4_addr_t mcast_group;
    IP4_ADDR(&mcast_group, 239, 255, 0, 1);

    cyw43_arch_lwip_begin();

    struct udp_pcb *pcb = udp_new();
    if (pcb == NULL) {
        cyw43_arch_lwip_end();
        shell_print("[wifi-test] ERROR: udp_new failed\r\n");
        return;
    }

    /* Bind to any local address on MCAST_PORT so we receive incoming
     * datagrams sent to the multicast group. */
    if (udp_bind(pcb, IP_ADDR_ANY, MCAST_PORT) != ERR_OK) {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        shell_print("[wifi-test] ERROR: udp_bind failed\r\n");
        return;
    }

    /* Join the multicast group on the STA interface. */
    const ip4_addr_t *my_addr =
        netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    igmp_joingroup(my_addr, &mcast_group);

    /* mcast_recv_cb is invoked by the wifi-poll thread during
     * cyw43_arch_poll() whenever a datagram arrives. */
    udp_recv(pcb, mcast_recv_cb, NULL);

    cyw43_arch_lwip_end();

    shell_print("[wifi-test] Joined 239.255.0.1 port %u — "
                "advertising every %u ms\r\n",
                MCAST_PORT, ANNOUNCE_INTERVAL_MS);

    /* ---- 6. Announce / listen loop ---- */
    char msg[48];
    while (wifi_get_state() == WIFI_STATE_UP) {
        snprintf(msg, sizeof(msg), "picoOS@%s", wifi_get_ip_str());
        size_t mlen = strlen(msg);

        cyw43_arch_lwip_begin();
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)mlen, PBUF_RAM);
        if (p != NULL) {
            memcpy(p->payload, msg, mlen);
            udp_sendto(pcb, p, (const ip_addr_t *)&mcast_group, MCAST_PORT);
            pbuf_free(p);
        }
        cyw43_arch_lwip_end();

        sys_sleep(ANNOUNCE_INTERVAL_MS);
    }

    /* ---- 7. Clean up ---- */
    cyw43_arch_lwip_begin();
    my_addr = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    igmp_leavegroup(my_addr, &mcast_group);
    udp_remove(pcb);
    cyw43_arch_lwip_end();

    shell_print("[wifi-test] Link lost — multicast stopped\r\n");
#endif /* __arm__ */
}

#endif /* PICOOS_WIFI_ENABLE */
