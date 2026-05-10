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

#ifndef KERNEL_BLUETOOTH_H
#define KERNEL_BLUETOOTH_H

#ifdef PICOOS_BT_ENABLE

#include <stdint.h>

typedef enum {
    BT_STATE_OFF      = 0,  /* BT radio not yet powered on              */
    BT_STATE_IDLE     = 1,  /* radio up, no active scan                 */
    BT_STATE_SCANNING = 2,  /* classic inquiry + BLE scan in progress   */
    BT_STATE_ERROR    = 3,  /* initialization or scan failed            */
} bt_state_t;

typedef enum {
    BT_DEVTYPE_CLASSIC = 0, /* Bluetooth Classic (BR/EDR)               */
    BT_DEVTYPE_BLE     = 1, /* Bluetooth Low Energy                     */
} bt_devtype_t;

/* Major device class derived from the Bluetooth Class of Device field.
 * BLE devices are always BT_CLASS_UNKNOWN (no CoD in ADV packets). */
typedef enum {
    BT_CLASS_UNKNOWN    = 0,
    BT_CLASS_COMPUTER   = 1,
    BT_CLASS_PHONE      = 2,
    BT_CLASS_NETWORK    = 3,
    BT_CLASS_AUDIO      = 4,
    BT_CLASS_PERIPHERAL = 5,
    BT_CLASS_IMAGING    = 6,
    BT_CLASS_WEARABLE   = 7,
    BT_CLASS_TOY        = 8,
    BT_CLASS_HEALTH     = 9,
    BT_CLASS_OTHER      = 10,
} bt_devclass_t;

#define BT_MAX_SCAN_RESULTS 20
#define BT_ADDR_LEN          6
#define BT_NAME_LEN         32

/* Sentinel values used when a field was not present in the advertising data. */
#define BT_TX_POWER_UNKNOWN  ((int8_t)  127)
#define BT_FLAGS_NONE        ((uint8_t) 0xFFu)
#define BT_COMPANY_NONE      ((uint16_t)0xFFFFu)

typedef struct {
    uint8_t       addr[BT_ADDR_LEN];
    char          name[BT_NAME_LEN];
    int8_t        rssi;
    bt_devtype_t  type;
    bt_devclass_t dev_class;
    uint32_t      class_of_device;  /* raw 24-bit CoD; 0 for BLE devices      */
    int8_t        tx_power;         /* TX Power Level dBm (AD 0x0A)            */
    uint8_t       flags;            /* AD Flags byte (AD 0x01)                 */
    uint16_t      company_id;       /* Manufacturer company ID (AD 0xFF)       */
} bt_scan_result_t;

void          bt_init(void);
bt_state_t    bt_get_state(void);
int           bt_scan(void);
int           bt_scan_is_done(void);
int           bt_get_scan_results(const bt_scan_result_t **out, int *out_count);
const char   *bt_devclass_str(bt_devclass_t cls);

/* --- Host / LSP stubs ---------------------------------------------------- */
#ifndef __arm__

typedef uint8_t bd_addr_t[6];

typedef struct {
    void (*callback)(uint8_t pkt_type, uint16_t chan, uint8_t *pkt, uint16_t size);
} btstack_packet_callback_registration_t;

typedef struct { uint8_t *data; uint8_t len; uint8_t pos; } ad_context_t;

static inline bool btstack_cyw43_init(void *ctx)   { (void)ctx; return true; }
static inline int  hci_power_control(int m)        { (void)m; return 0; }
static inline void hci_add_event_handler(btstack_packet_callback_registration_t *r) { (void)r; }
static inline void gap_inquiry_start(uint8_t d)    { (void)d; }
static inline int  gap_inquiry_stop(void)          { return 0; }
static inline void gap_set_scan_parameters(uint8_t t, uint16_t i, uint16_t w)
    { (void)t; (void)i; (void)w; }
static inline void gap_start_scan(void)            {}
static inline void gap_stop_scan(void)             {}
static inline int  gap_remote_name_request(const bd_addr_t a, uint8_t m, uint16_t c)
    { (void)a; (void)m; (void)c; return 0; }

/* HCI / GAP event codes used in bluetooth.c */
#define HCI_POWER_ON                             1
#define HCI_EVENT_INQUIRY_COMPLETE               0x01
#define HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE   0x07
#define GAP_EVENT_INQUIRY_RESULT                 0xF2
#define GAP_EVENT_INQUIRY_COMPLETE               0xF3
#define GAP_EVENT_ADVERTISING_REPORT             0xF4

/* Packet type sentinel */
#define HCI_EVENT_PACKET                         0x04

/* Stub event field getters */
static inline uint8_t hci_event_packet_get_type(const uint8_t *p) { (void)p; return 0; }
static inline void gap_event_inquiry_result_get_bd_addr(const uint8_t *p, bd_addr_t a)
    { (void)p; (void)a; }
static inline uint32_t gap_event_inquiry_result_get_class_of_device(const uint8_t *p) { (void)p; return 0; }
static inline bool gap_event_inquiry_result_get_rssi_available(const uint8_t *p) { (void)p; return false; }
static inline int8_t gap_event_inquiry_result_get_rssi(const uint8_t *p) { (void)p; return 0; }
static inline uint8_t gap_event_inquiry_result_get_page_scan_repetition_mode(const uint8_t *p) { (void)p; return 0; }
static inline uint16_t gap_event_inquiry_result_get_clock_offset(const uint8_t *p) { (void)p; return 0; }
static inline void hci_event_remote_name_request_complete_get_bd_addr(const uint8_t *p, bd_addr_t a)
    { (void)p; (void)a; }
static inline const uint8_t *hci_event_remote_name_request_complete_get_remote_name(const uint8_t *p) { (void)p; return (const uint8_t *)""; }
static inline void gap_event_advertising_report_get_address(const uint8_t *p, bd_addr_t a)
    { (void)p; (void)a; }
static inline int8_t gap_event_advertising_report_get_rssi(const uint8_t *p) { (void)p; return 0; }
static inline uint8_t gap_event_advertising_report_get_data_length(const uint8_t *p) { (void)p; return 0; }
static inline const uint8_t *gap_event_advertising_report_get_data(const uint8_t *p) { (void)p; return NULL; }

/* AD data iterator stubs */
static inline void ad_iterator_init(ad_context_t *ctx, uint8_t len, const uint8_t *data)
    { (void)ctx; (void)len; (void)data; }
static inline int  ad_iterator_has_more(const ad_context_t *ctx) { (void)ctx; return 0; }
static inline void ad_iterator_next(ad_context_t *ctx) { (void)ctx; }
static inline uint8_t ad_iterator_get_data_type(const ad_context_t *ctx) { (void)ctx; return 0; }
static inline uint8_t ad_iterator_get_data_len(const ad_context_t *ctx) { (void)ctx; return 0; }
static inline const uint8_t *ad_iterator_get_data(const ad_context_t *ctx) { (void)ctx; return NULL; }

static inline int  bd_addr_cmp(const bd_addr_t a, const bd_addr_t b) { (void)a; (void)b; return 0; }
static inline void bd_addr_copy(bd_addr_t dst, const bd_addr_t src) { (void)dst; (void)src; }
static inline void reverse_bd_addr(const bd_addr_t src, bd_addr_t dst) { (void)src; (void)dst; }

/* Async context stub (only used in bt_init -> btstack_cyw43_init) */
typedef struct { int dummy; } async_context_t;
static inline async_context_t *cyw43_arch_async_context(void) { return NULL; }

#endif /* !__arm__ */

#endif /* PICOOS_BT_ENABLE */
#endif /* KERNEL_BLUETOOTH_H */
