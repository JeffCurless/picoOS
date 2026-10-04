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

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_STATE_DOWN       = 0,  /* radio up, no AP association          */
    WIFI_STATE_SCANNING   = 1,  /* active background scan               */
    WIFI_STATE_CONNECTING = 2,  /* association in progress              */
    WIFI_STATE_UP         = 3,  /* associated and link is up            */
    WIFI_STATE_ERROR      = 4,  /* last operation failed                */
} wifi_state_t;

#define WIFI_MAX_SCAN_RESULTS 16

/* auth_mode bits.  The CYW43 driver builds these from the beacon's IEs; they
 * are not the CYW43_AUTH_* values wifi_connect() takes.  RSN covers WPA2 and
 * WPA3 alike, and APs that use WPA or RSN also set WEP (the privacy bit). */
#define WIFI_AUTH_WEP   0x01u   /* capability privacy bit   */
#define WIFI_AUTH_WPA   0x02u   /* WPA vendor IE            */
#define WIFI_AUTH_RSN   0x04u   /* RSN IE (WPA2 / WPA3)     */

/* phy bits */
#define WIFI_PHY_OFDM   0x01u   /* OFDM rates (802.11g)     */
#define WIFI_PHY_HT     0x02u   /* 802.11n                  */
#define WIFI_PHY_HT40   0x04u   /* 40 MHz channel allowed   */

typedef struct {
    char     ssid[33];
    uint8_t  bssid[6];
    int16_t  rssi;
    uint8_t  channel;
    uint8_t  auth_mode;   /* WIFI_AUTH_* bits                       */
    int8_t   noise;       /* dBm, 0 = unknown                       */
    int8_t   snr;         /* dB,  0 = unknown                       */
    uint16_t beacon_tu;   /* beacon interval in TU (1.024 ms)       */
    uint16_t capability;  /* 802.11 capability field                */
    uint8_t  max_rate;    /* highest legacy rate, 500 kb/s units    */
    uint8_t  phy;         /* WIFI_PHY_* bits                        */
} wifi_scan_result_t;

void         wifi_init(void);
wifi_state_t wifi_get_state(void);
/* Start a scan.  0, WIFI_ERR_BUSY if one is already running, or a CYW43
 * error code. */
int          wifi_scan(void);
int          wifi_scan_is_done(void);
/* Copy up to `max` scan results into the caller's `buf`.  Returns the number
 * copied (0..max) or WIFI_ERR_ARG.  Safe to call while a scan is running. */
int          wifi_copy_scan_results(wifi_scan_result_t *buf, int max);
/* Deprecated: returns the live kernel buffer with no lock, so entries can be
 * torn while a scan is running.  Use wifi_copy_scan_results() instead. */
int          wifi_get_scan_results(const wifi_scan_result_t **out_results, int *out_count);

/* --- Continuous scanning --------------------------------------------------
 *
 * wifi_scan_start() keeps the radio scanning in back-to-back passes until
 * wifi_scan_stop().  Each pass is one window: when it ends, the list of every
 * network heard in it is handed to the subscriber by swapping buffers (see
 * kernel/scanbuf.h), not by copying.  Firmware dwell times are shortened
 * while continuous mode is on so a pass takes well under a second; a pass
 * that runs past WIFI_WINDOW_MAX_MS is published early, partial, and the rest
 * of it lands in the next window.
 *
 * Two ways to receive windows (one subscriber at a time):
 *
 *   Pull     wifi_scan_start(NULL, NULL), then loop on wifi_scan_wait() in
 *            your own worker thread.  The returned list belongs to you until
 *            your next wifi_scan_wait(); read it with no lock.
 *
 *   Callback wifi_scan_start(cb, ctx).  The kernel spawns a "wifi-listen"
 *            thread in the caller's process, at the caller's priority, that
 *            calls cb(list, ctx) once per window.  The callback runs on an
 *            ordinary thread, so it may block, take mutexes and do real
 *            processing; while it runs, newer windows replace older ones and
 *            are counted in list->dropped.  Do not call wifi_scan_wait()
 *            yourself in this mode.
 *
 * Continuous mode and one-shot wifi_scan() exclude each other (WIFI_ERR_BUSY).
 * If the subscriber's process exits, scanning stops on its own.  Scanning
 * while associated (WIFI_STATE_UP) works but takes airtime from traffic.
 * ------------------------------------------------------------------------- */
#define WIFI_WINDOW_MAX_MS  1000u

typedef struct {
    uint32_t           seq;        /* window number, 1, 2, 3, ...          */
    uint32_t           dropped;    /* total windows lost to a slow reader  */
    uint32_t           window_ms;  /* how long this window lasted          */
    int                count;      /* valid entries in items[]             */
    wifi_scan_result_t items[WIFI_MAX_SCAN_RESULTS];  /* one per BSSID     */
} wifi_scan_list_t;

typedef void (*wifi_scan_cb_t)(const wifi_scan_list_t *list, void *ctx);

/* 0, WIFI_ERR_BUSY (a scan or subscriber is active, or the previous
 * listener thread is still inside its callback), WIFI_ERR_NOMEM (could not
 * create the listener thread) or a CYW43 error code. */
int          wifi_scan_start(wifi_scan_cb_t cb, void *ctx);
/* Stop continuous mode and wake the subscriber.  Safe to call when it is not
 * running.  A pass already under way finishes in the background (<1 s). */
void         wifi_scan_stop(void);
/* Block until the next window and point *list at it.  0, WIFI_ERR_STOPPED
 * (continuous mode is not running, or was stopped while waiting), or
 * WIFI_ERR_ARG. */
int          wifi_scan_wait(const wifi_scan_list_t **list);
bool         wifi_scan_running(void);

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
 * The receive callback runs in the CYW43 async context (a low-priority IRQ
 * on core 0), not on any thread.  Keep it short: copy the data out (e.g.
 * mqueue_try_send) and return.  Do not call wifi_mcast_* or block from
 * inside it.  `data` is
 * NUL-terminated (data[len] == '\0'); payloads longer than
 * WIFI_MCAST_MAX_PAYLOAD bytes are truncated.
 *
 * Sockets are not tied to a process: an app that may be killed should keep
 * its handle in a static and close it on its next start.
 * ------------------------------------------------------------------------- */
#define WIFI_MCAST_MAX_SOCKETS  2
#define WIFI_MCAST_MAX_PAYLOAD  128

/* Error codes returned by wifi_scan*, wifi_copy_scan_results and wifi_mcast_*
 * (all negative). */
#define WIFI_ERR_ARG     (-1)   /* bad argument, bad handle, or link not up */
#define WIFI_ERR_NOSOCK  (-2)   /* all WIFI_MCAST_MAX_SOCKETS in use        */
#define WIFI_ERR_NOMEM   (-3)   /* lwIP out of memory                       */
#define WIFI_ERR_BIND    (-4)   /* port already bound                       */
#define WIFI_ERR_JOIN    (-5)   /* IGMP join failed                         */
#define WIFI_ERR_SEND    (-6)   /* lwIP rejected the datagram               */
#define WIFI_ERR_BUSY    (-7)   /* a scan is already running                */
#define WIFI_ERR_STOPPED (-8)   /* continuous scanning is not running       */

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
static inline int  cyw43_ioctl(cyw43_t *self, uint32_t cmd, size_t len,
                               uint8_t *buf, uint32_t iface)
    { (void)self;(void)cmd;(void)len;(void)buf;(void)iface; return 0; }

#endif /* !__arm__ */

#endif /* PICOOS_WIFI_ENABLE */
#endif /* KERNEL_WIFI_H */
