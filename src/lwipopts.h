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

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/*
 * lwipopts.h — lwIP configuration for picoOS
 *
 * Built with pico_cyw43_arch_lwip_threadsafe_background, which drives CYW43
 * from a hardware alarm IRQ.  NO_SYS=1 still applies — it tells lwIP not to
 * use its own OS abstraction layer, independent of the CYW43 arch variant.
 *
 * Memory budget note (RP2040, 264 KB SRAM):
 *   lwIP pool (MEM_SIZE)        4 KB
 *   PBUF pool (16 × ~556 B)    ~9 KB
 *   TCP/UDP state structs        ~2 KB
 *   ─────────────────────────────────
 *   Total lwIP overhead         ~15 KB  (within the ~102 KB headroom)
 */

/* ---- Required by lwIP ----------------------------------------------------- */
#define NO_SYS                      1   /* lwIP runs without its own OS layer */
#define LWIP_SOCKET                 0   /* no POSIX socket API                */
#define LWIP_NETCONN                0   /* no netconn API                     */

/* ---- Memory allocator ----------------------------------------------------- */
/* MEM_LIBC_MALLOC must be 0 with threadsafe_background: CYW43 callbacks run
 * from an IRQ context where calling libc malloc is not safe. */
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4096    /* lwIP internal heap size          */

/* ---- Packet buffers ------------------------------------------------------- */
#define PBUF_POOL_SIZE              16      /* number of pre-allocated pbufs    */

/* ---- Protocols ------------------------------------------------------------ */
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1       /* ping support                     */
#define LWIP_RAW                    1
#define LWIP_IPV4                   1
#define LWIP_UDP                    1       /* required for DHCP                */
#define LWIP_TCP                    1       /* available for future student use */
#define LWIP_DHCP                   1       /* DHCP client for IP assignment    */
#define LWIP_DNS                    0       /* not needed yet                   */
#define LWIP_IGMP                   1       /* multicast group membership       */

/* ---- TCP tuning (conservative for RP2040) --------------------------------- */
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((2 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define MEMP_NUM_TCP_SEG            16

/* ---- Callbacks ------------------------------------------------------------ */
#define LWIP_NETIF_STATUS_CALLBACK  1   /* called when IP address changes      */
#define LWIP_NETIF_LINK_CALLBACK    1   /* called when link goes up/down       */
#define LWIP_NETIF_HOSTNAME         1   /* allow setting a hostname            */
#define LWIP_NETIF_TX_SINGLE_PBUF   1   /* more efficient for CYW43           */

/* ---- DHCP tweaks ---------------------------------------------------------- */
#define DHCP_DOES_ARP_CHECK         0   /* skip ARP probe — faster association */
#define LWIP_DHCP_DOES_ACD_CHECK    0   /* skip address conflict detection     */

/* ---- Checksum ------------------------------------------------------------- */
#define LWIP_CHKSUM_ALGORITHM       3   /* best algorithm for Cortex-M0+      */

/* ---- Debug (disabled in production builds) -------------------------------- */
#define LWIP_DEBUG                  0
#define LWIP_STATS                  0

#endif /* LWIPOPTS_H */
