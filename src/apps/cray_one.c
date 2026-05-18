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

#if defined(PICOOS_WIFI_ENABLE) && defined(PICOOS_DISPLAY_ENABLE)

#include "kernel/wifi.h"
#include "kernel/vfs.h"
#include "kernel/syscall.h"
#include "kernel/sync.h"
#include "kernel/dev.h"
#include "drivers/display.h"
#include "shell/shell.h"

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __arm__
#include "lwip/udp.h"
#include "lwip/igmp.h"
#include "lwip/pbuf.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#endif

/* -------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */
#define CONFIG_FILE          "config.txt"
#define CONFIG_BUFSZ         256
#define CRED_MAXLEN          64

#define MCAST_PORT           4210u
#define ANNOUNCE_INTERVAL_MS 2000u
#define DISPLAY_INTERVAL_MS   100u

/* Maximum supported cluster size — bounds message buffer and color arrays */
#define MAXNODES_MAX         16u

/* "<nodeid>:<c0>,<c1>,...,<cN-1>\0"
 * worst case: 2 + 1 + MAXNODES_MAX*3 - 1 + 1 = MAXNODES_MAX*3 + 3 = 51 for N=16 */
#define MSG_MAX              64u

/* Compile-time sanity: MAXNODES_MAX <= 99 keeps node-ID at most 2 decimal digits,
 * which is what the MSG_WORST formula assumes.  If MAXNODES_MAX is ever raised past
 * 99 the second assert catches the resulting buffer overflow automatically. */
_Static_assert(MAXNODES_MAX <= 99u,
    "MAXNODES_MAX > 99: MSG_MAX formula assumes 2-digit node IDs; "
    "increase MSG_MAX and update this assertion");
_Static_assert(MAXNODES_MAX * 3u + 3u <= MSG_MAX,
    "MSG_MAX too small for MAXNODES_MAX color payload; "
    "raise MSG_MAX or lower MAXNODES_MAX");

/* -------------------------------------------------------------------------
 * Display grid
 *
 * 4 columns × 7 rows = 28 blocks.
 * Display is 240 × 135 pixels.
 *   BLOCK_W = 240 / 4 = 60 px
 *   BLOCK_H = 135 / 7 = 19 px  (7×19 = 133; 2 px unused at bottom)
 *
 * A 1-pixel pad is drawn inside each block so the dark-gray background shows
 * through as grid lines between blocks.
 * ------------------------------------------------------------------------- */
#define GRID_COLS   9u
#define GRID_ROWS   7u
#define GRID_TOTAL  (GRID_COLS * GRID_ROWS)   /* 28 */
#define BLOCK_W     (DISP_WIDTH  / GRID_COLS)  /* 60 */
#define BLOCK_H     (DISP_HEIGHT / GRID_ROWS)  /* 19 */
#define BLOCK_PAD   1u                          /* px gap between blocks */

/* Grid background (visible in the 1-px gaps between blocks) */
#define COLOR_GRID_BG  RGB332(48, 48, 48)

/* -------------------------------------------------------------------------
 * Minimal LCG random number generator
 *
 * Parameters from Knuth / Numerical Recipes (MMIX-style for 32-bit).
 * Seeded from NODEID so each physical Pico picks a distinct deterministic
 * color on every boot.
 * ------------------------------------------------------------------------- */
static uint32_t rng_state = 1u;

static void rng_seed(uint32_t seed)
{
    rng_state = seed ? seed : 1u;
}

static uint32_t rng_next(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* -------------------------------------------------------------------------
 * Color palette — 64 vivid RGB332 colors covering the full hue wheel.
 * With 64 entries two nodes share a color only once in 64 picks on average,
 * making accidental synchronization far less likely than with 8 colors.
 * ------------------------------------------------------------------------- */
static const uint8_t color_palette[] = {
    /* Reds */
    RGB332(255,   0,   0),   /* red           */
    RGB332(219,   0,   0),   /* deep red      */
    RGB332(182,   0,   0),   /* dark red      */
    RGB332(255,  73,   0),   /* red-orange    */
    RGB332(255,   0,  85),   /* rose red      */
    RGB332(255,  73,  85),   /* coral         */
    RGB332(219,  36,   0),   /* brick         */
    RGB332(255,  36,  85),   /* crimson       */
    /* Oranges */
    RGB332(255, 128,   0),   /* orange        */
    RGB332(255, 146,   0),   /* amber         */
    RGB332(219, 109,   0),   /* dark orange   */
    RGB332(255, 182,   0),   /* golden amber  */
    RGB332(255, 109,  85),   /* peach         */
    RGB332(219,  73,   0),   /* burnt orange  */
    RGB332(182,  91,   0),   /* copper        */
    RGB332(255, 128,  85),   /* light peach   */
    /* Yellows */
    RGB332(255, 219,   0),   /* yellow        */
    RGB332(255, 255,   0),   /* bright yellow */
    RGB332(219, 219,   0),   /* dark yellow   */
    RGB332(182, 182,   0),   /* olive         */
    RGB332(255, 255,  85),   /* pale yellow   */
    RGB332(219, 182,   0),   /* golden        */
    RGB332(255, 219,  85),   /* light golden  */
    RGB332(128, 255,   0),   /* chartreuse    */
    /* Greens */
    RGB332(  0, 255,   0),   /* green         */
    RGB332(  0, 219,   0),   /* medium green  */
    RGB332(  0, 182,   0),   /* dark green    */
    RGB332(  0, 146,   0),   /* deep green    */
    RGB332( 73, 255,   0),   /* yellow-green  */
    RGB332( 36, 255,  85),   /* light green   */
    RGB332(  0, 255,  85),   /* spring green  */
    RGB332(  0, 219,  85),   /* mint          */
    /* Cyans */
    RGB332(  0, 255, 255),   /* cyan          */
    RGB332(  0, 219, 219),   /* medium cyan   */
    RGB332(  0, 255, 170),   /* aqua          */
    RGB332(  0, 219, 170),   /* teal          */
    RGB332( 73, 255, 255),   /* light cyan    */
    RGB332(  0, 182, 255),   /* sky blue      */
    RGB332(  0, 146, 219),   /* steel teal    */
    RGB332(  0, 182, 170),   /* dark teal     */
    /* Blues */
    RGB332(  0,   0, 255),   /* blue          */
    RGB332(  0,   0, 170),   /* medium blue   */
    RGB332( 73,  73, 255),   /* periwinkle    */
    RGB332(  0,  73, 255),   /* azure         */
    RGB332( 36,  36, 255),   /* vivid blue    */
    RGB332(109, 109, 255),   /* soft blue     */
    RGB332(  0,  36, 170),   /* deep azure    */
    RGB332( 73, 146, 255),   /* cornflower    */
    /* Purples */
    RGB332(128,   0, 255),   /* purple        */
    RGB332(182,   0, 255),   /* violet        */
    RGB332(146,   0, 219),   /* dark purple   */
    RGB332( 91,   0, 219),   /* indigo        */
    RGB332(164,  73, 255),   /* light purple  */
    RGB332( 73,   0, 182),   /* deep indigo   */
    RGB332(219,  73, 255),   /* orchid        */
    RGB332(109,  36, 255),   /* blue-violet   */
    /* Magentas */
    RGB332(255,   0, 255),   /* magenta       */
    RGB332(219,   0, 219),   /* med magenta   */
    RGB332(255,   0, 170),   /* hot pink      */
    RGB332(219,   0, 170),   /* deep pink     */
    RGB332(255,  73, 255),   /* light magenta */
    RGB332(255, 109, 255),   /* pink          */
    RGB332(255,  73, 170),   /* strawberry    */
    RGB332(219,  73, 182),   /* rose          */
};
#define PALETTE_SIZE  (sizeof(color_palette) / sizeof(color_palette[0]))

/* -------------------------------------------------------------------------
 * Receive message — enqueued by mcast_recv_cb, consumed by the main loop.
 * Sized to fit within MQ_MSG_SIZE (64 bytes):
 *   4 (sender) + 4 (num_colors) + 16 (colors) = 24 bytes.
 * ------------------------------------------------------------------------- */
typedef struct {
    int     sender;
    int     num_colors;
    uint8_t colors[MAXNODES_MAX];
} rx_msg_t;
_Static_assert(sizeof(rx_msg_t) <= MQ_MSG_SIZE,
    "rx_msg_t exceeds MQ_MSG_SIZE; raise MQ_MSG_SIZE or shrink rx_msg_t");

/* -------------------------------------------------------------------------
 * Shared state — written during init, read-only from the RX callback.
 * g_rx_count is written from the RX callback (poll-thread context).
 * ------------------------------------------------------------------------- */
static volatile uint32_t g_rx_count = 0;
static int     g_nodeid   = 0;
static int     g_maxnodes = 1;
static uint8_t       g_my_colors[MAXNODES_MAX]; /* current color set, one per slot */
static mqueue_t      g_rx_mq;                  /* peer updates → display thread   */
static event_flags_t g_cray_done;              /* thread completion signals        */
static volatile bool g_cray_running    = false;
static volatile bool g_my_colors_dirty = false;

#ifdef __arm__
/* Survive a thread kill: store PCB and multicast state so the next run can
 * clean up before allocating new resources. */
static struct udp_pcb *g_pcb         = NULL;
static ip4_addr_t      g_mcast_group_addr;
static bool            g_mcast_active = false;

/* Set by mcast_recv_cb on any parse error; cleared + checked each TX cycle. */
static volatile bool   g_net_error   = false;
#endif

/* -------------------------------------------------------------------------
 * draw_block — fill one grid cell with a solid color.
 *
 * block_idx is the linear index (row-major): col = idx % GRID_COLS,
 * row = idx / GRID_COLS.  BLOCK_PAD pixels are trimmed on every side so the
 * background shows through as grid lines.
 * ------------------------------------------------------------------------- */
static void draw_block(int block_idx, uint8_t color)
{
    int col = block_idx % (int)GRID_COLS;
    int row = block_idx / (int)GRID_COLS;

    disp_rect_arg_t r = {
        .x      = (uint16_t)(col * (int)BLOCK_W + (int)BLOCK_PAD),
        .y      = (uint16_t)(row * (int)BLOCK_H + (int)BLOCK_PAD),
        .w      = (uint16_t)(BLOCK_W - 2u * BLOCK_PAD),
        .h      = (uint16_t)(BLOCK_H - 2u * BLOCK_PAD),
        .color  = color,
        .filled = 1,
        ._pad1  = 0,
        ._pad2  = 0,
    };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &r);
}

/* -------------------------------------------------------------------------
 * paint_node_blocks — color every block owned by a given node.
 *
 * A node owns all blocks where block_index % g_maxnodes == nodeid.
 * (e.g. NODEID=0, MAXNODES=5 → blocks 0, 5, 10, 15, 20, 25)
 * ------------------------------------------------------------------------- */
static void paint_node_blocks(int nodeid, const uint8_t *colors, int num_colors)
{
    int i = 0;
    for (int b = nodeid; b < (int)GRID_TOTAL; b += g_maxnodes) {
        draw_block(b, colors[i % num_colors]);
        i++;
    }
}

/* -------------------------------------------------------------------------
 * draw_initial_grid — clear the display and paint the local node's blocks.
 * ------------------------------------------------------------------------- */
static void draw_initial_grid(void)
{
    /* Re-open the display device to re-arm SPI0.  The WiFi stack can disturb
     * the SPI0 peripheral registers between boot-time dev_init() and the first
     * time cray-one uses the display (which is always after WiFi connects). */
    dev_open(DEV_DISPLAY);

    uint8_t bg = COLOR_GRID_BG;
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_SET_BG, &bg);
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_CLEAR,  NULL);

    /* Draw all blocks black (unowned placeholder) */
    for (int b = 0; b < (int)GRID_TOTAL; b++) {
        draw_block(b, COLOR_BLACK);
    }

    /* Highlight own blocks with this node's chosen colors */
    paint_node_blocks(g_nodeid, g_my_colors, g_maxnodes);

    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);
}

/* -------------------------------------------------------------------------
 * parse_config — scan a NUL-terminated text buffer for known KEY=VALUE pairs.
 *
 * Recognized keys:
 *   SSID=<string>        WiFi network name (primary)
 *   PASSWORD=<string>    WiFi password (primary; absent → open network)
 *   SSIDALT=<string>     Alternate WiFi network name (fallback)
 *   PASSWORDALT=<string> Alternate WiFi password (fallback)
 *   NODEID=<int>         This node's ID (0-based)
 *   MAXNODES=<int>       Total number of nodes in the cluster
 * ------------------------------------------------------------------------- */
static void parse_config(const char *buf,
                          char *ssid,      size_t ssid_sz,
                          char *password,  size_t pass_sz,
                          char *ssid_alt,  size_t ssid_alt_sz,
                          char *pass_alt,  size_t pass_alt_sz,
                          int  *nodeid,
                          int  *maxnodes)
{
    const char *p = buf;
    while (*p) {
        const char *eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        size_t line_len = (size_t)(eol - p);

        /* SSIDALT must be checked before SSID to avoid a prefix match. */
        if (strncmp(p, "SSIDALT=", 8) == 0) {
            size_t vlen = line_len - 8u;
            if (vlen >= ssid_alt_sz) vlen = ssid_alt_sz - 1u;
            memcpy(ssid_alt, p + 8, vlen);
            ssid_alt[vlen] = '\0';

        } else if (strncmp(p, "SSID=", 5) == 0) {
            size_t vlen = line_len - 5u;
            if (vlen >= ssid_sz) vlen = ssid_sz - 1u;
            memcpy(ssid, p + 5, vlen);
            ssid[vlen] = '\0';

        /* PASSWORDALT must be checked before PASSWORD for the same reason. */
        } else if (strncmp(p, "PASSWORDALT=", 12) == 0) {
            size_t vlen = line_len - 12u;
            if (vlen >= pass_alt_sz) vlen = pass_alt_sz - 1u;
            memcpy(pass_alt, p + 12, vlen);
            pass_alt[vlen] = '\0';

        } else if (strncmp(p, "PASSWORD=", 9) == 0) {
            size_t vlen = line_len - 9u;
            if (vlen >= pass_sz) vlen = pass_sz - 1u;
            memcpy(password, p + 9, vlen);
            password[vlen] = '\0';

        } else if (strncmp(p, "NODEID=", 7) == 0) {
            int v = 0;
            for (size_t i = 7u; i < line_len && p[i] >= '0' && p[i] <= '9'; i++)
                v = v * 10 + (p[i] - '0');
            *nodeid = v;

        } else if (strncmp(p, "MAXNODES=", 9) == 0) {
            int v = 0;
            for (size_t i = 9u; i < line_len && p[i] >= '0' && p[i] <= '9'; i++)
                v = v * 10 + (p[i] - '0');
            *maxnodes = v;
        }

        p = eol;
        while (*p == '\n' || *p == '\r') p++;
    }
}

/* -------------------------------------------------------------------------
 * mcast_recv_cb — called by the wifi-poll thread during cyw43_arch_poll()
 * when a UDP datagram arrives on MCAST_PORT.
 *
 * Expected message format: "<nodeid>:<color_hex>"
 *   e.g.  "2:E0"   — node 2, color 0xE0 (bright red in RGB332)
 *
 * The callback:
 *   1. Parses sender node ID and color byte.
 *   2. Ignores messages from self or out-of-range senders.
 *   3. Paints that node's blocks on the display.
 *   4. Flushes the framebuffer.
 * ------------------------------------------------------------------------- */
#ifdef __arm__
static void mcast_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                           const ip_addr_t *src, u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)src;
    (void)port;

    if (p == NULL) {
        return;
    }

    char msg[MSG_MAX];
    u16_t copy_len = p->tot_len < (u16_t)(sizeof(msg) - 1u)
                     ? p->tot_len : (u16_t)(sizeof(msg) - 1u);
    pbuf_copy_partial(p, msg, copy_len, 0);
    msg[copy_len] = '\0';
    pbuf_free(p);

    /* Locate the ':' separator */
    const char *colon = msg;
    while (*colon && *colon != ':') colon++;
    if (*colon != ':') {
        g_net_error = true;
        return;   /* malformed message */
    }

    /* Decode sender node ID (decimal before ':') */
    int sender = 0;
    for (const char *q = msg; q < colon; q++) {
        if (*q >= '0' && *q <= '9') {
            sender = sender * 10 + (*q - '0');
        }
    }

    /* Decode comma-separated hex colors after ':' */
    uint8_t rx_colors[MAXNODES_MAX];
    int rx_num = 0;
    const char *q = colon + 1;
    while (*q && rx_num < (int)MAXNODES_MAX) {
        unsigned v = 0u;
        while (*q && *q != ',') {
            v <<= 4;
            if      (*q >= '0' && *q <= '9') v |= (unsigned)(*q - '0');
            else if (*q >= 'A' && *q <= 'F') v |= (unsigned)(*q - 'A' + 10);
            else if (*q >= 'a' && *q <= 'f') v |= (unsigned)(*q - 'a' + 10);
            q++;
        }
        rx_colors[rx_num++] = (uint8_t)v;
        if (*q == ',') q++;
    }
    if (rx_num == 0) {
        g_net_error = true;
        return;   /* malformed — no colors */
    }

    /* Ignore self and out-of-range senders */
    if (sender == g_nodeid || sender < 0 || sender >= g_maxnodes) {
        if (sender != g_nodeid) {
            /* Out-of-range drop: helps diagnose MAXNODES misconfiguration. */
            printf("[cray-one] RX dropped: sender=%d (nodeid=%d maxnodes=%d)\r\n",
                   sender, g_nodeid, g_maxnodes);
            g_net_error = true;
        }
        return;
    }

    uint32_t count = ++g_rx_count;
    char rx_color_str[MAXNODES_MAX * 3 + 1];
    int rcp = 0;
    for (int ci = 0; ci < rx_num; ci++)
        rcp += snprintf(rx_color_str + rcp, sizeof(rx_color_str) - (size_t)rcp,
                        ci ? ",%02X" : "%02X", (unsigned)rx_colors[ci]);
    printf("[cray-one] #%lu RX node %d colors %s\r\n",
           (unsigned long)count, sender, rx_color_str);

    /* Enqueue for the main loop to paint — no display work in this callback. */
    rx_msg_t m;
    m.sender     = sender;
    m.num_colors = rx_num;
    memcpy(m.colors, rx_colors, (size_t)rx_num);
    if (!mqueue_try_send(&g_rx_mq, &m)) {
        printf("[cray-one] RX queue full — drop node %d\r\n", sender);
        g_net_error = true;
    }
}
#endif /* __arm__ */

/* -------------------------------------------------------------------------
 * cray_tx_thread — picks new colors and broadcasts them every
 * ANNOUNCE_INTERVAL_MS.  Sets g_my_colors_dirty so cray_disp_thread repaints
 * the local node's blocks.  Signals g_cray_done bit 0x1 on exit.
 * ------------------------------------------------------------------------- */
#ifdef __arm__
static void cray_tx_thread(void *arg)
{
    (void)arg;

    char     msg[MSG_MAX];
    uint32_t tx_count = 0;

    while (g_cray_running && wifi_get_state() == WIFI_STATE_UP) {
#ifdef PICOOS_LED_ENABLE
        {
            uint32_t led_color = g_net_error ? 0xFF0000u : 0x0000FFu;
            dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &led_color);
            g_net_error = false;
        }
#endif
        /* Pick a new color set; volatile dirty flag tells display thread. */
        for (int i = 0; i < g_maxnodes && i < (int)MAXNODES_MAX; i++)
            g_my_colors[i] = color_palette[rng_next() % PALETTE_SIZE];
        g_my_colors_dirty = true;

        /* Build "<nodeid>:<c0>,<c1>,...,<cN-1>" */
        int pos = snprintf(msg, sizeof(msg), "%d:", g_nodeid);
        for (int i = 0; i < g_maxnodes && i < (int)MAXNODES_MAX; i++)
            pos += snprintf(msg + pos, sizeof(msg) - (size_t)pos,
                            i ? ",%02X" : "%02X", (unsigned)g_my_colors[i]);
        size_t mlen = (size_t)pos;

        cyw43_arch_lwip_begin();
        struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)mlen, PBUF_RAM);
        if (pb != NULL) {
            memcpy(pb->payload, msg, mlen);
            err_t tx_err = udp_sendto(g_pcb, pb,
                                      (const ip_addr_t *)&g_mcast_group_addr,
                                      MCAST_PORT);
            pbuf_free(pb);
            if (tx_err == ERR_OK) {
                tx_count++;
                printf("[cray-one] TX #%lu %s\r\n",
                       (unsigned long)tx_count, msg);
            } else {
                printf("[cray-one] TX #%lu sendto err %d — skipping\r\n",
                       (unsigned long)(tx_count + 1u), (int)tx_err);
#ifdef PICOOS_LED_ENABLE
                { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
            }
        } else {
            printf("[cray-one] TX #%lu pbuf_alloc failed — skipping\r\n",
                   (unsigned long)(tx_count + 1u));
#ifdef PICOOS_LED_ENABLE
            { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
        }
        cyw43_arch_lwip_end();

        sys_sleep(ANNOUNCE_INTERVAL_MS);
    }

    g_cray_running = false;     /* stop cray_disp_thread */
    event_flags_set(&g_cray_done, 0x1u);
}

/* -------------------------------------------------------------------------
 * cray_disp_thread — wakes every 50 ms, repaints any nodes with pending
 * updates (own or peer), then flushes the display in a single SPI pass.
 * Signals g_cray_done bit 0x2 on exit.
 * ------------------------------------------------------------------------- */
static void cray_disp_thread(void *arg)
{
    (void)arg;

    while (g_cray_running) {
        bool did_paint = false;

        /* Process exactly one update per tick: own-block repaint takes
         * priority; otherwise dequeue one peer update. */
        if (g_my_colors_dirty) {
            g_my_colors_dirty = false;
            paint_node_blocks(g_nodeid, g_my_colors, g_maxnodes);
            did_paint = true;
        } else {
            rx_msg_t rx;
            if (mqueue_try_recv(&g_rx_mq, &rx)) {
                paint_node_blocks(rx.sender, rx.colors, rx.num_colors);
                did_paint = true;
            }
        }

        if (did_paint)
            dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);

        sys_sleep(DISPLAY_INTERVAL_MS);
    }

    event_flags_set(&g_cray_done, 0x2u);
}
#endif /* __arm__ */

/* -------------------------------------------------------------------------
 * cray_one — application entry point.
 *
 * Registered in app_table[] in demo.c; run via:  run cray-one
 *
 * 1. Reads config.txt → SSID, PASSWORD, NODEID, MAXNODES
 * 2. Connects to WiFi — nothing else starts until the link is UP
 * 3. Picks a deterministic random color (seeded by NODEID)
 * 4. Renders the 4×7 grid with own blocks highlighted
 * 5. Joins multicast group 239.255.0.1 / UDP port 4210
 * 6. Spawns cray_tx_thread (2 s TX cycle) and cray_disp_thread (50 ms
 *    display poll); waits for both to finish via event flags
 * 7. Cleans up PCB / display / LED
 * ------------------------------------------------------------------------- */
void cray_one(void *arg)
{
    (void)arg;

#ifdef PICOOS_LED_ENABLE
    dev_open(DEV_LED);
    { uint32_t c = 0x0000FFu; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif

    /* ---- 1. Read and parse config.txt ---- */
    int fd = vfs_open(CONFIG_FILE, VFS_O_RDONLY);
    if (fd < 0) {
        shell_print("[cray-one] ERROR: config.txt not found\r\n");
        shell_print("[cray-one] Required fields: SSID, PASSWORD, NODEID, MAXNODES\r\n");
#ifdef PICOOS_LED_ENABLE
        { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
        return;
    }

    char buf[CONFIG_BUFSZ];
    int  n = vfs_read(fd, (uint8_t *)buf, sizeof(buf) - 1);
    vfs_close(fd);

    if (n <= 0) {
        shell_print("[cray-one] ERROR: config.txt is empty or unreadable\r\n");
#ifdef PICOOS_LED_ENABLE
        { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
        return;
    }
    buf[n] = '\0';

    char ssid[CRED_MAXLEN]      = {0};
    char password[CRED_MAXLEN]  = {0};
    char ssid_alt[CRED_MAXLEN]  = {0};
    char pass_alt[CRED_MAXLEN]  = {0};
    int  nodeid   = 0;
    int  maxnodes = 1;
    parse_config(buf,
                 ssid,     sizeof(ssid),
                 password, sizeof(password),
                 ssid_alt, sizeof(ssid_alt),
                 pass_alt, sizeof(pass_alt),
                 &nodeid, &maxnodes);

    if (ssid[0] == '\0') {
        shell_print("[cray-one] ERROR: SSID= not found in config.txt\r\n");
#ifdef PICOOS_LED_ENABLE
        { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
        return;
    }
    if (maxnodes < 1) {
        shell_print("[cray-one] ERROR: MAXNODES must be >= 1\r\n");
#ifdef PICOOS_LED_ENABLE
        { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
        return;
    }
    if (maxnodes > (int)MAXNODES_MAX) {
        shell_print("[cray-one] ERROR: MAXNODES=%d exceeds built-in limit %u\r\n",
                    maxnodes, MAXNODES_MAX);
#ifdef PICOOS_LED_ENABLE
        { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
        return;
    }
    if (nodeid < 0 || nodeid >= maxnodes) {
        shell_print("[cray-one] ERROR: NODEID must be 0 .. MAXNODES-1\r\n");
#ifdef PICOOS_LED_ENABLE
        { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
        return;
    }

    g_nodeid   = nodeid;
    g_maxnodes = maxnodes;

    shell_print("[cray-one] SSID      : %s\r\n", ssid);
    shell_print("[cray-one] Password  : %s\r\n",
                password[0] ? "(set)" : "(none — open network)");
    if (ssid_alt[0]) {
        shell_print("[cray-one] SSID alt  : %s\r\n", ssid_alt);
        shell_print("[cray-one] Pass alt  : %s\r\n",
                    pass_alt[0] ? "(set)" : "(none — open network)");
    }
    shell_print("[cray-one] Node      : %d / %d\r\n", nodeid, maxnodes);

    /* ---- 2. Connect to WiFi first — display setup follows on success ---- */
    shell_print("[cray-one] Connecting to WiFi \"%s\"...\r\n", ssid);
    int rc = wifi_connect(ssid, password);
    if (rc != 0 && ssid_alt[0] != '\0') {
        shell_print("[cray-one] Primary failed (err %d) — trying \"%s\"...\r\n",
                    rc, ssid_alt);
        rc = wifi_connect(ssid_alt, pass_alt);
    }
    if (rc != 0) {
        shell_print("[cray-one] Connection failed (err %d)\r\n", rc);
#ifdef PICOOS_LED_ENABLE
        { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
        return;
    }
    shell_print("[cray-one] Connected — IP: %s\r\n", wifi_get_ip_str());

    /* ---- 3. Seed RNG from flash unique ID (all boards) ---- */
#ifdef __arm__
    {
        /* Every Pico board has an 8-byte unique ID burned into its QSPI
         * flash chip at manufacture.  Fold all 8 bytes into a 32-bit seed
         * via XOR + rotation so no byte is lost.  Works on Pico, Pico W,
         * Pico 2, and Pico 2W — no WiFi required. */
        pico_unique_board_id_t uid;
        pico_get_unique_board_id(&uid);
        uint32_t seed = 0;
        for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
            seed = (seed << 5) | (seed >> 27);   /* rotate left 5 */
            seed ^= uid.id[i];
        }
        shell_print("[cray-one] Flash UID : %02X%02X%02X%02X%02X%02X%02X%02X\r\n",
                    uid.id[0], uid.id[1], uid.id[2], uid.id[3],
                    uid.id[4], uid.id[5], uid.id[6], uid.id[7]);

        /* On Pico W also XOR in MAC bytes for extra entropy */
        uint8_t mac[6] = {0};
        cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
        uint32_t mac_seed = ((uint32_t)mac[3] << 16)
                          | ((uint32_t)mac[4] <<  8)
                          |  (uint32_t)mac[5];
        mac_seed ^= ((uint32_t)mac[0] << 16)
                  | ((uint32_t)mac[1] <<  8)
                  |  (uint32_t)mac[2];
        seed ^= mac_seed;
        shell_print("[cray-one] MAC       : %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        rng_seed(seed ? seed : 1u);
    }
#else
    rng_seed((uint32_t)(nodeid + 1));   /* host build fallback */
#endif
    for (int i = 0; i < g_maxnodes && i < (int)MAXNODES_MAX; i++)
        g_my_colors[i] = color_palette[rng_next() % PALETTE_SIZE];
    shell_print("[cray-one] Color[0]  : 0x%02X\r\n", (unsigned)g_my_colors[0]);

    /* ---- 4. Initialize display and draw the initial grid ---- */
    draw_initial_grid();
    shell_print("[cray-one] Grid ready (%u x %u, %u blocks)\r\n",
                GRID_COLS, GRID_ROWS, GRID_TOTAL);

#ifdef __arm__
    /* ---- 5. Set up multicast UDP ---- */
    ip4_addr_t mcast_group;
    IP4_ADDR(&mcast_group, 239, 255, 0, 1);

    cyw43_arch_lwip_begin();

    /* Release any PCB left behind by a previous run that was killed before
     * reaching the normal cleanup path. */
    if (g_pcb != NULL) {
        if (g_mcast_active) {
            const ip4_addr_t *prev =
                netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
            igmp_leavegroup(prev, &g_mcast_group_addr);
            g_mcast_active = false;
        }
        udp_remove(g_pcb);
        g_pcb = NULL;
    }

    struct udp_pcb *pcb = udp_new();
    if (pcb == NULL) {
        cyw43_arch_lwip_end();
        shell_print("[cray-one] ERROR: udp_new failed\r\n");
#ifdef PICOOS_LED_ENABLE
        { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
        return;
    }

    if (udp_bind(pcb, IP_ADDR_ANY, MCAST_PORT) != ERR_OK) {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        shell_print("[cray-one] ERROR: udp_bind failed\r\n");
#ifdef PICOOS_LED_ENABLE
        { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
        return;
    }

    const ip4_addr_t *my_addr =
        netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    if (igmp_joingroup(my_addr, &mcast_group) != ERR_OK) {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        shell_print("[cray-one] ERROR: igmp_joingroup failed\r\n");
#ifdef PICOOS_LED_ENABLE
        { uint32_t c = 0xFF0000u; dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &c); }
#endif
        return;
    }
    udp_recv(pcb, mcast_recv_cb, NULL);
    g_pcb = pcb;
    g_mcast_group_addr = mcast_group;
    g_mcast_active = true;

    cyw43_arch_lwip_end();

    shell_print("[cray-one] Joined 239.255.0.1 port %u — "
                "announcing every %u ms\r\n",
                MCAST_PORT, ANNOUNCE_INTERVAL_MS);

    /* ---- 6. Spawn TX and display threads; wait for both to finish ---- */
    mqueue_init(&g_rx_mq, sizeof(rx_msg_t));
    event_flags_init(&g_cray_done);
    g_my_colors_dirty = false;
    g_cray_running    = true;

    uint32_t pid = (uint32_t)sys_getpid();
    syscall_dispatch(SYS_THREAD_CREATE, pid,
                     (uint32_t)(uintptr_t)cray_tx_thread,   0u, 0u);
    syscall_dispatch(SYS_THREAD_CREATE, pid,
                     (uint32_t)(uintptr_t)cray_disp_thread, 0u, 0u);

    /* Block until TX thread sets 0x1 and display thread sets 0x2. */
    event_flags_wait(&g_cray_done, 0x3u, true);

    /* ---- 7. Cleanup ---- */
    cyw43_arch_lwip_begin();
    my_addr = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    igmp_leavegroup(my_addr, &mcast_group);
    udp_remove(pcb);
    g_pcb = NULL;
    g_mcast_active = false;
    cyw43_arch_lwip_end();

    dev_close(DEV_DISPLAY);

#ifdef PICOOS_LED_ENABLE
    dev_ioctl(DEV_LED, IOCTL_LED_OFF, NULL);
    dev_close(DEV_LED);
#endif

    shell_print("[cray-one] stopped\r\n");
#endif /* __arm__ */
}

#endif /* PICOOS_WIFI_ENABLE && PICOOS_DISPLAY_ENABLE */
