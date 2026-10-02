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

#ifndef KERNEL_WIFI_H
#define KERNEL_WIFI_H

#ifdef PICOOS_WIFI_ENABLE

#include <stdint.h>

typedef enum {
    WIFI_STATE_DOWN       = 0,  /* radio up, no AP association          */
    WIFI_STATE_SCANNING   = 1,  /* active background scan               */
    WIFI_STATE_CONNECTING = 2,  /* association in progress              */
    WIFI_STATE_UP         = 3,  /* associated and link is up            */
    WIFI_STATE_ERROR      = 4,  /* last operation failed                */
} wifi_state_t;

#define WIFI_MAX_SCAN_RESULTS 16

typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    int16_t rssi;
    uint8_t channel;
    uint8_t auth_mode;
} wifi_scan_result_t;

void         wifi_init(void);
wifi_state_t wifi_get_state(void);
int          wifi_scan(void);
int          wifi_scan_is_done(void);
int          wifi_get_scan_results(const wifi_scan_result_t **out_results, int *out_count);
int          wifi_connect(const char *ssid, const char *password);
int          wifi_disconnect(void);
const char  *wifi_get_ip_str(void);  /* returns dotted-decimal IP, or "0.0.0.0" */
int          wifi_get_mac(uint8_t mac[6]);  /* STA MAC address; 0 on success */

/* --- Multicast UDP -------------------------------------------------------
 *
 * A minimal socket-style wrapper around lwIP so applications never touch
 * lwIP or the CYW43 driver directly.  All calls take the lwIP lock
 * internally.  The link must be up (wifi_connect() returned 0) before
 * wifi_mcast_open().
 *
 * The receive callback runs on the wifi-poll thread, not the caller's
 * thread.  Keep it short: copy the data out (e.g. mqueue_try_send) and
 * return.  Do not call wifi_mcast_* or block from inside it.  `data` is
 * NUL-terminated (data[len] == '\0'); payloads longer than
 * WIFI_MCAST_MAX_PAYLOAD bytes are truncated.
 *
 * Sockets are not tied to a process: an app that may be killed should keep
 * its handle in a static and close it on its next start.
 * ------------------------------------------------------------------------- */
#define WIFI_MCAST_MAX_SOCKETS  2
#define WIFI_MCAST_MAX_PAYLOAD  128

/* Error codes returned by wifi_mcast_* (all negative). */
#define WIFI_ERR_ARG     (-1)   /* bad argument, bad handle, or link not up */
#define WIFI_ERR_NOSOCK  (-2)   /* all WIFI_MCAST_MAX_SOCKETS in use        */
#define WIFI_ERR_NOMEM   (-3)   /* lwIP out of memory                       */
#define WIFI_ERR_BIND    (-4)   /* port already bound                       */
#define WIFI_ERR_JOIN    (-5)   /* IGMP join failed                         */
#define WIFI_ERR_SEND    (-6)   /* lwIP rejected the datagram               */

typedef void (*wifi_mcast_rx_cb_t)(const char *data, uint16_t len,
                                   const char *src_ip, void *ctx);

/* Join `group` (dotted-decimal, e.g. "239.255.0.1") and listen on `port`.
 * Returns a socket handle >= 0, or a WIFI_ERR_* code. */
int  wifi_mcast_open(const char *group, uint16_t port,
                     wifi_mcast_rx_cb_t cb, void *ctx);

/* Send `len` bytes to the socket's group and port.  0 or WIFI_ERR_*. */
int  wifi_mcast_send(int sock, const void *data, uint16_t len);

/* Leave the group and free the socket.  Ignores invalid handles. */
void wifi_mcast_close(int sock);

/* --- Host / LSP stubs ---------------------------------------------------- */
#ifndef __arm__

typedef struct { int dummy; } cyw43_t;
typedef struct {
    uint32_t version;
} cyw43_wifi_scan_options_t;

typedef struct {
    uint8_t  ssid[32];
    uint8_t  ssid_len;
    uint8_t  bssid[6];
    int16_t  rssi;
    uint8_t  channel;
    uint16_t auth_mode;
} cyw43_ev_scan_result_t;

/* Minimal ip4_addr stub so wifi_get_ip_str's #else branch compiles. */
typedef struct { uint32_t addr; } ip4_addr_t;
static inline const char *ip4addr_ntoa(const ip4_addr_t *a) { (void)a; return "0.0.0.0"; }

extern cyw43_t cyw43_state;

#define CYW43_AUTH_OPEN          0u
#define CYW43_AUTH_WPA2_AES_PSK  0x00400004u
#define CYW43_ITF_STA            0
#define CYW43_LINK_UP            3

static inline void cyw43_arch_enable_sta_mode(void) {}
static inline void cyw43_arch_poll(void) {}
static inline int  cyw43_wifi_join(
    cyw43_t *self, size_t ssid_len, const uint8_t *ssid,
    size_t key_len, const uint8_t *key, uint32_t auth_type,
    const uint8_t *bssid, uint32_t channel)
    { (void)self;(void)ssid_len;(void)ssid;(void)key_len;(void)key;
      (void)auth_type;(void)bssid;(void)channel; return 0; }
static inline int  cyw43_wifi_scan(
    cyw43_t *self, cyw43_wifi_scan_options_t *opts, void *env,
    int (*cb)(void *, const cyw43_ev_scan_result_t *))
    { (void)self;(void)opts;(void)env;(void)cb; return 0; }
static inline int  cyw43_wifi_scan_active(cyw43_t *self)
    { (void)self; return 0; }
static inline int  cyw43_tcpip_link_status(cyw43_t *self, int itf)
    { (void)self;(void)itf; return CYW43_LINK_UP; }
static inline void cyw43_wifi_leave(cyw43_t *self, int itf)
    { (void)self;(void)itf; }

#endif /* !__arm__ */

#endif /* PICOOS_WIFI_ENABLE */
#endif /* KERNEL_WIFI_H */
