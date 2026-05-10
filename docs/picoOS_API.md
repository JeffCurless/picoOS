# picoOS API Reference

A developer reference for writing built-in applications that run on picoOS.

---

## 1. Overview

picoOS is a dual-core preemptive operating system for the Raspberry Pi Pico family
(RP2040 and RP2350). It provides threads, processes, synchronization primitives, a
virtual filesystem, device drivers, an interactive shell, optional WiFi support, and
optional Bluetooth scanning support.

**Key constraints:**
- No MPU — there is no hardware memory isolation between processes.
- No SVC instruction — syscalls are direct C function calls (`syscall_dispatch`).
- Dual-core split: Core 0 runs the shell, USB console, and scheduler; Core 1 registers
  as a multicore lockout victim (required for safe flash writes) and is otherwise
  available for user worker threads.
- On RP2350 (Cortex-M33) the hardware FPU is present. picoOS saves/restores
  `EXC_RETURN` per-thread in `tcb_t.exc_return` so the correct exception frame size
  (basic 8-word or extended 26-word FP frame) is used on every context switch.

---

## 2. Application Model

Built-in apps are registered in `app_table[]`.  The type definition and extern declarations
live in `src/apps/app_table.h` (the stable ABI header).  In a standalone picoOS build,
`src/apps/demo.c` defines the table with the built-in demo apps.  When picoOS is used as a
submodule, the parent project provides its own `app_table[]` definition.
The shell `run <name>` command searches this table and spawns the matching entry as a new
process.

### `app_entry_t`

```c
typedef struct {
    const char *name;          /* Name used with the 'run' shell command */
    void      (*entry)(void *); /* Thread entry function                  */
    uint8_t    priority;       /* Scheduling priority: 0 = highest        */
} app_entry_t;
```

### Adding an app

**Step 1** — Write a thread entry function:

```c
void my_app(void *arg) {
    (void)arg;
    // app logic here
    sys_exit(0);
}
```

**Step 2** — Add an entry to `app_table[]`.  In a standalone build, edit `src/apps/demo.c`:

```c
const app_entry_t app_table[] = {
    { "producer", demo_producer, 4u },
    { "consumer", demo_consumer, 4u },
    { "sensor",   demo_sensor,   5u },
    { "myapp",    my_app,        4u },  /* <-- add your entry */
};
```

**Step 3** (optional) — Register a shell command from your app's init path before
`sched_start()`. See [Section 8: Shell Integration](#8-shell-integration).

---

## 3. Process and Thread Management

### 3.1 Syscall Wrappers (recommended for apps)

Include: `src/kernel/syscall.h`

| Function | Signature | Description |
|----------|-----------|-------------|
| `sys_yield` | `void sys_yield(void)` | Voluntarily surrender the CPU to the scheduler |
| `sys_sleep` | `void sys_sleep(uint32_t ms)` | Sleep for at least `ms` milliseconds |
| `sys_exit` | `void sys_exit(int code)` | Exit the current thread |
| `sys_getpid` | `int sys_getpid(void)` | Return the current process ID |
| `sys_gettid` | `int sys_gettid(void)` | Return the current thread ID |

### 3.2 Spawning Processes and Threads

For apps running inside the OS (not from shell input), use the task API directly:

Include: `src/kernel/task.h`

```c
// Allocate a new PCB
pcb_t *proc = task_create_process("myapp", pid);

// Create a thread inside that process
tcb_t *t = task_create_thread(proc, "worker", my_entry, arg, priority, stack_size);
```

**`task_create_process`**

```c
pcb_t *task_create_process(const char *name, uint32_t pid);
```

Allocates a PCB from the static pool. Returns `NULL` if the pool is full
(`MAX_PROCESSES = 8`).

**`task_create_thread`**

```c
tcb_t *task_create_thread(pcb_t      *proc,
                           const char *name,
                           void      (*entry)(void *),
                           void       *arg,
                           uint8_t     priority,
                           uint32_t    stack_size);
```

Allocates a TCB and a stack from the kernel heap. Returns `NULL` if the thread pool
(`MAX_THREADS = 16`) or heap is exhausted. The new thread enters `THREAD_READY` and the
scheduler will pick it up.

To spawn via the syscall interface (e.g., from inside a running thread):

```c
// Spawn a new process
syscall_dispatch(SYS_SPAWN, (uint32_t)name, (uint32_t)entry, (uint32_t)arg, 0);

// Add a thread to an existing process
syscall_dispatch(SYS_THREAD_CREATE, pid, (uint32_t)entry, (uint32_t)arg, 0);
```

### 3.3 Finding Threads and Processes

```c
tcb_t *task_find_thread(uint32_t tid);
pcb_t *task_find_process(uint32_t pid);

// Returns the kernel process (PID 1); useful for spawning background service threads
pcb_t *task_get_kernel_proc(void);
```

Returns `NULL` if not found.  `task_get_kernel_proc()` is safe to call after
`task_create_process("kernel", 1u)` runs in `main.c`; kernel modules (e.g. the WiFi
poll thread) use it to create threads without needing a PCB pointer passed in.

### 3.4 Priority and Stack Sizes

| Constant | Value | Use |
|----------|-------|-----|
| `DEFAULT_STACK_SIZE` | 2048 bytes | General-purpose threads |
| `DEEP_STACK_SIZE` | 3072 bytes | Threads with deep call chains |
| `IDLE_STACK_SIZE` | 512 bytes | Idle thread only |

Priority `0` is highest; `7` is lowest. Default for apps is `4`.

### 3.5 Thread States

```
THREAD_NEW → THREAD_READY → THREAD_RUNNING → THREAD_BLOCKED
                                           → THREAD_SLEEPING
                                           → THREAD_ZOMBIE
```

### 3.6 Killing Threads and Processes

```c
// Kill a single thread by TID (frees its stack and TCB)
syscall_dispatch(SYS_KILL, tid, 0, 0, 0);

// Kill all threads in a process and free the PCB
task_kill_process(pcb_t *proc);
```

---

## 4. Synchronization Primitives

Include: `src/kernel/sync.h`

### 4.1 Mutex (non-recursive, FIFO)

```c
kmutex_t m;
kmutex_init(&m);

kmutex_lock(&m);   // blocks if already held
// critical section
kmutex_unlock(&m);
```

### 4.2 Semaphore (counting)

```c
ksemaphore_t s;
ksemaphore_init(&s, initial_count);

ksemaphore_wait(&s);    // P() — decrement; blocks if count reaches 0
ksemaphore_signal(&s);  // V() — increment; wakes a blocked waiter if any
```

### 4.3 Event Flags (32-bit bitmask)

```c
event_flags_t e;
event_flags_init(&e);

event_flags_set(&e, mask);               // set bits
event_flags_clear(&e, mask);             // clear bits

// Block until condition is met; returns flags value at wake time
uint32_t bits = event_flags_wait(&e, mask, wait_for_all);
//   wait_for_all = true  : ALL bits in mask must be set
//   wait_for_all = false : ANY bit in mask is sufficient
```

### 4.4 Message Queue (ring buffer)

Fixed depth (`MQ_MAX_MSG = 16`) and fixed message width (`MQ_MSG_SIZE = 64` bytes).
`msg_size` passed to `mqueue_init` must be ≤ `MQ_MSG_SIZE`.

```c
mqueue_t q;
mqueue_init(&q, sizeof(my_msg_t));  // msg_size ≤ 64

mqueue_send(&q, &msg);   // blocks if queue is full
mqueue_recv(&q, &buf);   // blocks if queue is empty
```

---

## 5. I/O — Virtual File System (VFS)

Include: `src/kernel/vfs.h`

```c
int fd = vfs_open(path, mode);
vfs_read(fd, buf, n);
vfs_write(fd, buf, n);
vfs_close(fd);
```

### Open Mode Flags (OR-able)

| Flag | Value | Meaning |
|------|-------|---------|
| `VFS_O_RDONLY` | 0x01 | Open for reading |
| `VFS_O_WRONLY` | 0x02 | Open for writing |
| `VFS_O_RDWR` | 0x03 | Open for reading and writing |
| `VFS_O_CREAT` | 0x04 | Create the file if it does not exist |
| `VFS_O_TRUNC` | 0x08 | Truncate to zero length on open |

`vfs_open` returns a non-negative file descriptor on success, or `-1` on failure.
Maximum simultaneously open files: `VFS_MAX_OPEN = 16`.

### Device Paths

| Path | Device | Notes |
|------|--------|-------|
| `/dev/console` | USB CDC serial | Read/write text |
| `/dev/timer` | System timer | ioctl only |
| `/dev/flash` | Raw flash | ioctl only |
| `/dev/gpio` | GPIO pins | ioctl only |
| `/dev/display` | ST7789 framebuffer | Requires `PICOOS_DISPLAY_ENABLE` |
| `/dev/led` | RGB LED | Requires `PICOOS_LED_ENABLE` |

All other paths are forwarded to the filesystem layer.

---

## 6. Devices

Include: `src/kernel/dev.h`

Devices can be accessed directly via `dev_ioctl()` without going through VFS:

```c
int dev_ioctl(dev_id_t id, uint32_t cmd, void *arg);
```

### 6.1 Timer

```c
uint32_t ticks;
dev_ioctl(DEV_TIMER, IOCTL_TIMER_GET_TICK, &ticks);  // scheduler tick count

uint64_t us;
dev_ioctl(DEV_TIMER, IOCTL_TIMER_GET_US, &us);       // absolute time in µs
```

| Command | Arg type | Description |
|---------|----------|-------------|
| `IOCTL_TIMER_GET_TICK` (0x0100) | `uint32_t *` | Fills scheduler tick count |
| `IOCTL_TIMER_GET_US` (0x0101) | `uint64_t *` | Fills absolute time in microseconds |

### 6.2 GPIO

The `arg` for GPIO ioctls encodes pin number in bits [15:0] and the value/direction in
bit 16:

```c
// Set pin 25 as output
dev_ioctl(DEV_GPIO, IOCTL_GPIO_SET_DIR, (void *)(uintptr_t)(25u | (1u << 16)));

// Drive pin 25 high
dev_ioctl(DEV_GPIO, IOCTL_GPIO_SET_VAL, (void *)(uintptr_t)(25u | (1u << 16)));

// Drive pin 25 low
dev_ioctl(DEV_GPIO, IOCTL_GPIO_SET_VAL, (void *)(uintptr_t)(25u | (0u << 16)));

// Read pin value (arg = uint32_t * receiving 0 or 1)
uint32_t val;
dev_ioctl(DEV_GPIO, IOCTL_GPIO_GET_VAL, &val);
```

| Command | Arg encoding | Description |
|---------|-------------|-------------|
| `IOCTL_GPIO_SET_DIR` (0x0200) | `pin \| (dir << 16)` — dir: 1=output, 0=input | Set GPIO direction |
| `IOCTL_GPIO_SET_VAL` (0x0201) | `pin \| (val << 16)` — val: 1=high, 0=low | Set GPIO output level |
| `IOCTL_GPIO_GET_VAL` (0x0202) | `uint32_t *` | Read GPIO pin level |

### 6.3 Display (requires `PICOOS_DISPLAY_ENABLE`)

Include: `src/drivers/display.h` for constants and arg structs.

Panel dimensions depend on the compile-time flag:
- Default (`PICOOS_DISPLAY_PACK2` not set): ST7789 **240×135**, ~32 KB framebuffer
- `PICOOS_DISPLAY_PACK2=ON`: ST7789V **320×240**, ~75 KB framebuffer

Both use RGB332 framebuffer (1 byte/pixel), expanded to RGB565 on SPI flush.

```c
// Clear the framebuffer
dev_ioctl(DEV_DISPLAY, IOCTL_DISP_CLEAR, NULL);

// Draw text
disp_text_arg_t t = {
    .x = 10, .y = 10,
    .color = COLOR_WHITE, .bg = COLOR_BLACK,
    .scale = 1,
    .str = "Hello picoOS"
};
dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_TEXT, &t);

// Flush dirty rows to the panel (only changed rows are sent)
dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);

// Read buttons
uint8_t btns;
dev_ioctl(DEV_DISPLAY, IOCTL_DISP_GET_BTNS, &btns);
if (btns & DISP_BTN_A) { /* A pressed */ }
if (btns & DISP_BTN_B) { /* B pressed */ }
if (btns & DISP_BTN_X) { /* X pressed */ }
if (btns & DISP_BTN_Y) { /* Y pressed */ }
```

**Color:** `RGB332(r, g, b)` macro packs 8-bit r, g, b into one byte (top bits only).

| Predefined color | Value |
|-----------------|-------|
| `COLOR_BLACK` | `RGB332(0, 0, 0)` |
| `COLOR_WHITE` | `RGB332(255, 255, 255)` |
| `COLOR_RED` | `RGB332(255, 0, 0)` |
| `COLOR_GREEN` | `RGB332(0, 255, 0)` |
| `COLOR_BLUE` | `RGB332(0, 0, 255)` |
| `COLOR_YELLOW` | `RGB332(255, 255, 0)` |
| `COLOR_CYAN` | `RGB332(0, 255, 255)` |
| `COLOR_MAGENTA` | `RGB332(255, 0, 255)` |

**Display ioctl table:**

| Command | Code | Arg type | Description |
|---------|------|----------|-------------|
| `IOCTL_DISP_CLEAR` | 0x0300 | `NULL` | Fill framebuffer with background color |
| `IOCTL_DISP_FLUSH` | 0x0301 | `NULL` | Push dirty rows to panel via SPI |
| `IOCTL_DISP_SET_BG` | 0x0302 | `uint8_t *` | Set background color (RGB332) |
| `IOCTL_DISP_DRAW_PIXEL` | 0x0303 | `disp_pixel_arg_t *` | Draw a single pixel |
| `IOCTL_DISP_DRAW_LINE` | 0x0304 | `disp_line_arg_t *` | Draw a line |
| `IOCTL_DISP_DRAW_RECT` | 0x0305 | `disp_rect_arg_t *` | Draw a rectangle |
| `IOCTL_DISP_DRAW_TEXT` | 0x0306 | `disp_text_arg_t *` | Draw a text string |
| `IOCTL_DISP_SET_BL` | 0x0307 | `uint8_t *` | Set backlight brightness (0–255) |
| `IOCTL_DISP_GET_BTNS` | 0x0308 | `uint8_t *` | Read button bitmask |
| `IOCTL_DISP_GET_DIMS` | 0x0309 | `disp_dims_arg_t *` | Read panel dimensions |

**Button bitmask bits:**

| Bit | Constant | GPIO |
|-----|----------|------|
| 0 | `DISP_BTN_A` | 12 |
| 1 | `DISP_BTN_B` | 13 |
| 2 | `DISP_BTN_X` | 14 |
| 3 | `DISP_BTN_Y` | 15 |

**Arg structs** (from `src/kernel/dev.h`):

```c
typedef struct { uint16_t x, y; uint8_t color, _pad;                        } disp_pixel_arg_t;
typedef struct { uint16_t x0,y0,x1,y1; uint8_t color, _pad;                } disp_line_arg_t;
typedef struct { uint16_t x,y,w,h; uint8_t color,filled,_pad1,_pad2;       } disp_rect_arg_t;
typedef struct { uint16_t x,y; uint8_t color,bg,scale,_pad; const char *str; } disp_text_arg_t;
typedef struct { uint16_t width, height;                                     } disp_dims_arg_t;
```

### 6.4 LED (requires `PICOOS_LED_ENABLE`)

```c
// Set LED color (packed 0x00RRGGBB)
uint32_t color = 0x00FF0000u;  // red
dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &color);

// Turn off
dev_ioctl(DEV_LED, IOCTL_LED_OFF, NULL);
```

| Command | Code | Arg type | Description |
|---------|------|----------|-------------|
| `IOCTL_LED_SET_RGB` | 0x0400 | `uint32_t *` packed as `0x00RRGGBB` | Set LED color |
| `IOCTL_LED_OFF` | 0x0401 | ignored | Turn LED off |

### 6.5 WiFi (requires `PICOOS_WIFI_ENABLE` — pico_w / pico2_w builds only)

Include: `src/kernel/wifi.h`

`wifi_init()` is called automatically by `main.c` when `PICOOS_WIFI_ENABLE` is defined.
It initialises the CYW43 radio in STA mode, spawns the `wifi-poll` thread (priority 6),
and registers the `wifi` shell command.  Applications can query and control WiFi state
directly via the API below.

```c
// Query current state
wifi_state_t s = wifi_get_state();
// Returns: WIFI_STATE_DOWN, WIFI_STATE_SCANNING, WIFI_STATE_CONNECTING,
//          WIFI_STATE_UP, or WIFI_STATE_ERROR

// Start a background scan (results available once state returns to DOWN)
wifi_scan();

// Connect to an AP (blocks up to 10 seconds)
int rc = wifi_connect("MyNetwork", "password");  // rc == 0 on success

// Connect to an open AP
int rc = wifi_connect("OpenNetwork", "");

// Disconnect
wifi_disconnect();
```

**`wifi_state_t`:**

| Value | Meaning |
|-------|---------|
| `WIFI_STATE_DOWN` | Radio initialised, no AP association |
| `WIFI_STATE_SCANNING` | Background scan in progress |
| `WIFI_STATE_CONNECTING` | Association attempt in progress |
| `WIFI_STATE_UP` | Associated and link is up |
| `WIFI_STATE_ERROR` | Last operation failed or link dropped unexpectedly |

**Shell command:**

```
wifi status                      — print current state
wifi scan                        — scan and list visible APs (SSID, RSSI, channel, auth)
wifi connect <ssid> [password]   — connect to an AP
wifi disconnect                  — drop the current connection
```

**`wifi_scan_result_t`** (from `src/kernel/wifi.h`):

```c
typedef struct {
    char    ssid[33];
    int16_t rssi;
    uint8_t channel;
    uint8_t auth_mode;   // 0 = open, CYW43_AUTH_WPA2_AES_PSK = WPA2
} wifi_scan_result_t;
```

Up to `WIFI_MAX_SCAN_RESULTS` (16) results are stored internally.  The poll thread
manages `cyw43_arch_poll()` so applications do not need to call it directly.

### 6.6 Bluetooth (requires `PICOOS_BT_ENABLE` — pico_w / pico2_w builds only)

Include: `src/kernel/bluetooth.h`

`bt_init()` is called automatically by `main.c` after `wifi_init()` when `PICOOS_BT_ENABLE`
is defined.  It hooks BTstack into the CYW43 async context already created by `wifi_init()`
and powers on the BT radio asynchronously.  No separate poll thread is created — the
existing `wifi-poll` thread drives both stacks via `cyw43_arch_poll()`.

```c
// Query current state
bt_state_t s = bt_get_state();
// Returns: BT_STATE_OFF, BT_STATE_IDLE, BT_STATE_SCANNING, or BT_STATE_ERROR

// Start a simultaneous Classic inquiry (~6.4 s) + BLE passive scan
bt_scan();

// Poll for completion (or sleep-loop as shown in cmd_bt)
while (!bt_scan_is_done()) { sys_sleep(100); }

// Retrieve results
const bt_scan_result_t *results;
int count;
bt_get_scan_results(&results, &count);
for (int i = 0; i < count; i++) {
    const bt_scan_result_t *r = &results[i];
    // r->addr[6]          — device address (big-endian byte order)
    // r->name             — device name (empty string if unavailable)
    // r->rssi             — signal strength in dBm
    // r->type             — BT_DEVTYPE_CLASSIC or BT_DEVTYPE_BLE
    // r->dev_class        — bt_devclass_t (see table below)
    // r->class_of_device  — raw 24-bit CoD (Classic only; 0 for BLE)
}
```

**`bt_state_t`:**

| Value | Meaning |
|-------|---------|
| `BT_STATE_OFF` | BT radio not yet powered on |
| `BT_STATE_IDLE` | Radio up, no active scan |
| `BT_STATE_SCANNING` | Classic inquiry + BLE scan in progress |
| `BT_STATE_ERROR` | Initialization failed |

**`bt_devclass_t`** — derived from the Classic Bluetooth Class of Device major class field:

| Value | String (`bt_devclass_str()`) | CoD major class |
|-------|------------------------------|----------------|
| `BT_CLASS_UNKNOWN` | `"unknown"` | BLE devices; or CoD = 0 |
| `BT_CLASS_COMPUTER` | `"computer"` | 0x01 |
| `BT_CLASS_PHONE` | `"phone"` | 0x02 |
| `BT_CLASS_NETWORK` | `"network"` | 0x03 |
| `BT_CLASS_AUDIO` | `"audio"` | 0x04 |
| `BT_CLASS_PERIPHERAL` | `"peripheral"` | 0x05 (mouse, keyboard, etc.) |
| `BT_CLASS_IMAGING` | `"imaging"` | 0x06 (printer, scanner, camera) |
| `BT_CLASS_WEARABLE` | `"wearable"` | 0x07 |
| `BT_CLASS_TOY` | `"toy"` | 0x08 |
| `BT_CLASS_HEALTH` | `"health"` | 0x09 |
| `BT_CLASS_OTHER` | `"other"` | All other major classes |

**Shell command:**

```
bt status     — print current state (off / idle / scanning / error)
bt scan       — run a combined Classic + BLE scan (~7 s) and print a device table
```

**`bt_scan_result_t`** (from `src/kernel/bluetooth.h`):

```c
typedef struct {
    uint8_t       addr[6];           /* device address — bytes [5:0] = MSB:LSB */
    char          name[32];          /* device name; empty string if not available */
    int8_t        rssi;              /* received signal strength in dBm */
    bt_devtype_t  type;              /* BT_DEVTYPE_CLASSIC or BT_DEVTYPE_BLE */
    bt_devclass_t dev_class;         /* major device class */
    uint32_t      class_of_device;   /* raw 24-bit CoD (0 for BLE) */
} bt_scan_result_t;
```

Up to `BT_MAX_SCAN_RESULTS` (20) devices are stored.  Duplicates are suppressed by
address.  Classic scan duration is fixed at 5 × 1.28 s ≈ 6.4 s; the BLE scan runs
concurrently and stops when the Classic inquiry completes.

---

## 7. Filesystem

Include: `src/kernel/fs.h` (or use VFS paths — preferred for read/write).

The filesystem is flash-backed (1 MB offset, 512 KB region). Writes are buffered in RAM
and committed to flash on `vfs_close()`. **Flash writes briefly pause all execution.**

### Reading and Writing Files via VFS (preferred)

```c
// Write a file
int fd = vfs_open("myfile.txt", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
vfs_write(fd, (const uint8_t *)"hello", 5);
vfs_close(fd);  // commits to flash

// Read it back
uint8_t buf[64];
fd = vfs_open("myfile.txt", VFS_O_RDONLY);
int n = vfs_read(fd, buf, sizeof(buf));
vfs_close(fd);
```

### Listing and Deleting Files

```c
// List all files (callback receives one fs_entry_t * per file)
static int list_cb(const fs_entry_t *e) {
    shell_print("%s  %u bytes\r\n", e->name, e->size);
    return 0;  // non-zero stops iteration early
}
fs_list(list_cb);

// Delete a file
fs_delete("myfile.txt");  // returns 0 on success, -1 if not found
```

### `fs_entry_t`

```c
typedef struct {
    bool     used;
    char     name[FS_NAME_MAX];   /* 16 bytes: max 15-char name + NUL */
    uint32_t size;                /* current file size in bytes        */
    uint32_t start_block;
    uint32_t block_count;
} fs_entry_t;
```

### Filesystem Limits

| Constant | Value | Meaning |
|----------|-------|---------|
| `FS_MAX_FILES` | 64 (RP2040) / 127 (RP2350) | Maximum number of files — set per-board by CMake |
| `FS_MAX_FILE_DATA` | 4096 bytes | Maximum size per file |
| `FS_NAME_MAX` | 16 | Filename buffer (15 chars + NUL) |
| `FS_BLOCK_SIZE` | 4096 bytes | Flash erase sector size |

---

## 8. Shell Integration

Include: `src/shell/shell.h`

### Registering a Command

Call `shell_register_cmd()` before `sched_start()` (e.g., from your module's init
function or from `main.c`).

```c
static int cmd_myapp(int argc, char **argv) {
    if (argc < 2) {
        shell_print("usage: myapp <value>\r\n");
        return -1;
    }
    shell_print("myapp got: %s\r\n", argv[1]);
    return 0;
}

static const shell_cmd_t my_cmd = {
    .name    = "myapp",
    .help    = "myapp <value> — example command",
    .handler = cmd_myapp,
};

// call during init, before sched_start():
shell_register_cmd(&my_cmd);
```

`shell_register_cmd` returns `0` on success, `-1` if the command table is full
(`SHELL_MAX_CMDS = 32`).

### Output Functions

```c
shell_print("value = %u\r\n", val);   // printf-style formatted output
shell_println("done");                 // print string + CRLF
```

---

## 9. Memory

Include: `src/kernel/mem.h`

```c
void *p = kmalloc(size);  // 8-byte aligned; returns NULL on out-of-memory
kfree(p);                  // adjacent free blocks are coalesced; safe with NULL

uint32_t used, free_bytes, largest;
kmem_stats(&used, &free_bytes, &largest);
// used     — bytes currently allocated (including block headers)
// free_bytes — total free bytes (including headers of free blocks)
// largest  — largest single contiguous free allocation available
```

Heap size: `HEAP_SIZE = 64 KB` (shared between dynamic thread stacks and kernel objects).

At peak (16 threads × `DEFAULT_STACK_SIZE`), thread stacks alone consume 32 KB of the
heap, leaving ~32 KB for other allocations.

---

## 10. Quick Reference

### Syscall Numbers and Wrappers

| Number | Constant | Wrapper | Description |
|--------|----------|---------|-------------|
| 0 | `SYS_SPAWN` | — | Spawn a new process |
| 1 | `SYS_THREAD_CREATE` | — | Add a thread to a process |
| 2 | `SYS_EXIT` | `sys_exit(code)` | Exit current thread |
| 3 | `SYS_YIELD` | `sys_yield()` | Yield CPU |
| 4 | `SYS_SLEEP` | `sys_sleep(ms)` | Sleep N milliseconds |
| 5 | `SYS_OPEN` | — | Open a VFS path |
| 6 | `SYS_READ` | — | Read from fd |
| 7 | `SYS_WRITE` | — | Write to fd |
| 8 | `SYS_CLOSE` | — | Close fd |
| 9 | `SYS_MQ_SEND` | — | Send to message queue |
| 10 | `SYS_MQ_RECV` | — | Receive from message queue |
| 11 | `SYS_MUTEX_LOCK` | — | Lock a mutex |
| 12 | `SYS_MUTEX_UNLOCK` | — | Unlock a mutex |
| 13 | `SYS_GETPID` | `sys_getpid()` | Get process ID |
| 14 | `SYS_GETTID` | `sys_gettid()` | Get thread ID |
| 15 | `SYS_PS` | — | Process/thread list |
| 16 | `SYS_KILL` | — | Kill thread by TID |

### Synchronization Primitives

| Type | Init | Operations |
|------|------|-----------|
| `kmutex_t` | `kmutex_init(&m)` | `kmutex_lock(&m)`, `kmutex_unlock(&m)` |
| `ksemaphore_t` | `ksemaphore_init(&s, n)` | `ksemaphore_wait(&s)`, `ksemaphore_signal(&s)` |
| `event_flags_t` | `event_flags_init(&e)` | `event_flags_set/clear/wait` |
| `mqueue_t` | `mqueue_init(&q, size)` | `mqueue_send(&q, &msg)`, `mqueue_recv(&q, &buf)` |

### VFS Mode Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `VFS_O_RDONLY` | 0x01 | Read-only |
| `VFS_O_WRONLY` | 0x02 | Write-only |
| `VFS_O_RDWR` | 0x03 | Read-write |
| `VFS_O_CREAT` | 0x04 | Create if absent |
| `VFS_O_TRUNC` | 0x08 | Truncate on open |

### All ioctl Commands

| Command | Code | Device | Arg type |
|---------|------|--------|----------|
| `IOCTL_TIMER_GET_TICK` | 0x0100 | `DEV_TIMER` | `uint32_t *` |
| `IOCTL_TIMER_GET_US` | 0x0101 | `DEV_TIMER` | `uint64_t *` |
| `IOCTL_GPIO_SET_DIR` | 0x0200 | `DEV_GPIO` | `pin \| (dir << 16)` |
| `IOCTL_GPIO_SET_VAL` | 0x0201 | `DEV_GPIO` | `pin \| (val << 16)` |
| `IOCTL_GPIO_GET_VAL` | 0x0202 | `DEV_GPIO` | `uint32_t *` |
| `IOCTL_DISP_CLEAR` | 0x0300 | `DEV_DISPLAY` | `NULL` |
| `IOCTL_DISP_FLUSH` | 0x0301 | `DEV_DISPLAY` | `NULL` |
| `IOCTL_DISP_SET_BG` | 0x0302 | `DEV_DISPLAY` | `uint8_t *` |
| `IOCTL_DISP_DRAW_PIXEL` | 0x0303 | `DEV_DISPLAY` | `disp_pixel_arg_t *` |
| `IOCTL_DISP_DRAW_LINE` | 0x0304 | `DEV_DISPLAY` | `disp_line_arg_t *` |
| `IOCTL_DISP_DRAW_RECT` | 0x0305 | `DEV_DISPLAY` | `disp_rect_arg_t *` |
| `IOCTL_DISP_DRAW_TEXT` | 0x0306 | `DEV_DISPLAY` | `disp_text_arg_t *` |
| `IOCTL_DISP_SET_BL` | 0x0307 | `DEV_DISPLAY` | `uint8_t *` |
| `IOCTL_DISP_GET_BTNS` | 0x0308 | `DEV_DISPLAY` | `uint8_t *` |
| `IOCTL_DISP_GET_DIMS` | 0x0309 | `DEV_DISPLAY` | `disp_dims_arg_t *` |
| `IOCTL_LED_SET_RGB` | 0x0400 | `DEV_LED` | `uint32_t *` (0x00RRGGBB) |
| `IOCTL_LED_OFF` | 0x0401 | `DEV_LED` | ignored |

### Stack Size Constants

| Constant | Value | Use |
|----------|-------|-----|
| `DEFAULT_STACK_SIZE` | 2048 | General-purpose threads |
| `DEEP_STACK_SIZE` | 3072 | Threads with deep call chains |
| `IDLE_STACK_SIZE` | 512 | Idle thread only |

### System Limits

| Constant | Value | Meaning |
|----------|-------|---------|
| `MAX_THREADS` | 16 | Maximum live threads |
| `MAX_PROCESSES` | 8 | Maximum processes |
| `MQ_MAX_MSG` | 16 | Messages per queue |
| `MQ_MSG_SIZE` | 64 | Max bytes per message |
| `VFS_MAX_OPEN` | 16 | Max simultaneous open VFS fds |
| `FS_MAX_FILES` | 64 / 127 | Max files — 64 on RP2040, 127 on RP2350 |
| `FS_MAX_FILE_DATA` | 4096 | Max bytes per file |
| `SHELL_MAX_CMDS` | 32 | Max registered shell commands |
| `HEAP_SIZE` | 65536 | Kernel heap in bytes (64 KB) |
| `WIFI_MAX_SCAN_RESULTS` | 16 | Max WiFi scan results stored |
| `BT_MAX_SCAN_RESULTS` | 20 | Max Bluetooth scan results stored |
| `DISP_WIDTH` | 240 or 320 | Display width — 240 (Display Pack) / 320 (Display Pack 2) |
| `DISP_HEIGHT` | 135 or 240 | Display height — 135 (Display Pack) / 240 (Display Pack 2) |
