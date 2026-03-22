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
    int16_t rssi;
    uint8_t channel;
    uint8_t auth_mode;
} wifi_scan_result_t;

void         wifi_init(void);
wifi_state_t wifi_get_state(void);
int          wifi_scan(void);
int          wifi_connect(const char *ssid, const char *password);
int          wifi_disconnect(void);
const char  *wifi_get_ip_str(void);  /* returns dotted-decimal IP, or "0.0.0.0" */

/* --- Host / LSP stubs ---------------------------------------------------- */
#ifndef __arm__

typedef struct { int dummy; } cyw43_t;
typedef struct {
    uint32_t version;
} cyw43_wifi_scan_options_t;

typedef struct {
    uint8_t  ssid[32];
    uint8_t  ssid_len;
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
