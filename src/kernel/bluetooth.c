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

#ifdef PICOOS_BT_ENABLE

#include "kernel/arch.h"
#include "kernel/bluetooth.h"
#include "kernel/scanbuf.h"
#include "kernel/sync.h"
#include "kernel/task.h"
#include "kernel/syscall.h"
#include "shell/shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- state --------------------------------------------------------------- *
 * packet_handler() runs in the CYW43 async context (a low-priority IRQ on
 * core 0), so g_scan[], g_scan_count and the scan state are touched from
 * threads only while holding the async context lock (cyw43_arch_lwip_begin/
 * end).  See the matching note in wifi.c. */
static volatile bt_state_t  g_state        = BT_STATE_OFF;
static bt_scan_result_t     g_scan[BT_MAX_SCAN_RESULTS];
static volatile int         g_scan_count   = 0;
static volatile bool        g_scan_done    = false;
static volatile bool        g_classic_done = false;

static btstack_packet_callback_registration_t hci_event_cb_reg;

/* ---- continuous scanning state ------------------------------------------- *
 * Same scheme as wifi.c: three lists rotate through fill / ready / held
 * (scanbuf.h).  packet_handler() writes g_lists[g_sb.fill] in IRQ context;
 * threads move the roles only while holding g_cont_lock, and publish also
 * holds the async context lock.  The kmutex orders threads (the async lock is
 * per core), the async lock keeps the IRQ out. */
#define CONT_EV_READY  (1u << 0)
#define CONT_EV_STOP   (1u << 1)

static bt_scan_list_t       g_lists[3];
static scanbuf_t            g_sb;
static kmutex_t             g_cont_lock;
static event_flags_t        g_cont_ev;
static volatile bool        g_cont        = false;  /* continuous mode on   */
static volatile bool        g_tgt_cont    = false;  /* IRQ target: g_lists   */
static volatile bool        g_inq_active  = false;  /* Classic inquiry on   */
static uint64_t             g_inq_start_us;         /* last inquiry start   */
static uint64_t             g_window_start_us;
static uint32_t             g_owner_pid;
static bt_scan_cb_t         g_cb;
static void                *g_cb_ctx;
static uint32_t             g_listen_tid;           /* 0 = no listener      */

/* Name cache, continuous mode only.  Touched only by packet_handler (IRQ)
 * and by bt_scan_start() under the async context lock.  Addresses are in
 * the same (reversed) byte order as bt_scan_result_t.addr.  When full, the
 * oldest slot is reused round-robin.  Larger than one window's list because
 * Classic devices still waiting for a name share it with BLE names. */
#define BT_NAME_CACHE  32

typedef enum { NAME_FREE = 0, NAME_PENDING, NAME_KNOWN } bt_name_state_t;

typedef struct {
    uint8_t  addr[BT_ADDR_LEN];
    uint8_t  state;              /* bt_name_state_t */
    uint8_t  psrm;               /* Classic: page scan repetition mode  */
    uint16_t clock_offset;       /* Classic: clock offset | 0x8000      */
    char     name[BT_NAME_LEN];
} bt_name_ent_t;

static bt_name_ent_t g_names[BT_NAME_CACHE];
static uint8_t       g_name_next;

/* BTstack runs one remote name request at a time and refuses a second
 * while one is outstanding, so requests are sent one by one: a Classic
 * device that arrives while one is busy waits as NAME_PENDING and is asked
 * when the busy one completes.  g_name_busy_us lets a request whose
 * completion never arrives stop blocking the queue. */
#define BT_NAME_REQ_TIMEOUT_US  (10u * 1000u * 1000u)

static bool     g_name_busy;
static uint64_t g_name_busy_us;

static bt_name_ent_t *name_find(const uint8_t addr[BT_ADDR_LEN])
{
    for (int i = 0; i < BT_NAME_CACHE; i++) {
        if (g_names[i].state != NAME_FREE &&
            memcmp(g_names[i].addr, addr, BT_ADDR_LEN) == 0) return &g_names[i];
    }
    return NULL;
}

static bt_name_ent_t *name_add(const uint8_t addr[BT_ADDR_LEN])
{
    bt_name_ent_t *e = NULL;
    for (int i = 0; i < BT_NAME_CACHE && e == NULL; i++) {
        if (g_names[i].state == NAME_FREE) e = &g_names[i];
    }
    if (e == NULL) {
        e = &g_names[g_name_next];
        g_name_next = (uint8_t)((g_name_next + 1u) % BT_NAME_CACHE);
    }
    memcpy(e->addr, addr, BT_ADDR_LEN);
    e->state   = NAME_PENDING;
    e->name[0] = '\0';
    return e;
}

/* Remember a BLE name heard in advertising data. */
static void name_store(const uint8_t addr[BT_ADDR_LEN], const char *name)
{
    bt_name_ent_t *e = name_find(addr);
    if (e == NULL) e = name_add(addr);
    memcpy(e->name, name, BT_NAME_LEN);
    e->state = NAME_KNOWN;
}

/* Send a remote name request unless one is outstanding.  Returns true if
 * it was sent. */
static bool name_request(const uint8_t raddr[BT_ADDR_LEN], uint8_t psrm,
                         uint16_t clock_offset)
{
    if (g_name_busy &&
        time_us_64() - g_name_busy_us < BT_NAME_REQ_TIMEOUT_US) return false;

    bd_addr_t addr;
    reverse_bd_addr(raddr, addr);
    if (gap_remote_name_request(addr, psrm, clock_offset) != 0) return false;
    g_name_busy    = true;
    g_name_busy_us = time_us_64();
    return true;
}

/* Ask for the next Classic name still waiting in the cache, if any. */
static void name_request_next(void)
{
    for (int i = 0; i < BT_NAME_CACHE; i++) {
        bt_name_ent_t *e = &g_names[i];
        if (e->state == NAME_PENDING) {
            name_request(e->addr, e->psrm, e->clock_offset);
            return;
        }
    }
}

/* Where packet_handler writes: the one-shot buffer, or the fill list. */
static bt_scan_result_t *tgt_items(void)
{
    return g_tgt_cont ? g_lists[g_sb.fill].items : g_scan;
}

static volatile int *tgt_count(void)
{
    return g_tgt_cont ? (volatile int *)&g_lists[g_sb.fill].count : &g_scan_count;
}

/* ---- Class of Device decoder --------------------------------------------- */
static bt_devclass_t cod_to_devclass(uint32_t cod)
{
    switch ((cod >> 8) & 0x1Fu) {
        case 0x01u: return BT_CLASS_COMPUTER;
        case 0x02u: return BT_CLASS_PHONE;
        case 0x03u: return BT_CLASS_NETWORK;
        case 0x04u: return BT_CLASS_AUDIO;
        case 0x05u: return BT_CLASS_PERIPHERAL;
        case 0x06u: return BT_CLASS_IMAGING;
        case 0x07u: return BT_CLASS_WEARABLE;
        case 0x08u: return BT_CLASS_TOY;
        case 0x09u: return BT_CLASS_HEALTH;
        default:    return BT_CLASS_OTHER;
    }
}

/* Find the slot index whose address matches addr, or -1 if not found. */
static int find_slot_by_addr(const bd_addr_t addr)
{
    bt_scan_result_t *items = tgt_items();
    int n = *tgt_count();
    for (int i = 0; i < n; i++) {
        bd_addr_t slot_addr;
        reverse_bd_addr(items[i].addr, slot_addr);
        if (bd_addr_cmp(addr, slot_addr) == 0) return i;
    }
    return -1;
}

/* HCI reports RSSI 127 (0x7F) when it has no reading. */
static int8_t ble_report_rssi(const uint8_t *packet)
{
    int8_t rssi = (int8_t)gap_event_advertising_report_get_rssi(packet);
    return rssi == 127 ? BT_RSSI_UNKNOWN : rssi;
}

/* Index of the next free slot, or -1 if full.  The slot is not visible to
 * readers until publish_slot() is called, so fill it completely first. */
#ifndef PICOOS_SCAN_RACE_INJECT
static int next_slot(void)
{
    int n = *tgt_count();
    return n < BT_MAX_SCAN_RESULTS ? n : -1;
}

static void publish_slot(void)
{
    volatile int *cnt = tgt_count();
    __dmb();
    *cnt = *cnt + 1;
}
#else
/* TEST ONLY (PICOOS_SCAN_RACE_INJECT): the old bug, widened.  The slot is
 * published as soon as it is reserved, poisoned (unterminated name, invalid
 * type), and held there before the caller fills it. */
static int next_slot(void)
{
    volatile int *cnt = tgt_count();
    if (*cnt >= BT_MAX_SCAN_RESULTS) return -1;
    int idx = (*cnt)++;
    memset(&tgt_items()[idx], 0xFF, sizeof(bt_scan_result_t));
    busy_wait_us_32(50);
    return idx;
}

static void publish_slot(void) {}
#endif

/* Set an empty name on a slot readers may already see.  The tail and the
 * terminator go in first and name[0] last, so a lock-free reader sees
 * either "" or the whole name, never part of it. */
static void set_name(bt_scan_result_t *dev, const uint8_t *src, size_t len)
{
    if (len > BT_NAME_LEN - 1u) len = BT_NAME_LEN - 1u;
    if (len == 0u) return;
    if (len > 1u) memcpy(&dev->name[1], &src[1], len - 1u);
    dev->name[len] = '\0';
    __dmb();
    dev->name[0] = (char)src[0];
}

/* Start a new slot: every optional field "unknown", one report heard. */
static void slot_init(bt_scan_result_t *dev, const bd_addr_t addr,
                      bt_devtype_t type, int8_t rssi)
{
    memset(dev, 0, sizeof(*dev));   /* name "", lengths 0, zero sentinels */
    reverse_bd_addr(addr, dev->addr);
    dev->type       = type;
    dev->dev_class  = BT_CLASS_UNKNOWN;
    dev->rssi       = rssi;
    dev->tx_power   = BT_TX_POWER_UNKNOWN;
    dev->flags      = BT_FLAGS_NONE;
    dev->company_id = BT_COMPANY_NONE;
    dev->addr_type  = BT_ADDR_TYPE_NONE;
    dev->adv_type   = BT_ADV_TYPE_NONE;
    dev->pkt_count  = 1u;
}

static void count_report(bt_scan_result_t *dev)
{
    if (dev->pkt_count != UINT16_MAX) dev->pkt_count++;
}

static uint16_t le16(const uint8_t *d) { return (uint16_t)((uint16_t)d[1] << 8 | d[0]); }

static uint32_t le32(const uint8_t *d)
{
    return (uint32_t)d[3] << 24 | (uint32_t)d[2] << 16 |
           (uint32_t)d[1] << 8  | d[0];
}

/* Copy up to `max` bytes and set *len.  The caller publishes the field
 * that says the bytes are there (a UUID or company ID) after a barrier. */
static void set_bytes(uint8_t *dst, uint8_t *len, size_t max,
                      const uint8_t *src, size_t n)
{
    if (n > max) n = max;
    memcpy(dst, src, n);
    *len = (uint8_t)n;
}

/* ---- BLE AD data parser ------------------------------------------------- */
/* `rsp` is true for a scan response.  Some devices send a different
 * company ID there than in their advertisement; the advertisement's wins
 * so the field does not flip between the two from packet to packet.
 *
 * `fill_only` is for a one-shot slot that is already published: a field
 * that is set is never changed, so each field goes from unknown to known
 * at most once while readers watch (the rule scantest checks).  Service
 * and manufacturer data are written before the UUID / company ID that
 * marks them present, so a reader that checks that field first sees whole
 * data. */
static void extract_ble_adv_data(const uint8_t *ad_data, uint8_t ad_len,
                                  bool rsp, bool fill_only,
                                  bt_scan_result_t *dev)
{
    ad_context_t ctx;
    ad_iterator_init(&ctx, ad_len, ad_data);
    while (ad_iterator_has_more(&ctx)) {
        uint8_t        type = ad_iterator_get_data_type(&ctx);
        uint8_t        dlen = ad_iterator_get_data_len(&ctx);
        const uint8_t *d    = ad_iterator_get_data(&ctx);
        if (d == NULL || dlen == 0) { ad_iterator_next(&ctx); continue; }
        switch (type) {
        case 0x02u: /* Incomplete List of 16-bit Service Class UUIDs */
        case 0x03u: /* Complete List of 16-bit Service Class UUIDs   */
            if (dlen >= 2 && dev->service_uuid == BT_SERVICE_NONE)
                dev->service_uuid = le16(d);
            break;
        case 0x04u: /* Incomplete List of 32-bit Service Class UUIDs */
        case 0x05u: /* Complete List of 32-bit Service Class UUIDs   */
            if (dlen >= 4 && dev->uuid32 == BT_UUID32_NONE)
                dev->uuid32 = le32(d);
            break;
        case 0x06u: /* Incomplete List of 128-bit Service Class UUIDs */
        case 0x07u: /* Complete List of 128-bit Service Class UUIDs   */
            /* LSB first, so the top 32 bits are the last four bytes. */
            if (dlen >= 16 && dev->uuid32 == BT_UUID32_NONE)
                dev->uuid32 = le32(&d[12]);
            break;
        case 0x16u: /* Service Data - 16-bit UUID, then the data */
            if (dlen >= 2) {
                uint16_t uuid = le16(d);
                if (dev->svc_data_uuid == BT_SERVICE_NONE ||
                    (!fill_only && uuid == dev->svc_data_uuid)) {
                    set_bytes(dev->svc_data, &dev->svc_data_len,
                              BT_SVC_DATA_LEN, &d[2], dlen - 2u);
                    __dmb();
                    dev->svc_data_uuid = uuid;
                }
            }
            break;
        case 0x19u: /* Appearance */
            if (dlen >= 2 && (!fill_only || dev->appearance == BT_APPEARANCE_NONE))
                dev->appearance = le16(d);
            break;
        case 0x08u: /* Shortened Local Name */
        case 0x09u: /* Complete Local Name */
            if (dev->name[0] == '\0') set_name(dev, d, dlen);
            break;
        case 0x01u: /* Flags */
            if (!fill_only || dev->flags == BT_FLAGS_NONE)
                dev->flags = d[0];
            break;
        case 0x0Au: /* TX Power Level */
            if (!fill_only || dev->tx_power == BT_TX_POWER_UNKNOWN)
                dev->tx_power = (int8_t)d[0];
            break;
        case 0xFFu: /* Manufacturer Specific Data — company ID (LE), then data */
            if (dlen >= 2 && (dev->company_id == BT_COMPANY_NONE ||
                              (!rsp && !fill_only))) {
                set_bytes(dev->mfr_data, &dev->mfr_data_len,
                          BT_MFR_DATA_LEN, &d[2], dlen - 2u);
                __dmb();
                dev->company_id = le16(d);
            }
            break;
        default:
            break;
        }
        ad_iterator_next(&ctx);
    }
}

/* ---- HCI event callback -------------------------------------------------- */
static void packet_handler(uint8_t pkt_type, uint16_t channel,
                           uint8_t *packet, uint16_t size)
{
    (void)channel; (void)size;
    if (pkt_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);

    /* The radio is usable only once HCI reports WORKING; until then
     * bt_scan() and bt_scan_start() refuse instead of being dropped. */
    if (event == BTSTACK_EVENT_STATE) {
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING &&
            g_state == BT_STATE_OFF) {
            g_state = BT_STATE_IDLE;
        }
        return;
    }

    bt_scan_result_t *items = tgt_items();

    /* Classic inquiry result — one device per event. */
    if (event == GAP_EVENT_INQUIRY_RESULT) {
        bd_addr_t addr;
        gap_event_inquiry_result_get_bd_addr(packet, addr);
        int8_t rssi = gap_event_inquiry_result_get_rssi_available(packet)
                          ? gap_event_inquiry_result_get_rssi(packet)
                          : BT_RSSI_UNKNOWN;

        /* Already seen this scan: refresh the RSSI so it tracks the latest
         * reading instead of freezing at the first one. */
        int dup = find_slot_by_addr(addr);
        if (dup >= 0) {
            if (rssi != BT_RSSI_UNKNOWN) items[dup].rssi = rssi;
            count_report(&items[dup]);
            return;
        }

        int idx = next_slot();
        if (idx < 0) return;

        slot_init(&items[idx], addr, BT_DEVTYPE_CLASSIC, rssi);
        items[idx].class_of_device =
            gap_event_inquiry_result_get_class_of_device(packet);
        items[idx].dev_class       =
            cod_to_devclass(items[idx].class_of_device);

        /* Extended inquiry response (bt_init asks for it): the Device ID
         * record and the name, which then needs no remote name request. */
        if (gap_event_inquiry_result_get_device_id_available(packet)) {
            items[idx].did_source  =
                gap_event_inquiry_result_get_device_id_vendor_id_source(packet);
            items[idx].did_vendor  =
                gap_event_inquiry_result_get_device_id_vendor_id(packet);
            items[idx].did_product =
                gap_event_inquiry_result_get_device_id_product_id(packet);
            items[idx].did_version =
                gap_event_inquiry_result_get_device_id_version(packet);
        }
        if (gap_event_inquiry_result_get_name_available(packet))
            set_name(&items[idx], gap_event_inquiry_result_get_name(packet),
                     gap_event_inquiry_result_get_name_len(packet));
        bool eir_name = items[idx].name[0] != '\0';

        uint8_t  psrm =
            gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
        uint16_t clk  =
            gap_event_inquiry_result_get_clock_offset(packet) | 0x8000u;

        /* Continuous mode asks for each name once per session and fills it
         * in from the cache in later windows.  A device whose request has
         * not been sent yet stays NAME_PENDING with fresh paging info. */
        bool ask = !eir_name;
        if (g_tgt_cont && eir_name) {
            name_store(items[idx].addr, items[idx].name);
        } else if (g_tgt_cont) {
            bt_name_ent_t *n = name_find(items[idx].addr);
            if (n == NULL) n = name_add(items[idx].addr);
            if (n->state == NAME_KNOWN) {
                memcpy(items[idx].name, n->name, BT_NAME_LEN);
                ask = false;
            } else {
                n->psrm         = psrm;
                n->clock_offset = clk;
            }
        }
        publish_slot();   /* before the name request: its reply looks us up */

        /* Request the human-readable name asynchronously.  If another
         * request is outstanding this one is sent when it completes. */
        if (ask) name_request(items[idx].addr, psrm, clk);
        return;
    }

    /* Classic remote name response. */
    if (event == HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE) {
        bd_addr_t addr;
        hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
        bool ok = hci_event_remote_name_request_complete_get_status(packet) == 0;
        const uint8_t *name = ok
            ? hci_event_remote_name_request_complete_get_remote_name(packet)
            : NULL;
        g_name_busy = false;

        if (g_tgt_cont) {
            /* Record the answer even if the device has rotated out of the
             * fill list.  A failure is cached as an empty name, so a device
             * that never answers is not paged again every inquiry. */
            uint8_t raddr[BT_ADDR_LEN];
            reverse_bd_addr(addr, raddr);
            bt_name_ent_t *n = name_find(raddr);
            if (n != NULL) {
                n->name[0] = '\0';
                if (name) {
                    strncpy(n->name, (const char *)name, BT_NAME_LEN - 1);
                    n->name[BT_NAME_LEN - 1] = '\0';
                }
                n->state = NAME_KNOWN;
            }
            name_request_next();
        }

        int idx = find_slot_by_addr(addr);
        if (idx < 0) return;
        if (name && items[idx].name[0] == '\0')
            set_name(&items[idx], name, strnlen((const char *)name, BT_NAME_LEN - 1));
        return;
    }

    /* Classic inquiry complete.  Continuous mode: bt_scan_poll() starts the
     * next inquiry.  One-shot: signal done.  The one-shot BLE scan is
     * stopped by bt_scan_stop() after the results are read, so the HCI
     * disable command doesn't race with the next gap_inquiry_start() call. */
    if (event == GAP_EVENT_INQUIRY_COMPLETE) {
        g_inq_active = false;
        if (g_cont) return;
        g_classic_done = true;
        g_scan_done = true;
        if (g_state == BT_STATE_SCANNING) g_state = BT_STATE_IDLE;
        return;
    }

    /* BLE advertising report. */
    if (event == GAP_EVENT_ADVERTISING_REPORT) {
        bd_addr_t addr;
        gap_event_advertising_report_get_address(packet, addr);
        int8_t rssi = ble_report_rssi(packet);
        uint8_t       ad_len  = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
        uint8_t       evt     =
            gap_event_advertising_report_get_advertising_event_type(packet);
        bool          rsp     = evt == 4u;   /* SCAN_RSP */

        /* Already seen this scan: refresh the RSSI (see Classic above) and
         * merge this report's AD fields — a device often spreads its name
         * and other fields over several packets.  In continuous mode the
         * fill list is not visible to readers yet, so fields may change.
         * A one-shot slot is already published: until the device has a
         * name, only fill fields that are still unknown. */
        int dup = find_slot_by_addr(addr);
        if (dup >= 0) {
            if (rssi != BT_RSSI_UNKNOWN) items[dup].rssi = rssi;
            count_report(&items[dup]);
            /* The first advertisement sets the type; a device can mix
             * types (e.g. ADV_IND and NONCONN), so it does not flip. */
            if (!rsp && items[dup].adv_type == BT_ADV_TYPE_NONE)
                items[dup].adv_type = evt;
            bool had_name = items[dup].name[0] != '\0';
            if (g_tgt_cont || !had_name) {
                extract_ble_adv_data(ad_data, ad_len, rsp, !g_tgt_cont,
                                     &items[dup]);
                if (g_tgt_cont && !had_name && items[dup].name[0] != '\0')
                    name_store(items[dup].addr, items[dup].name);
            }
            return;
        }

        int idx = next_slot();
        if (idx < 0) return;

        slot_init(&items[idx], addr, BT_DEVTYPE_BLE, rssi);
        /* 2 and 3 are resolved identities, which need a bond: none here. */
        items[idx].addr_type = gap_event_advertising_report_get_address_type(packet) & 1u;
        if (!rsp) items[idx].adv_type = evt;

        extract_ble_adv_data(ad_data, ad_len, rsp, false, &items[idx]);
        if (g_tgt_cont) {
            if (items[idx].name[0] != '\0') {
                name_store(items[idx].addr, items[idx].name);
            } else {
                bt_name_ent_t *n = name_find(items[idx].addr);
                if (n != NULL && n->state == NAME_KNOWN)
                    memcpy(items[idx].name, n->name, BT_NAME_LEN);
            }
        }
        publish_slot();
    }
}

/* ---- public API ---------------------------------------------------------- */
bt_state_t bt_get_state(void) { return g_state; }

int bt_scan(void)
{
    /* Check, reset and start in one locked section so the buffer is never
     * reset under a scan that is still delivering results. */
    cyw43_arch_lwip_begin();
    if (g_state == BT_STATE_OFF || g_state == BT_STATE_SCANNING) {
        cyw43_arch_lwip_end();
        return -1;
    }

    if (g_cont) {
        cyw43_arch_lwip_end();
        return -1;
    }

    g_tgt_cont     = false;
    g_scan_count   = 0;
    g_scan_done    = false;
    g_classic_done = false;
    g_name_busy    = false;
    g_state        = BT_STATE_SCANNING;

    /* Classic inquiry: 5 × 1.28 s ≈ 6.4 s window. */
    g_inq_active = (gap_inquiry_start(5) == 0);
    /* BLE active scan, so devices that put their name only in the scan
     * response get one: interval 48 slots (30 ms), window 30 (18.75 ms). */
    gap_set_scan_parameters(1, 48, 30);
    gap_start_scan();
    cyw43_arch_lwip_end();

    return 0;
}

int bt_scan_is_done(void)
{
    return g_scan_done ? 1 : 0;
}

int bt_copy_scan_results(bt_scan_result_t *buf, int max)
{
    if (buf == NULL || max < 0) return -1;
    cyw43_arch_lwip_begin();
    int n = g_scan_count < max ? g_scan_count : max;
    memcpy(buf, g_scan, (size_t)n * sizeof(buf[0]));
    cyw43_arch_lwip_end();
    return n;
}

/* Deprecated — see bluetooth.h.  Kept as a teaching example of a buffer
 * shared with an IRQ-context writer and no lock. */
int bt_get_scan_results(const bt_scan_result_t **out, int *out_count)
{
    *out       = g_scan;
    *out_count = (int)g_scan_count;
    return 0;
}

/* ---- continuous scanning ------------------------------------------------- */
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

/* Caller holds g_cont_lock. */
static void cont_stop_locked(void)
{
    cyw43_arch_lwip_begin();
    if (g_cont) {
        g_cont = false;
        gap_stop_scan();
        gap_inquiry_stop();     /* its INQUIRY_COMPLETE takes the one-shot path */
        if (g_state == BT_STATE_SCANNING) g_state = BT_STATE_IDLE;
    }
    cyw43_arch_lwip_end();
    event_flags_set(&g_cont_ev, CONT_EV_STOP);
}

static void bt_listen_thread(void *arg)
{
    (void)arg;
    const bt_scan_list_t *l;
    while (bt_scan_wait(&l) == 0) {
        g_cb(l, g_cb_ctx);
    }
    kmutex_lock(&g_cont_lock);
    if (g_listen_tid == CURRENT_TCB->tid) g_listen_tid = 0u;
    kmutex_unlock(&g_cont_lock);
}

int bt_scan_start(bt_scan_cb_t cb, void *ctx)
{
    uint32_t me = (uint32_t)sys_getpid();

    kmutex_lock(&g_cont_lock);
    if (g_cont && owner_gone()) cont_stop_locked();   /* reclaim a dead owner */
    if (g_cont || listener_alive()) {
        kmutex_unlock(&g_cont_lock);
        return BT_ERR_BUSY;
    }

    cyw43_arch_lwip_begin();
    if (g_state == BT_STATE_OFF || g_state == BT_STATE_ERROR) {
        cyw43_arch_lwip_end();
        kmutex_unlock(&g_cont_lock);
        return BT_ERR_NOTREADY;
    }
    if (g_state == BT_STATE_SCANNING) {     /* one-shot bt_scan() running */
        cyw43_arch_lwip_end();
        kmutex_unlock(&g_cont_lock);
        return BT_ERR_BUSY;
    }

    scanbuf_init(&g_sb);
    for (int i = 0; i < 3; i++) g_lists[i].count = 0;
    memset(g_names, 0, sizeof(g_names));
    g_name_next  = 0u;
    g_name_busy  = false;
    g_tgt_cont   = true;
    g_state      = BT_STATE_SCANNING;
    g_inq_active   = (gap_inquiry_start(BT_INQUIRY_LEN) == 0);
    g_inq_start_us = time_us_64();
    /* BLE active scan, window == interval (48 slots, 30 ms): listen all the
     * time the controller is not busy with inquiry.  Active, so devices that
     * put their name only in the scan response get one.  Report every
     * packet, not just the first per device, so RSSI keeps updating. */
    gap_set_scan_parameters(1, 48, 48);
    gap_set_scan_duplicate_filter(false);
    gap_start_scan();

    g_window_start_us = g_inq_start_us;
    g_owner_pid       = me;
    g_cb              = cb;
    g_cb_ctx          = ctx;
    g_cont            = true;
    event_flags_clear(&g_cont_ev, CONT_EV_READY | CONT_EV_STOP);
    cyw43_arch_lwip_end();

    if (cb != NULL) {
        pcb_t *proc = task_find_process(me);
        tcb_t *t = proc ? task_create_thread(proc, "bt-listen",
                                             bt_listen_thread, NULL,
                                             CURRENT_TCB->priority,
                                             DEFAULT_STACK_SIZE)
                        : NULL;
        if (t == NULL) {
            cont_stop_locked();
            kmutex_unlock(&g_cont_lock);
            return BT_ERR_NOMEM;
        }
        g_listen_tid = t->tid;
    }
    kmutex_unlock(&g_cont_lock);
    return 0;
}

void bt_scan_stop(void)
{
    kmutex_lock(&g_cont_lock);
    if (g_cont) {
        cont_stop_locked();
    } else {
        /* One-shot: stop the BLE scan bt_scan() leaves running, and cut the
         * inquiry short if it is still going. */
        cyw43_arch_lwip_begin();
        gap_stop_scan();
        if (g_state == BT_STATE_SCANNING) {
            gap_inquiry_stop();
            g_scan_done = true;
            g_state     = BT_STATE_IDLE;
        }
        cyw43_arch_lwip_end();
    }
    kmutex_unlock(&g_cont_lock);
}

bool bt_scan_running(void) { return g_cont; }

int bt_scan_wait(const bt_scan_list_t **list)
{
    if (list == NULL) return BT_ERR_ARG;

    for (;;) {
        event_flags_wait(&g_cont_ev, CONT_EV_READY | CONT_EV_STOP, false);

        /* Clear before taking; see wifi_scan_wait(). */
        kmutex_lock(&g_cont_lock);
        event_flags_clear(&g_cont_ev, CONT_EV_READY);
        bool running = g_cont;
        bool took = running && scanbuf_take(&g_sb);
        uint8_t held = g_sb.held;
        kmutex_unlock(&g_cont_lock);

        if (!running) return BT_ERR_STOPPED;
        if (took) {
            *list = &g_lists[held];
            return 0;
        }
    }
}

void bt_scan_poll(void)
{
    if (!g_cont) return;

    bool published = false;
    kmutex_lock(&g_cont_lock);
    if (g_cont && owner_gone()) cont_stop_locked();

    if (g_cont) {
        uint64_t now = time_us_64();
        cyw43_arch_lwip_begin();
        /* Start the next Classic inquiry here rather than in packet_handler,
         * so a refused start (e.g. HCI busy) is simply retried 10 ms later. */
        if (!g_inq_active &&
            now - g_inq_start_us >= (uint64_t)BT_INQUIRY_PERIOD_MS * 1000u &&
            gap_inquiry_start(BT_INQUIRY_LEN) == 0) {
            g_inq_active   = true;
            g_inq_start_us = now;
        }

        uint32_t ms = (uint32_t)((now - g_window_start_us) / 1000u);
        if (ms >= BT_WINDOW_MS) {
            g_lists[g_sb.fill].window_ms = ms;
            scanbuf_publish(&g_sb);
            g_lists[g_sb.ready].seq     = g_sb.seq;
            g_lists[g_sb.ready].dropped = g_sb.dropped;
            g_lists[g_sb.fill].count    = 0;
            g_window_start_us = now;
            published = true;
        }
        cyw43_arch_lwip_end();
    }
    kmutex_unlock(&g_cont_lock);

    if (published) event_flags_set(&g_cont_ev, CONT_EV_READY);
}

const char *bt_devclass_str(bt_devclass_t cls)
{
    switch (cls) {
        case BT_CLASS_COMPUTER:   return "computer";
        case BT_CLASS_PHONE:      return "phone";
        case BT_CLASS_NETWORK:    return "network";
        case BT_CLASS_AUDIO:      return "audio";
        case BT_CLASS_PERIPHERAL: return "peripheral";
        case BT_CLASS_IMAGING:    return "imaging";
        case BT_CLASS_WEARABLE:   return "wearable";
        case BT_CLASS_TOY:        return "toy";
        case BT_CLASS_HEALTH:     return "health";
        case BT_CLASS_OTHER:      return "other";
        default:                  return "unknown";
    }
}

/* ---- shell command ------------------------------------------------------- */
static const char *state_str(bt_state_t s)
{
    switch (s) {
        case BT_STATE_OFF:      return "off";
        case BT_STATE_IDLE:     return "idle";
        case BT_STATE_SCANNING: return "scanning";
        case BT_STATE_ERROR:    return "error";
        default:                return "unknown";
    }
}

static void print_addr(const uint8_t addr[BT_ADDR_LEN])
{
    shell_print("%02X:%02X:%02X:%02X:%02X:%02X",
                addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static void print_hex(const uint8_t *d, int n)
{
    for (int i = 0; i < n; i++) shell_print("%02X", d[i]);
}

/* `bt scan -v`: one more line per device with every optional field set. */
static void print_fields(const bt_scan_result_t *r)
{
    shell_print("    pkts=%u", (unsigned)r->pkt_count);
    if (r->addr_type != BT_ADDR_TYPE_NONE)
        shell_print(" addr=%s", r->addr_type == BT_ADDR_PUBLIC ? "public" : "random");
    if (r->adv_type != BT_ADV_TYPE_NONE) shell_print(" adv=%u", r->adv_type);
    if (r->class_of_device)              shell_print(" cod=%06lX",
                                                     (unsigned long)r->class_of_device);
    if (r->tx_power != BT_TX_POWER_UNKNOWN) shell_print(" tx=%d", r->tx_power);
    if (r->flags != BT_FLAGS_NONE)       shell_print(" flags=%02X", r->flags);
    if (r->appearance != BT_APPEARANCE_NONE)
        shell_print(" app=%04X", r->appearance);
    if (r->service_uuid != BT_SERVICE_NONE) shell_print(" svc=%04X", r->service_uuid);
    if (r->uuid32 != BT_UUID32_NONE)     shell_print(" uuid=%08lX",
                                                     (unsigned long)r->uuid32);
    if (r->did_source != BT_DID_NONE)
        shell_print(" did=%u:%04X:%04X:%04X", r->did_source, r->did_vendor,
                    r->did_product, r->did_version);
    if (r->company_id != BT_COMPANY_NONE) {
        shell_print(" mfr=%04X:", r->company_id);
        print_hex(r->mfr_data, r->mfr_data_len);
    }
    if (r->svc_data_uuid != BT_SERVICE_NONE) {
        shell_print(" data=%04X:", r->svc_data_uuid);
        print_hex(r->svc_data, r->svc_data_len);
    }
    shell_print("\r\n");
}

static int cmd_bt(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : "status";

    if (strcmp(sub, "status") == 0) {
        shell_print("Bluetooth state: %s%s\r\n", state_str(g_state),
                    g_cont ? " (continuous scan on)" : "");
        return 0;
    }

    if (strcmp(sub, "scan") == 0) {
        bool verbose = argc >= 3 && strcmp(argv[2], "-v") == 0;
        if (g_state == BT_STATE_OFF) {
            shell_print("Bluetooth not ready\r\n");
            return -1;
        }
        shell_print("Scanning (~7 s)...\r\n");
        if (bt_scan() != 0) {
            shell_print("Scan failed\r\n");
            return -1;
        }
        for (int t = 0; !g_scan_done && t < 120; t++)
            sys_sleep(100);
        bool done = g_scan_done;
        bt_scan_stop();
        if (!done) {
            shell_print("Scan timed out\r\n");
            return -1;
        }

        /* Print from a snapshot: shell_print can block on USB, and the
         * async context lock must not be held that long. */
        static bt_scan_result_t res[BT_MAX_SCAN_RESULTS];
        int n = bt_copy_scan_results(res, BT_MAX_SCAN_RESULTS);
        if (n <= 0) {
            shell_print("No devices found\r\n");
            return 0;
        }

        shell_print("%-17s  %4s  %-7s  %-10s  %s\r\n",
                    "Address", "RSSI", "Type", "Class", "Name");
        shell_print("%-17s  %4s  %-7s  %-10s  %s\r\n",
                    "-----------------", "----", "-------", "----------", "----");
        for (int i = 0; i < n; i++) {
            const bt_scan_result_t *r = &res[i];
            print_addr(r->addr);
            shell_print("  %4d  %-7s  %-10s  %s\r\n",
                        (int)r->rssi,
                        r->type == BT_DEVTYPE_CLASSIC ? "Classic" : "BLE",
                        bt_devclass_str(r->dev_class),
                        r->name[0] ? r->name : "(unknown)");
            if (verbose) print_fields(r);
        }
        return 0;
    }

    if (strcmp(sub, "watch") == 0) {
        int windows = (argc >= 3) ? atoi(argv[2]) : 5;
        if (windows < 1) windows = 1;
        int rc = bt_scan_start(NULL, NULL);
        if (rc == BT_ERR_BUSY) {
            shell_print("A scan is already running\r\n");
            return -1;
        }
        if (rc != 0) {
            shell_print("Bluetooth not ready (%d)\r\n", rc);
            return -1;
        }
        for (int w = 0; w < windows; w++) {
            const bt_scan_list_t *l;
            if (bt_scan_wait(&l) != 0) break;
            shell_print("-- window %u: %u ms, %d devices, %u dropped\r\n",
                        (unsigned)l->seq, (unsigned)l->window_ms,
                        l->count, (unsigned)l->dropped);
            for (int i = 0; i < l->count; i++) {
                const bt_scan_result_t *r = &l->items[i];
                shell_print("   ");
                print_addr(r->addr);
                shell_print("  %4d  %-7s  %s\r\n", (int)r->rssi,
                            r->type == BT_DEVTYPE_CLASSIC ? "Classic" : "BLE",
                            r->name[0] ? r->name : "(unknown)");
            }
        }
        bt_scan_stop();
        return 0;
    }

    shell_print("Usage: bt [status|scan [-v]|watch [n]]\r\n");
    return -1;
}

static const shell_cmd_t bt_cmd = {
    "bt",
    "bt [status|scan [-v]|watch [n]]",
    cmd_bt
};

/* ---- bt_init ------------------------------------------------------------- */
void bt_init(void)
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    kmutex_init(&g_cont_lock);
    event_flags_init(&g_cont_ev);
    event_flags_set(&g_cont_ev, CONT_EV_STOP);   /* not running yet */

    /* btstack_cyw43_init() hooks BTstack into the async context that was
     * already created by cyw43_arch_init() (called from wifi_init()).
     * After this call, cyw43_arch_poll() drives both WiFi and BTstack. */
    if (!btstack_cyw43_init(cyw43_arch_async_context())) {
        printf("[bt] btstack_cyw43_init failed\r\n");
        g_state = BT_STATE_ERROR;
        return;
    }

    hci_event_cb_reg.callback = packet_handler;
    hci_add_event_handler(&hci_event_cb_reg);

    /* Power on the BT radio asynchronously.  The HCI init sequence runs in
     * the async context; packet_handler moves g_state from OFF to IDLE when
     * BTstack reports HCI_STATE_WORKING. */
    /* BTstack's default inquiry mode is standard, whose results carry no
     * RSSI.  RSSI + EIR adds it, and the EIR gives many devices' name and
     * Device ID record with no remote name request.  Must precede power on:
     * the mode is written during HCI init. */
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    hci_power_control(HCI_POWER_ON);

    shell_register_cmd(&bt_cmd);
    printf("[bt] BTstack initialized (scan mode)\r\n");
}

#endif /* PICOOS_BT_ENABLE */
