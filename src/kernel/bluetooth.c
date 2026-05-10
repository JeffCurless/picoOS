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
#include "kernel/syscall.h"
#include "shell/shell.h"
#include <string.h>
#include <stdio.h>

/* ---- state --------------------------------------------------------------- */
static volatile bt_state_t  g_state        = BT_STATE_OFF;
static bt_scan_result_t     g_scan[BT_MAX_SCAN_RESULTS];
static volatile int         g_scan_count   = 0;
static volatile bool        g_scan_done    = false;
static volatile bool        g_classic_done = false;

static btstack_packet_callback_registration_t hci_event_cb_reg;

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
    for (int i = 0; i < g_scan_count; i++) {
        bd_addr_t slot_addr;
        reverse_bd_addr(g_scan[i].addr, slot_addr);
        if (bd_addr_cmp(addr, slot_addr) == 0) return i;
    }
    return -1;
}

/* Reserve a new slot; returns index or -1 if full. */
static int alloc_slot(void)
{
    if (g_scan_count >= BT_MAX_SCAN_RESULTS) return -1;
    return g_scan_count++;
}

/* ---- BLE AD data parser: extracts name, flags, TX power, company ID ------- */
static void extract_ble_adv_data(const uint8_t *ad_data, uint8_t ad_len,
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
        case 0x08u: /* Shortened Local Name */
        case 0x09u: /* Complete Local Name */
            if (dev->name[0] == '\0') {
                int copy = (dlen < (uint8_t)(BT_NAME_LEN - 1)) ? dlen : (BT_NAME_LEN - 1);
                memcpy(dev->name, d, (size_t)copy);
                dev->name[copy] = '\0';
            }
            break;
        case 0x01u: /* Flags */
            dev->flags = d[0];
            break;
        case 0x0Au: /* TX Power Level */
            dev->tx_power = (int8_t)d[0];
            break;
        case 0xFFu: /* Manufacturer Specific Data — first 2 bytes are company ID (LE) */
            if (dlen >= 2)
                dev->company_id = (uint16_t)((uint16_t)d[1] << 8 | d[0]);
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

    /* Classic inquiry result — one device per event. */
    if (event == GAP_EVENT_INQUIRY_RESULT) {
        bd_addr_t addr;
        gap_event_inquiry_result_get_bd_addr(packet, addr);

        /* Skip duplicates. */
        if (find_slot_by_addr(addr) >= 0) return;

        int idx = alloc_slot();
        if (idx < 0) return;

        reverse_bd_addr(addr, g_scan[idx].addr);
        g_scan[idx].name[0]         = '\0';
        g_scan[idx].type            = BT_DEVTYPE_CLASSIC;
        g_scan[idx].class_of_device =
            gap_event_inquiry_result_get_class_of_device(packet);
        g_scan[idx].dev_class       =
            cod_to_devclass(g_scan[idx].class_of_device);
        g_scan[idx].rssi            =
            gap_event_inquiry_result_get_rssi_available(packet)
                ? gap_event_inquiry_result_get_rssi(packet)
                : -127;
        g_scan[idx].tx_power        = BT_TX_POWER_UNKNOWN;
        g_scan[idx].flags           = BT_FLAGS_NONE;
        g_scan[idx].company_id      = BT_COMPANY_NONE;

        /* Request the human-readable name asynchronously. */
        gap_remote_name_request(
            addr,
            gap_event_inquiry_result_get_page_scan_repetition_mode(packet),
            gap_event_inquiry_result_get_clock_offset(packet) | 0x8000u);
        return;
    }

    /* Classic remote name response. */
    if (event == HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE) {
        bd_addr_t addr;
        hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
        int idx = find_slot_by_addr(addr);
        if (idx < 0) return;
        const uint8_t *name =
            hci_event_remote_name_request_complete_get_remote_name(packet);
        if (name) {
            strncpy(g_scan[idx].name, (const char *)name, BT_NAME_LEN - 1);
            g_scan[idx].name[BT_NAME_LEN - 1] = '\0';
        }
        return;
    }

    /* Classic inquiry complete — stop BLE scan and signal done. */
    if (event == GAP_EVENT_INQUIRY_COMPLETE) {
        g_classic_done = true;
        gap_stop_scan();
        g_scan_done = true;
        if (g_state == BT_STATE_SCANNING) g_state = BT_STATE_IDLE;
        return;
    }

    /* BLE advertising report. */
    if (event == GAP_EVENT_ADVERTISING_REPORT) {
        bd_addr_t addr;
        gap_event_advertising_report_get_address(packet, addr);

        /* Skip duplicates. */
        if (find_slot_by_addr(addr) >= 0) return;

        int idx = alloc_slot();
        if (idx < 0) return;

        reverse_bd_addr(addr, g_scan[idx].addr);
        g_scan[idx].name[0]         = '\0';
        g_scan[idx].type            = BT_DEVTYPE_BLE;
        g_scan[idx].dev_class       = BT_CLASS_UNKNOWN;
        g_scan[idx].class_of_device = 0;
        g_scan[idx].rssi            =
            gap_event_advertising_report_get_rssi(packet);
        g_scan[idx].tx_power        = BT_TX_POWER_UNKNOWN;
        g_scan[idx].flags           = BT_FLAGS_NONE;
        g_scan[idx].company_id      = BT_COMPANY_NONE;

        uint8_t       ad_len  = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
        extract_ble_adv_data(ad_data, ad_len, &g_scan[idx]);
    }
}

/* ---- public API ---------------------------------------------------------- */
bt_state_t bt_get_state(void) { return g_state; }

int bt_scan(void)
{
    if (g_state == BT_STATE_OFF) return -1;
    g_scan_count   = 0;
    g_scan_done    = false;
    g_classic_done = false;
    g_state        = BT_STATE_SCANNING;

    /* Classic inquiry: 5 × 1.28 s ≈ 6.4 s window. */
    gap_inquiry_start(5);

    /* BLE passive scan: interval 48 slots (30 ms), window 30 slots (18.75 ms). */
    gap_set_scan_parameters(0, 48, 30);
    gap_start_scan();

    return 0;
}

int bt_scan_is_done(void)
{
    return g_scan_done ? 1 : 0;
}

int bt_get_scan_results(const bt_scan_result_t **out, int *out_count)
{
    *out       = g_scan;
    *out_count = (int)g_scan_count;
    return 0;
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

static int cmd_bt(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : "status";

    if (strcmp(sub, "status") == 0) {
        shell_print("Bluetooth state: %s\r\n", state_str(g_state));
        return 0;
    }

    if (strcmp(sub, "scan") == 0) {
        if (g_state == BT_STATE_OFF) {
            shell_print("Bluetooth not initialized\r\n");
            return -1;
        }
        shell_print("Scanning (~7 s)...\r\n");
        if (bt_scan() != 0) {
            shell_print("Scan failed\r\n");
            return -1;
        }
        while (!g_scan_done) { sys_sleep(100); }

        if (g_scan_count == 0) {
            shell_print("No devices found\r\n");
            return 0;
        }

        shell_print("%-17s  %4s  %-7s  %-10s  %s\r\n",
                    "Address", "RSSI", "Type", "Class", "Name");
        shell_print("%-17s  %4s  %-7s  %-10s  %s\r\n",
                    "-----------------", "----", "-------", "----------", "----");
        for (int i = 0; i < g_scan_count; i++) {
            const bt_scan_result_t *r = &g_scan[i];
            print_addr(r->addr);
            shell_print("  %4d  %-7s  %-10s  %s\r\n",
                        (int)r->rssi,
                        r->type == BT_DEVTYPE_CLASSIC ? "Classic" : "BLE",
                        bt_devclass_str(r->dev_class),
                        r->name[0] ? r->name : "(unknown)");
        }
        return 0;
    }

    shell_print("Usage: bt [status|scan]\r\n");
    return -1;
}

static const shell_cmd_t bt_cmd = {
    "bt",
    "bt [status|scan]",
    cmd_bt
};

/* ---- bt_init ------------------------------------------------------------- */
void bt_init(void)
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

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

    /* Power on the BT radio asynchronously.  The HCI init sequence completes
     * once the wifi-poll thread starts calling cyw43_arch_poll(). */
    hci_power_control(HCI_POWER_ON);
    g_state = BT_STATE_IDLE;

    shell_register_cmd(&bt_cmd);
    printf("[bt] BTstack initialized (scan mode)\r\n");
}

#endif /* PICOOS_BT_ENABLE */
