#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/*
 * lwipopts.h — lwIP configuration for picoOS
 *
 * Required by lwIP when building with pico_cyw43_arch_lwip_poll.
 * The polling arch sets PICO_CYW43_ARCH_POLL=1 and requires NO_SYS=1.
 *
 * Memory budget note (RP2040, 264 KB SRAM):
 *   lwIP pool (MEM_SIZE)        4 KB
 *   PBUF pool (24 × ~556 B)    ~13 KB
 *   TCP/UDP state structs        ~2 KB
 *   ─────────────────────────────────
 *   Total lwIP overhead         ~19 KB  (within the ~102 KB headroom)
 */

/* ---- Required by pico_cyw43_arch_lwip_poll -------------------------------- */
#define NO_SYS                      1   /* polling model, no RTOS             */
#define LWIP_SOCKET                 0   /* no POSIX socket API                */
#define LWIP_NETCONN                0   /* no netconn API                     */

/* ---- Memory allocator ----------------------------------------------------- */
/* MEM_LIBC_MALLOC=1 routes lwIP's internal allocator through the C library
 * malloc/free.  Compatible with the poll arch; saves the separate lwIP heap. */
#define MEM_LIBC_MALLOC             1
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4096    /* fallback pool if libc malloc off */

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
