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

/*
 * btstack_config.h — Minimal BTstack configuration for picoOS scan-only use.
 *
 * BTstack sources #include "btstack_config.h" without a path prefix, so this
 * file must live in src/ which is already on the compiler include path.
 *
 * ENABLE_BLE and ENABLE_CLASSIC are injected by the CMake library targets
 * (pico_btstack_ble, pico_btstack_classic) and must NOT be defined here.
 *
 * All MAX_NR_* values are set to 0 because picoOS only scans — it never
 * establishes L2CAP channels, RFCOMM sessions, or HCI connections.
 */

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* CYW43 HCI transport requires a 4-byte header before each packet. */
#define HCI_OUTGOING_PRE_BUFFER_SIZE            4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT            4
#define HCI_INCOMING_PRE_BUFFER_SIZE            6   /* gatt_client.c requires >= 6 */

/* Minimum ACL payload size satisfying l2cap.h constraints:
 *   Classic L2CAP minimum MTU 48 B + 4 B header = 52 B
 *   BLE L2CAP minimum MTU    23 B + 4 B header = 27 B
 * 64 B is the smallest power-of-two that covers both. */
#define HCI_ACL_PAYLOAD_SIZE                    64

/* Required by hci_dump_embedded_stdout.c (compiled as part of pico_btstack_base). */
#define ENABLE_PRINTF_HEXDUMP                   1

/* BTstack BLE ATT utility requires one of these to be defined.
 * HAVE_MALLOC is the simplest choice; Pico's pico_stdlib provides malloc. */
#define HAVE_MALLOC                             1

/* Enable BLE central role (observer/scanner) and Classic inquiry.
 * ENABLE_LE_PERIPHERAL is also required: BTstack hci.c accesses
 * le_advertisements_state (a PERIPHERAL-only struct field) outside its
 * own #ifdef guard when ENABLE_LE_CENTRAL is active — without it, the
 * struct field is absent and compilation fails.  Cost: a few extra bytes. */
#define ENABLE_LE_CENTRAL                       1
#define ENABLE_LE_PERIPHERAL                    1

/* NVM device database entry count — required by le_device_db_tlv.c (must be > 0). */
#define NVM_NUM_DEVICE_DB_ENTRIES               1

/* Classic link key storage — required by btstack_link_key_db_tlv.c. */
#define NVM_NUM_LINK_KEYS                       1

/* No connections, services, or channels needed for scan-only. */
#define MAX_NR_HCI_CONNECTIONS                  0
#define MAX_NR_LE_DEVICE_DB_ENTRIES             0   /* no BLE pairing/bonding */
#define MAX_NR_L2CAP_SERVICES                   0
#define MAX_NR_L2CAP_CHANNELS                   0
#define MAX_NR_RFCOMM_MULTIPLEXERS              0
#define MAX_NR_RFCOMM_SERVICES                  0
#define MAX_NR_RFCOMM_CHANNELS                  0
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 0

#endif /* BTSTACK_CONFIG_H */
