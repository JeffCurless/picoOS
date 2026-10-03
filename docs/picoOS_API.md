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
- Full SMP scheduling — both RP2040/RP2350 cores run the preemptive scheduler
  concurrently. Each core has its own SysTick, PendSV, and `current_tcb` slot.
  Core 1 also registers as a multicore lockout victim so the filesystem's flash writes
  (`flash_safe_execute()`) can safely pause it during flash erase/program.
  Ready queues, the heap and the event-waiter pool each have a dedicated hardware
  spinlock; mutexes, semaphores, event flags and message queues share a small striped
  pool of hardware locks; VFS and filesystem operations are serialised by mutexes.
  See [locking.md](locking.md).
- On RP2350 (Cortex-M33) the hardware FPU is present. picoOS saves/restores
  `EXC_RETURN` per-thread in `tcb_t.exc_return` so the correct exception frame size
  (basic 8-word or extended 26-word FP frame) is used on every context switch.

---

## 2. Application Model

Built-in apps are registered in `app_table[]`.  The type definition and extern declarations
live in `src/apps/app_table.h` (the stable ABI header).  In a standalone picoOS build,
`src/apps/demo.c` defines the table with the built-in demo apps.  When picoOS is used as a
submodule, the parent project provides its own `app_table[]` definition.
The shell `run <name> [arg]` command searches this table and spawns the matching entry as a
new process.  If a second word is supplied it is copied to a heap buffer and passed to the
entry function as `void *arg`; the app owns that allocation and must call `kfree(arg)` when
done with it.

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

**Step 3** — Add the source file to `PICOOS_SOURCES` in `src/CMakeLists.txt` (inside
`if(PICOOS_INCLUDE_DEMO_APPS)`, or a board-feature block if the app needs WiFi or the
display), and declare the entry function in a header that `demo.c` includes.

**Step 4** (optional) — Launch it at boot with `AUTORUN=myapp` in `config.txt`, or bind
it to a Display Pack button. See [Launching Apps at Boot](#launching-apps-at-boot-configtxt).

A full walkthrough is in [application.md](application.md).

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
| `sys_getcore` | `int sys_getcore(void)` | Return the core (0 or 1) the caller is running on now |

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

### 3.4 Priority, Stack Sizes, and Thread Affinity

| Constant | Value | Use |
|----------|-------|-----|
| `DEFAULT_STACK_SIZE` | 2048 bytes | General-purpose threads |
| `DEEP_STACK_SIZE` | 3072 bytes | Threads with deep call chains |
| `IDLE_STACK_SIZE` | 512 bytes | Idle thread only |

Priority `0` is highest; `7` is lowest. Default for apps is `4`.

**Thread affinity** controls which core may schedule a thread.  Set it on the
TCB before or immediately after the thread starts:

```c
// Pin to Core 0 (USB, shell, filesystem-heavy work)
CURRENT_TCB->affinity = THREAD_AFFINITY_C0;

// Pin to Core 1 (compute workers, background tasks)
CURRENT_TCB->affinity = THREAD_AFFINITY_C1;

// Eligible on either core (default)
CURRENT_TCB->affinity = THREAD_AFFINITY_ANY;
```

`CURRENT_TCB` is a macro that expands to `current_tcb[get_core_num()]` — the
TCB pointer for whichever core evaluates it.

| Constant | Value | Meaning |
|----------|-------|---------|
| `THREAD_AFFINITY_ANY` | -1 | Scheduled on either core (default) |
| `THREAD_AFFINITY_C0` | 0 | Core 0 only |
| `THREAD_AFFINITY_C1` | 1 | Core 1 only |

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

### 3.7 Additional Task API

```c
// Count of live threads / processes (across all slots)
int task_thread_count(void);
int task_process_count(void);

// Raw slot access — used by 'ps' and 'threads' shell commands
// Returns the TCB/PCB at index idx, or NULL if the slot is empty
tcb_t *task_get_thread_slot(int idx);
pcb_t *task_get_process_slot(int idx);

// Release a TCB or PCB back to the static pool
void task_free_thread(tcb_t *t);
void task_free_process(pcb_t *p);
```

### 3.8 Boot Threads

At boot, these threads are created before `sched_start()`:

| TID | Priority | Core | Name | Purpose |
|-----|----------|------|------|---------|
| 1 | 7 | 0 | `idle` | Core 0 idle — runs when no other thread is eligible |
| 2 | 7 | 1 | `idle1` | Core 1 idle — pinned to Core 1 (`THREAD_AFFINITY_C1`) |
| 3 | 2 | any | `shell` | USB CDC interactive shell |

On pico_w / pico2_w builds `wifi_init()` runs first and creates `wifi-poll` (priority 6,
any core) as TID 1, so `idle`, `idle1` and `shell` become TIDs 2–4.

`MAX_THREADS = 16`, so **13 thread slots** are free at startup (12 on WiFi builds).
After the scheduler starts, the shell may add an AUTORUN app and, on display builds
with button bindings, a `btn-mon` thread (priority 3).

---

## 4. Synchronization Primitives

Include: `src/kernel/sync.h`

All blocking primitives (mutex, semaphore, event flags, message queue) are
SMP-safe: each contains an embedded `spinlock_t` that points at one of a small
pool of SIO hardware spinlocks (`prim_pool`, chosen from the object's address).
You can create as many of them as you like without using up hardware locks.
While the lock is held, interrupts are disabled on the local core only.
Waiters block in the scheduler; they do not spin. See [locking.md](locking.md).

### 4.0 Spinlock

Low-level building block used internally by all higher-level primitives.  Most
application code should use a mutex or semaphore instead.

```c
spinlock_t s;
spinlock_init(&s);        // claims a dedicated hardware spinlock (IDs 24-31)

// IRQ-aware pair (preferred — saves and restores interrupt state)
uint32_t saved = spinlock_irq_acquire(&s);
// ... critical section ...
spinlock_irq_release(&s, saved);

// Plain pair (use only when the caller already has IRQs disabled)
spinlock_acquire(&s);
spinlock_release(&s);
```

`spinlock_irq_acquire` disables IRQs and acquires the lock; the saved IRQ state
is returned and must be passed back to `spinlock_irq_release`.

**Avoid `spinlock_init()` in applications.**  Each call claims one of only 8
claimable hardware spinlocks, several of which the kernel already uses, and it
panics when none are left.  Use a `kmutex_t` instead — it costs no hardware lock.
The plain `spinlock_acquire`/`spinlock_release` pair has no callers in the kernel
and is only safe if no interrupt handler on the same core takes the same lock.

The
`PICOOS_LOCK_DEBUG` build adds a timeout: if the lock is held for more than
`PICOOS_LOCK_TIMEOUT_MS` (default 5 000 ms), `lock_deadlock_panic()` is called.

### 4.1 Mutex (non-recursive)

```c
kmutex_t m;
kmutex_init(&m);

kmutex_lock(&m);   // blocks if already held
// critical section
kmutex_unlock(&m);
```

Blocked threads are woken in FIFO order, but a woken thread re-checks the lock, so a
thread arriving at that moment can take it first.  Locking a mutex you already hold
blocks forever, and `kmutex_unlock` does not check the caller is the owner.

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

// Non-blocking variants — return false immediately rather than blocking
bool sent = mqueue_try_send(&q, &msg);   // false = queue was full
bool got  = mqueue_try_recv(&q, &buf);   // false = queue was empty
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
| `VFS_O_APPEND` | 0x10 | Start writing at the end of the file |

`vfs_open` returns a non-negative file descriptor on success, or `-1` on failure.
Maximum simultaneously open VFS descriptors: `VFS_MAX_OPEN = 16`, of which at most
`FS_MAX_OPEN_FDS = 8` can be filesystem files.  Only one file can be open for writing
at a time.

### Device Paths

| Path | Device | Notes |
|------|--------|-------|
| `/dev/console` | USB CDC serial | Read (non-blocking) / write text |
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

Every GPIO ioctl takes a **pointer to a `uint32_t` word**.  The pin number goes in
bits [7:0] and the value or direction in bit 16.  (Passing the encoded value itself,
cast to a pointer, makes the driver read from a bogus address.)

```c
uint32_t w;

// Set pin 25 as output
w = 25u | (1u << 16);
dev_ioctl(DEV_GPIO, IOCTL_GPIO_SET_DIR, &w);

// Drive pin 25 high, then low
w = 25u | (1u << 16);
dev_ioctl(DEV_GPIO, IOCTL_GPIO_SET_VAL, &w);
w = 25u;
dev_ioctl(DEV_GPIO, IOCTL_GPIO_SET_VAL, &w);

// Read pin 15: put the pin number in the word; it is replaced by 0 or 1
w = 15u;
dev_ioctl(DEV_GPIO, IOCTL_GPIO_GET_VAL, &w);
```

| Command | Arg (`uint32_t *` to) | Description |
|---------|-------------|-------------|
| `IOCTL_GPIO_SET_DIR` (0x0200) | `pin \| (dir << 16)` — dir: 1=output, 0=input | Set GPIO direction |
| `IOCTL_GPIO_SET_VAL` (0x0201) | `pin \| (val << 16)` — val: 1=high, 0=low | Set GPIO output level |
| `IOCTL_GPIO_GET_VAL` (0x0202) | `pin` in; 0 or 1 out | Read GPIO pin level |

The pins must already be set up as GPIO (the driver does not call `gpio_init`).
`vfs_read`/`vfs_write` on `/dev/gpio` are not implemented yet.

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

// Start a background scan.  Returns 0, WIFI_ERR_BUSY if a scan is already
// running (the running scan and its results are left alone), or a CYW43 error.
wifi_scan();
while (!wifi_scan_is_done()) { sys_sleep(50); }

// Copy the results out.  Safe while a scan is still running; returns the
// number copied (0..max) or WIFI_ERR_ARG.  Keep the buffer static — 16
// entries is 704 bytes, a third of a default thread stack.
static wifi_scan_result_t nets[WIFI_MAX_SCAN_RESULTS];
int n = wifi_copy_scan_results(nets, WIFI_MAX_SCAN_RESULTS);

// Connect to an AP (blocks up to 10 seconds)
int rc = wifi_connect("MyNetwork", "password");  // rc == 0 on success

// Connect to an open AP
int rc = wifi_connect("OpenNetwork", "");

// Disconnect
wifi_disconnect();

// STA MAC address
uint8_t mac[6];
wifi_get_mac(mac);                               // 0 on success
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
wifi watch [n]                   — continuous scan: print n windows (default 5), then stop
wifi connect <ssid> [password]   — connect to an AP
wifi disconnect                  — drop the current connection
```

**`wifi_scan_result_t`** (from `src/kernel/wifi.h`):

```c
typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    int16_t rssi;
    uint8_t channel;
    uint8_t auth_mode;   // 0 = open, CYW43_AUTH_WPA2_AES_PSK = WPA2
} wifi_scan_result_t;
```

Up to `WIFI_MAX_SCAN_RESULTS` (16) results are stored internally, one per BSSID: repeat reports of the same BSS during a scan refresh its RSSI instead of using another slot.

**Where the radio code runs.**  picoOS links `pico_cyw43_arch_lwip_threadsafe_background`,
so CYW43, lwIP and BTstack work runs in the SDK async context: a low-priority IRQ on
core 0, the core that called `wifi_init()`.  `cyw43_arch_poll()` does nothing in this
mode.  The `wifi-poll` thread only watches for the end of a scan and for link drops.
Scan results, multicast receive callbacks and BT events are all delivered from that IRQ.

**Scan results are copied out.**  The kernel buffer is filled from the IRQ while a scan
runs, so read it only with `wifi_copy_scan_results()`, which copies under the async
context lock.  `wifi_get_scan_results(&ptr, &count)` still exists but is deprecated: it
returns the live buffer with no lock, so entries can be torn mid-scan (see
`docs/imperfections.md` §10).

**Continuous scanning (list swap).**  For an app that monitors the air rather than
taking one snapshot, `wifi_scan_start()` keeps the radio scanning back to back and hands
the app one list per *window* — one scan pass, at most `WIFI_WINDOW_MAX_MS` (1000 ms).
Firmware dwell times are shortened while it runs (25 ms active, 60 ms passive per
channel), so a pass normally takes well under a second.  The list holds every network
heard in that window, one entry per BSSID with its latest RSSI; a network that has gone
quiet simply drops out of the next list.

The handover is a buffer swap, not a copy: the kernel keeps three lists (being filled,
ready, held by the app — see `src/kernel/scanbuf.h`).  The list `wifi_scan_wait()`
returns is the app's until its next `wifi_scan_wait()`, so it can be read and processed
with no lock while the radio fills another one.  If the app falls behind, the newest
window replaces the unread one and `dropped` counts it.

```c
// Pull model: your own worker thread does the processing.
static void scan_worker(void *arg)
{
    if (wifi_scan_start(NULL, NULL) != 0) return;      // WIFI_ERR_BUSY if in use
    const wifi_scan_list_t *l;
    while (wifi_scan_wait(&l) == 0) {                   // blocks until next window
        for (int i = 0; i < l->count; i++)
            process(&l->items[i]);                      // merge, graph, ...
    }
    // WIFI_ERR_STOPPED: someone called wifi_scan_stop()
}

// Callback model: the kernel creates a "wifi-listen" thread in your process,
// at your priority, and calls on_window() once per window from it.
static void on_window(const wifi_scan_list_t *l, void *ctx) { /* may block */ }
wifi_scan_start(on_window, NULL);
...
wifi_scan_stop();
```

```c
typedef struct {
    uint32_t           seq;        // window number, 1, 2, 3, ...
    uint32_t           dropped;    // total windows lost to a slow reader
    uint32_t           window_ms;  // how long this window lasted
    int                count;      // valid entries in items[]
    wifi_scan_result_t items[WIFI_MAX_SCAN_RESULTS];
} wifi_scan_list_t;
```

| Function | Returns |
|----------|---------|
| `wifi_scan_start(cb, ctx)` | `0`, `WIFI_ERR_BUSY` (scan or subscriber active), `WIFI_ERR_NOMEM` (no listener thread), or a CYW43 error |
| `wifi_scan_wait(&list)` | `0`, `WIFI_ERR_STOPPED`, or `WIFI_ERR_ARG` |
| `wifi_scan_stop()` | — (safe when not running) |
| `wifi_scan_running()` | `true` while continuous mode is on |

One subscriber at a time.  Continuous mode and one-shot `wifi_scan()` refuse each other
with `WIFI_ERR_BUSY`.  If the subscriber's process exits, the `wifi-poll` thread stops
the scan by itself.  Scanning while connected works but takes airtime from traffic.

**Multicast UDP** — a socket-style wrapper so applications never call lwIP or the
CYW43 driver directly.  The link must be up before `wifi_mcast_open()`.

```c
static void on_rx(const char *data, uint16_t len, const char *src_ip, void *ctx)
{
    // Runs in the CYW43 async context (IRQ on core 0): copy the data out and
    // return quickly.  Never block or sleep here.
    // data[len] == '\0'; payloads over WIFI_MCAST_MAX_PAYLOAD (128) are truncated.
}

int sock = wifi_mcast_open("239.255.0.1", 4210, on_rx, NULL);  // handle >= 0
if (sock >= 0) {
    wifi_mcast_send(sock, "hello", 5);                         // 0 on success
    wifi_mcast_close(sock);
}
```

| Function | Returns |
|----------|---------|
| `wifi_mcast_open(group, port, cb, ctx)` | Socket handle `>= 0`, or a `WIFI_ERR_*` code |
| `wifi_mcast_send(sock, data, len)` | `0`, or a `WIFI_ERR_*` code |
| `wifi_mcast_close(sock)` | — (invalid handles are ignored) |

| Error | Value | Meaning |
|-------|-------|---------|
| `WIFI_ERR_ARG` | -1 | Bad argument or handle, or link not up |
| `WIFI_ERR_NOSOCK` | -2 | All `WIFI_MCAST_MAX_SOCKETS` (2) in use |
| `WIFI_ERR_NOMEM` | -3 | lwIP out of memory |
| `WIFI_ERR_BIND` | -4 | Port already bound |
| `WIFI_ERR_JOIN` | -5 | IGMP join failed |
| `WIFI_ERR_SEND` | -6 | lwIP rejected the datagram |
| `WIFI_ERR_BUSY` | -7 | `wifi_scan()` / `wifi_scan_start()`: a scan is already running |
| `WIFI_ERR_STOPPED` | -8 | `wifi_scan_wait()`: continuous scanning is not running |

Sockets are not owned by a process.  An app that may be killed should keep its handle
in a `static int` initialised to `-1` and close it on its next start (see
`src/apps/cray_one.c`).

### 6.6 Bluetooth (requires `PICOOS_BT_ENABLE` — pico_w / pico2_w builds only)

Include: `src/kernel/bluetooth.h`

`bt_init()` is called automatically by `main.c` after `wifi_init()` when `PICOOS_BT_ENABLE`
is defined.  It hooks BTstack into the CYW43 async context already created by `wifi_init()`
and powers on the BT radio asynchronously.  No separate thread is created: BTstack
events are delivered from the same async-context IRQ as WiFi (see §6.5).

```c
// Query current state
bt_state_t s = bt_get_state();
// Returns: BT_STATE_OFF, BT_STATE_IDLE, BT_STATE_SCANNING, or BT_STATE_ERROR

// Start a simultaneous Classic inquiry (~6.4 s) + BLE passive scan.
// Returns -1 if the radio is off or a scan is already running.
bt_scan();

// Poll for completion (or sleep-loop as shown in cmd_bt)
while (!bt_scan_is_done()) { sys_sleep(100); }

// Copy the results out (safe while a scan is still running).  Returns the
// number copied (0..max) or -1 on a bad argument.  BLE results keep arriving
// after bt_scan_is_done(), and Classic names fill in as name requests return,
// so copy again for the latest view.
static bt_scan_result_t results[BT_MAX_SCAN_RESULTS];
int count = bt_copy_scan_results(results, BT_MAX_SCAN_RESULTS);
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
bt watch [n]  — continuous scan: print n 1-second windows (default 5), then stop
```

**`bt_scan_result_t`** (from `src/kernel/bluetooth.h`):

```c
typedef struct {
    uint8_t       addr[6];           /* device address — bytes [5:0] = MSB:LSB */
    char          name[32];          /* device name; empty string if not available */
    int8_t        rssi;              /* dBm, BT_RSSI_UNKNOWN (-127) if absent */
    bt_devtype_t  type;              /* BT_DEVTYPE_CLASSIC or BT_DEVTYPE_BLE */
    bt_devclass_t dev_class;         /* major device class */
    uint32_t      class_of_device;   /* raw 24-bit CoD (0 for BLE) */
    int8_t        tx_power;          /* TX power dBm, BT_TX_POWER_UNKNOWN if absent */
    uint8_t       flags;             /* AD flags, BT_FLAGS_NONE if absent */
    uint16_t      company_id;        /* manufacturer ID, BT_COMPANY_NONE if absent */
    uint16_t      service_uuid;      /* first 16-bit service UUID, BT_SERVICE_NONE if absent */
} bt_scan_result_t;
```

Up to `BT_MAX_SCAN_RESULTS` (20) devices are stored, one per address.  Repeat
reports of a device during the scan refresh its `rssi` with the latest reading.  Classic
scan duration is fixed at 5 × 1.28 s ≈ 6.4 s and `bt_scan_is_done()` turns true when it
completes.  The BLE scan runs concurrently and is **not** stopped then: call
`bt_scan_stop()` once you have read the results (the `bt scan` shell command does), or
the radio keeps receiving BLE reports until the next `bt_scan()`.

`bt_get_state()` stays `BT_STATE_OFF` until BTstack reports the controller is up
(`HCI_STATE_WORKING`), shortly after boot; `bt_scan()` and `bt_scan_start()` refuse
until then.

**Continuous scanning (list swap).**  `bt_scan_start()` works like `wifi_scan_start()`
(§6.5): BLE passive scanning stays on, Classic inquiry restarts back to back in
1.28 s runs, and every `BT_WINDOW_MS` (1000 ms) the list of every device heard in that
window is swapped to the subscriber.

```c
if (bt_scan_start(NULL, NULL) == 0) {                   // or bt_scan_start(cb, ctx)
    const bt_scan_list_t *l;
    while (bt_scan_wait(&l) == 0) {
        for (int i = 0; i < l->count; i++) process(&l->items[i]);
    }
}
```

`bt_scan_list_t` has the same header fields as `wifi_scan_list_t` (`seq`, `dropped`,
`window_ms`, `count`) and `items[BT_MAX_SCAN_RESULTS]`.  Notes:

- A Classic device answers an inquiry at most once per 1.28 s run, so it can be missing
  from a window that falls between two answers.  Keep a short history in the app if
  that matters.
- Names are cached for the session.  A Classic name is requested once per device; a BLE
  name is taken from advertising data whenever one carries it.  Later windows get the
  cached name even when the packet carrying it was not heard.  A failed name request is
  not retried until the next `bt_scan_start()`.
- Within one window, every BLE report from a device is merged into its entry (name,
  flags, TX power, company ID, service UUID), not only the first.

| Function | Returns |
|----------|---------|
| `bt_scan_start(cb, ctx)` | `0`, `BT_ERR_BUSY`, `BT_ERR_NOTREADY` (radio off / HCI not up), `BT_ERR_NOMEM` |
| `bt_scan_wait(&list)` | `0`, `BT_ERR_STOPPED`, or `BT_ERR_ARG` |
| `bt_scan_stop()` | — ends continuous mode, or cuts a one-shot scan short and stops its BLE scan |
| `bt_scan_running()` | `true` while continuous mode is on |

---

### 6.7 Flash

```c
uint8_t uid[FLASH_UID_SIZE];                        // 8 bytes
dev_ioctl(DEV_FLASH, IOCTL_FLASH_GET_UID, uid);    // unique ID of the flash chip
```

| Command | Arg type | Description |
|---------|----------|-------------|
| `IOCTL_FLASH_GET_UID` (0x0500) | `uint8_t[FLASH_UID_SIZE]` | Fills the board's unique 8-byte flash ID |

---

## 7. Filesystem

Include: `src/kernel/fs.h` (or use VFS paths — preferred for read/write).

The filesystem is flash-backed: it starts 1 MB into flash and is 1 MB long on RP2040
boards (3 MB on RP2350).  Each file gets one 4 KB sector, so a file holds at most 4 KB.
Reads come straight from XIP flash.  Writes are buffered in a single 4 KB RAM buffer and
committed to flash on `vfs_close()`, so only one file can be open for writing at a time.
**Flash writes briefly pause both cores** (via `flash_safe_execute()`).

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

Call `shell_register_cmd()` from a kernel module's init function, called from `main.c`
before `sched_start()` (this is what `wifi_init()` and the display/LED drivers do).
The command table has no lock, so do not register commands from app threads while the
shell is running.

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

The handler runs on the shell thread, so a long-running command blocks the shell.
Use `run` and an app for anything that should run in the background.

`shell_register_cmd` returns `0` on success, `-1` if the command table is full
(`SHELL_MAX_CMDS = 32`).

### Output Functions

```c
shell_print("value = %u\r\n", val);   // printf-style formatted output
shell_println("done");                 // print string + CRLF
```

### Launching Apps at Boot (`config.txt`)

The shell reads `config.txt` from the filesystem once, when it starts:

| Line | Effect |
|------|--------|
| `AUTORUN=<app>` | Spawn `<app>` from `app_table[]` at boot (PID 50+, priority from the table, 2 KB stack, `arg = NULL`) |
| `BUTTONA=<app>` … `BUTTONY=<app>` | Display builds: launch `<app>` when that Display Pack button is pressed (PID 200+; a second press while it runs is ignored) |

```
pico> fs write config.txt AUTORUN=sensor
pico> reboot
...
[shell] autorun: started 'sensor' PID 50 TID 4
```

Only the first 255 bytes of `config.txt` are read, only the first `AUTORUN=` line is
used, and the value must match the app name exactly (no trailing spaces).  See
[application.md](application.md#launching-an-app-automatically) for details.

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
| 17 | `SYS_GETCORE` | `sys_getcore()` | Get current core number |

### Thread Affinity

| Constant | Value | Meaning |
|----------|-------|---------|
| `THREAD_AFFINITY_ANY` | -1 | Eligible on either core (default) |
| `THREAD_AFFINITY_C0` | 0 | Core 0 only |
| `THREAD_AFFINITY_C1` | 1 | Core 1 only |

Set via `CURRENT_TCB->affinity = THREAD_AFFINITY_C1;` (etc.) at thread start.  
`CURRENT_TCB` expands to `current_tcb[get_core_num()]`.

### Synchronization Primitives

| Type | Init | Blocking operations | Non-blocking |
|------|------|---------------------|--------------|
| `spinlock_t` | `spinlock_init(&s)` | `spinlock_irq_acquire/release`, `spinlock_acquire/release` | — |
| `kmutex_t` | `kmutex_init(&m)` | `kmutex_lock(&m)`, `kmutex_unlock(&m)` | — |
| `ksemaphore_t` | `ksemaphore_init(&s, n)` | `ksemaphore_wait(&s)`, `ksemaphore_signal(&s)` | — |
| `event_flags_t` | `event_flags_init(&e)` | `event_flags_set/clear/wait` | — |
| `mqueue_t` | `mqueue_init(&q, size)` | `mqueue_send(&q, &msg)`, `mqueue_recv(&q, &buf)` | `mqueue_try_send`, `mqueue_try_recv` |

### VFS Mode Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `VFS_O_RDONLY` | 0x01 | Read-only |
| `VFS_O_WRONLY` | 0x02 | Write-only |
| `VFS_O_RDWR` | 0x03 | Read-write |
| `VFS_O_CREAT` | 0x04 | Create if absent |
| `VFS_O_TRUNC` | 0x08 | Truncate on open |
| `VFS_O_APPEND` | 0x10 | Write at end of file |

### All ioctl Commands

| Command | Code | Device | Arg type |
|---------|------|--------|----------|
| `IOCTL_TIMER_GET_TICK` | 0x0100 | `DEV_TIMER` | `uint32_t *` |
| `IOCTL_TIMER_GET_US` | 0x0101 | `DEV_TIMER` | `uint64_t *` |
| `IOCTL_GPIO_SET_DIR` | 0x0200 | `DEV_GPIO` | `uint32_t *` → `pin \| (dir << 16)` |
| `IOCTL_GPIO_SET_VAL` | 0x0201 | `DEV_GPIO` | `uint32_t *` → `pin \| (val << 16)` |
| `IOCTL_GPIO_GET_VAL` | 0x0202 | `DEV_GPIO` | `uint32_t *` → pin in, 0/1 out |
| `IOCTL_FLASH_GET_UID` | 0x0500 | `DEV_FLASH` | `uint8_t[FLASH_UID_SIZE]` |
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
| `MAX_THREADS` | 16 | Maximum live threads (3 used at boot: idle, idle1, shell → **13 free**; 4 on WiFi builds) |
| `MAX_PROCESSES` | 8 | Maximum processes |
| `MQ_MAX_MSG` | 16 | Messages per queue |
| `MQ_MSG_SIZE` | 64 | Max bytes per message |
| `VFS_MAX_OPEN` | 16 | Max simultaneous open VFS fds |
| `FS_MAX_OPEN_FDS` | 8 | Max simultaneously open filesystem files |
| `FS_MAX_FILES` | 64 / 127 | Max files — 64 on RP2040, 127 on RP2350 |
| `FS_MAX_FILE_DATA` | 4096 | Max bytes per file |
| `SHELL_MAX_CMDS` | 32 | Max registered shell commands |
| `HEAP_SIZE` | 65536 | Kernel heap in bytes (64 KB) |
| `WIFI_MAX_SCAN_RESULTS` | 16 | Max WiFi scan results stored |
| `BT_MAX_SCAN_RESULTS` | 20 | Max Bluetooth scan results stored |
| `DISP_WIDTH` | 240 or 320 | Display width — 240 (Display Pack) / 320 (Display Pack 2) |
| `DISP_HEIGHT` | 135 or 240 | Display height — 135 (Display Pack) / 240 (Display Pack 2) |
