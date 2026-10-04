# Writing Applications for picoOS

This guide explains how to write, register, build, and run an application on
picoOS.  It assumes you already have a working build environment — see
[setup.md](setup.md) if not.

---

## Application model

In Phase 1 of picoOS all applications are **compiled directly into the
firmware**.  There is no dynamic loader or separate binary format.  An
application is a C function with the signature:

```c
void my_app(void *arg);
```

When the user types `run my_app` at the shell, the kernel:

1. Creates a new process (PCB) with a fresh PID (100 and up).
2. Creates one thread (TCB) inside that process, pointing at `my_app`, with
   the priority from `app_table[]` and a `DEFAULT_STACK_SIZE` (2 KB) stack.
3. Adds the thread to the ready queue; the scheduler runs it preemptively,
   on either core unless the app sets its affinity.

An app can also be started automatically at boot or by a Display Pack button —
see [Launching an app automatically](#launching-an-app-automatically).

Applications interact with the kernel through the syscall wrappers in
`kernel/syscall.h`, the synchronisation primitives in `kernel/sync.h`, the
filesystem in `kernel/fs.h`, devices via `kernel/dev.h`, console output via
`shell/shell.h`, and (on pico_w / pico2_w) networking via `kernel/wifi.h` and
Bluetooth scanning via `kernel/bluetooth.h`.  Apps must not include
`kernel/arch.h`.

**Apps use picoOS APIs only.**  Because there is no MPU enforcement an app
*could* call the Pico SDK, lwIP, or the CYW43 driver directly, but the built-in
demos never do, and new apps should not either.  If an app needs something no
picoOS API provides, add a small wrapper to the kernel first.  Common
replacements:

| Instead of (SDK / lwIP) | Use (picoOS) |
|-------------------------|--------------|
| `printf()` | `shell_print()` / `shell_println()` |
| `sleep_ms()` | `sys_sleep()` |
| `get_core_num()` | `sys_getcore()` |
| `time_us_64()` | `dev_ioctl(DEV_TIMER, IOCTL_TIMER_GET_US, &us)` |
| `pico_get_unique_board_id()` | `dev_ioctl(DEV_FLASH, IOCTL_FLASH_GET_UID, uid)` |
| `cyw43_wifi_get_mac()` | `wifi_get_mac()` |
| lwIP `udp_*` / `igmp_*` / `pbuf_*` | `wifi_mcast_open()` / `wifi_mcast_send()` / `wifi_mcast_close()` |
| reading CYW43 scan results directly | `wifi_copy_scan_results()` / `bt_copy_scan_results()` |

---

## Step 1 — Create the source file

Create a new `.c` file in `src/apps/`.  Optionally create a matching `.h` if
you need to export symbols (e.g. an IPC init function or a shared message
queue).

```
src/
└── apps/
    ├── demo.c        ← existing demo apps
    ├── demo.h
    ├── myapp.c       ← your new file
    └── myapp.h       ← optional
```

Minimal template — `src/apps/myapp.c`:

```c
#include "../kernel/syscall.h"   /* sys_sleep, sys_yield, sys_exit */
#include "../kernel/fs.h"        /* fs_open, fs_read, fs_write, fs_close */
#include "../shell/shell.h"      /* shell_print */

#include <stdint.h>

/* myapp — a minimal application that prints a counter every second. */
void myapp(void *arg)
{
    (void)arg;   /* unused in this example */

    uint32_t count = 0u;

    for (;;) {
        sys_sleep(1000);   /* sleep 1 second — yields CPU while waiting */
        shell_print("[myapp] tick %u\r\n", count);
        count++;
    }
}
```

Key points:

- Persistent apps **never return**; use `for (;;)`.  A one-shot app (like
  `pi`) may simply return: the thread exits, and the process is freed when its
  last thread is gone.  Nothing restarts it.
- Use `sys_sleep(ms)` instead of the Pico SDK's `sleep_ms(ms)`.  `sys_sleep`
  moves the thread to `SLEEPING` state so other threads get CPU time; `sleep_ms`
  busy-waits and starves the scheduler.
- Always cast away unused `arg` with `(void)arg` to avoid compiler warnings.

---

## Step 2 — Register the app in the application table

The shell `run` command discovers apps through the `app_table[]` array defined
at the bottom of `src/apps/demo.c`.  Add an entry for your app there.

Open `src/apps/demo.c` and locate the table at the end of the file:

```c
const app_entry_t app_table[] = {
    { "producer",  demo_producer, 4u },
    { "consumer",  demo_consumer, 4u },
    { "sensor",    demo_sensor,   5u },
    { "pi",        pi_estimate,   3u },
#if defined(PICOOS_WIFI_ENABLE) && defined(PICOOS_DISPLAY_ENABLE)
    { "cray-one",  cray_one,      3u },
#endif
#ifdef PICOOS_WIFI_ENABLE
    { "scantest",  scantest,      4u },
#endif
};
```

Add your app:

```c
const app_entry_t app_table[] = {
    { "producer",  demo_producer, 4u },
    ...
    { "myapp",     myapp,         4u },   /* ← new entry */
};
```

If the app needs WiFi or the display, wrap its entry in the same `#if` as
`cray-one` / `scantest`, so builds without that hardware still link.

The three fields are:

| Field | Type | Description |
|-------|------|-------------|
| `name` | `const char *` | Name typed at the shell prompt (`run <name>`) |
| `entry` | `void (*)(void *)` | Thread entry function |
| `priority` | `uint8_t` | Scheduling priority — see guidance below |

If you created a header (`myapp.h`), include it at the top of `demo.c`:

```c
#include "myapp.h"
```

Otherwise, add a forward declaration directly in `demo.c` above the table:

```c
void myapp(void *arg);
```

---

## Step 3 — Add the source file to the build

Open `src/CMakeLists.txt` and add your file to `PICOOS_SOURCES` next to the
other built-in apps (the "Sources" section, before `add_executable`):

```cmake
if(PICOOS_INCLUDE_DEMO_APPS)
    list(APPEND PICOOS_SOURCES apps/demo.c)
    list(APPEND PICOOS_SOURCES apps/pi.c)
    list(APPEND PICOOS_SOURCES apps/myapp.c)   # ← add your app here
endif()
```

Keep it inside `if(PICOOS_INCLUDE_DEMO_APPS)`, because that is where
`app_table[]` (in `demo.c`) is built.  An app that needs WiFi goes inside the
`if(PICOOS_HAS_WIFI)` block instead, as `scantest.c` does.

No other CMake changes are needed.  The include paths for all kernel and app
headers are already set by the `target_include_directories` directive pointing
at `${CMAKE_CURRENT_SOURCE_DIR}` (i.e. `src/`).

---

## Step 4 — Build and flash

Build all variants with the `build` script (output goes to `kits/`), or one
board by hand:

```bash
cd /path/to/picoOS
./build pico                          # pico + picow, all display variants → kits/
# or a single build directory:
cmake -B build_pico -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico
make -j$(nproc) -C build_pico
```

`build` at the repository root is a script, not a directory — don't pass it to
`cmake -B`.  The output is named after the board and display variant, for
example `build_pico/src/picoos_D-v0.3.6.uf2` for a pico + Display Pack build.

Flash to the Pico:

```bash
# Option A: drag-and-drop (hold BOOTSEL while plugging in USB)
cp build_pico/src/picoos_D-v0.3.6.uf2 /media/$USER/RPI-RP2/

# Option B: from the running shell (reboots into BOOTSEL automatically)
pico> update
# then copy the UF2 file
```

---

## Step 5 — Run the app

After flashing, connect with the console tool and use the `run` command:

```
pico> run myapp
run: started 'myapp' as PID 100 TID 4

[myapp] tick 0
[myapp] tick 1
...
```

(PIDs count up from 100 with each `run`; the TID depends on what is already
running.)

List available apps with `run` (no arguments):

```
pico> run
Usage: run <appname> [arg]
Available apps:
  producer
  consumer
  sensor
  pi
  myapp
```

### Passing an argument

`run <app> <word>` copies `<word>` to the kernel heap and passes it as `arg`.
The app owns that copy and must free it.  `arg` is `NULL` when no word is given
(and always for AUTORUN and button launches):

```c
void myapp(void *arg)
{
    bool fast = false;
    if (arg != NULL) {
        fast = (strcmp((const char *)arg, "fast") == 0);
        kfree(arg);                      /* from kernel/mem.h */
    }
    ...
}
```

`pi.c` (`run pi single`) is a complete example.

Stop an app with `killproc <pid>` (all its threads) or `kill <tid>`.

---

## API reference

All headers are under `src/kernel/`.  Include them with relative paths from
`src/apps/`:

```c
#include "../kernel/syscall.h"
#include "../kernel/sync.h"
#include "../kernel/fs.h"
```

### Scheduling

Declared in `kernel/syscall.h`:

| Function | Description |
|----------|-------------|
| `sys_sleep(uint32_t ms)` | Sleep for at least `ms` milliseconds.  Thread is SLEEPING; CPU is yielded. |
| `sys_yield()` | Voluntarily give up the CPU for one scheduling round. |
| `sys_exit(int code)` | Terminate the current thread.  The thread becomes ZOMBIE. |
| `sys_getpid()` | Return the calling thread's process ID. |
| `sys_gettid()` | Return the calling thread's thread ID. |
| `sys_getcore()` | Return the core (0 or 1) the thread is running on now. |

### Mutual exclusion — `kmutex_t`

Declared in `kernel/sync.h`:

```c
kmutex_t my_mutex;

kmutex_init(&my_mutex);          /* call once before first use */
kmutex_lock(&my_mutex);          /* blocks if another thread holds the lock */
/* ... critical section ... */
kmutex_unlock(&my_mutex);
```

The mutex is non-recursive.  A thread that calls `kmutex_lock` twice without
an intervening `kmutex_unlock` will deadlock.

### Counting semaphore — `ksemaphore_t`

```c
ksemaphore_t my_sem;

ksemaphore_init(&my_sem, 0);     /* initial count (0 = nothing ready yet) */
ksemaphore_signal(&my_sem);      /* increment count; wake a waiter if any */
ksemaphore_wait(&my_sem);        /* decrement count; block if count == 0  */
```

### Event flags — `event_flags_t`

Up to 32 independent binary flags in one object:

```c
event_flags_t my_flags;

event_flags_init(&my_flags);

/* Producer thread: */
event_flags_set(&my_flags, 0x01u);         /* set bit 0 */

/* Consumer thread (waits for bit 0 OR bit 1): */
uint32_t seen = event_flags_wait(&my_flags, 0x03u, false);

/* Consumer thread (waits for BOTH bit 0 AND bit 1): */
uint32_t seen = event_flags_wait(&my_flags, 0x03u, true);

event_flags_clear(&my_flags, 0x01u);       /* clear bit 0 */
```

`event_flags_wait` returns the flags value at the moment the condition was
satisfied.

### Message queue — `mqueue_t`

Fixed-depth (16 messages), fixed-width (up to 64 bytes per message) ring
buffer.  Senders block when full; receivers block when empty.

```c
typedef struct { uint32_t value; } my_msg_t;

mqueue_t my_queue;

mqueue_init(&my_queue, sizeof(my_msg_t));  /* call once before first use */

/* Sender: */
my_msg_t out = { .value = 42u };
mqueue_send(&my_queue, &out);

/* Receiver: */
my_msg_t in;
mqueue_recv(&my_queue, &in);
shell_print("got %u\r\n", in.value);
```

`msg_size` passed to `mqueue_init` must be ≤ `MQ_MSG_SIZE` (64 bytes).

### Filesystem

Declared in `kernel/fs.h`:

```c
/* Write a string to a file. */
int fd = fs_open("data.txt", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
if (fd >= 0) {
    const char *msg = "hello flash";
    fs_write(fd, (const uint8_t *)msg, strlen(msg));
    fs_close(fd);   /* data is committed to flash on close */
}

/* Read it back. */
fd = fs_open("data.txt", VFS_O_RDONLY);
if (fd >= 0) {
    uint8_t buf[64];
    int n = fs_read(fd, buf, sizeof(buf) - 1u);
    if (n > 0) {
        buf[n] = '\0';
        shell_print("read: %s\r\n", (char *)buf);
    }
    fs_close(fd);
}
```

Files survive a reboot.  The filesystem is stored in external QSPI flash
starting at 1 MB from the base address; up to `FS_MAX_FILES` files (64 on
RP2040, 127 on RP2350), 4 KB each.

Only **one file at a time** can be open for writing.  Opening a second write
fd returns -1.  Always close the write fd before opening another.

Open-mode flags (may be OR'd):

| Flag | Value | Meaning |
|------|-------|---------|
| `VFS_O_RDONLY` | 0x01 | Open for reading only |
| `VFS_O_WRONLY` | 0x02 | Open for writing only |
| `VFS_O_RDWR`   | 0x03 | Open for reading and writing |
| `VFS_O_CREAT`  | 0x04 | Create the file if it does not exist |
| `VFS_O_TRUNC`  | 0x08 | Truncate to zero length on open |
| `VFS_O_APPEND` | 0x10 | Start writing at the end of the file |

### Output

Use `shell_print()` (printf-style) or `shell_println()` from `shell/shell.h`.
Output goes to the USB CDC serial port.  Use `\r\n` line endings so the host
terminal renders correctly.

---

## Priority and stack guidance

### Priority

The scheduler is preemptive priority round-robin.  **Lower number = higher
priority.**  Priority 0 is reserved for critical kernel tasks; the idle thread
is always priority 7.

| Priority | Who uses it |
|----------|-------------|
| 0–1 | Reserved — do not use |
| 2 | Shell thread |
| 3 | High-priority user services (e.g. a real-time sensor loop); `btn-mon` (button bindings) |
| 4–5 | Normal user applications (recommended default) |
| 6 | Low-priority background tasks; `wifi-poll` on pico_w / pico2_w |
| 7 | Idle threads (`idle`, `idle1`) — do not use |

Use priority **4** unless your app has a specific reason to be higher or lower.
Giving an app priority 2 or lower will compete directly with the shell and may
make the console unresponsive.

### Stack size

Pass one of the constants from `kernel/task.h` as the stack size when the
thread is created by `run`.  The `run` command always uses `DEFAULT_STACK_SIZE`
(2 KB), and so do AUTORUN and button launches.  If your app uses deep call
chains, large local arrays, or heavy shell_print formatting, it may need more
stack: keep the entry function small and have it create a worker thread with
`task_create_thread(..., DEEP_STACK_SIZE)` (see the multi-threaded example
below).  Large buffers are better made `static` than put on the stack.

| Constant | Size | Suitable for |
|----------|------|--------------|
| `DEFAULT_STACK_SIZE` | 2 KB | Simple loops, small local variables |
| `DEEP_STACK_SIZE` | 3 KB | Apps with deep call chains or heavy shell_print |
| `IDLE_STACK_SIZE` | 512 B | Idle thread only |

A stack canary (`0xDEADBEEF`) is placed at the base of every stack.  Check it
with the `mem` or `threads` shell command.  If it reads anything other than
`0xDEADBEEF` the stack has overflowed (`mem` prints `*** OVERFLOWED ***`,
`threads` prints `OVERFLOW`).

### Thread limits

`MAX_THREADS` is 16.  At boot the system creates three threads, plus one more
on pico_w / pico2_w:

| Thread | Priority |
|--------|----------|
| idle (core 0) | 7 |
| idle1 (core 1) | 7 |
| shell | 2 |
| wifi-poll *(pico_w / pico2_w only)* | 6 |

That leaves **13 free thread slots** (12 on WiFi boards).  Each `run`
invocation consumes one slot, as do any worker threads an app creates.
The demo apps (producer/consumer/sensor) are **not** started at boot — they are
launched on demand via `run producer`, `run consumer`, `run sensor`.

---

## Multi-threaded application

An app entry function can create additional threads within its own process by
calling `task_create_thread` and `task_create_process` directly.  This is
appropriate when your app naturally decomposes into parallel workers.

```c
#include "../kernel/task.h"
#include "../kernel/syscall.h"
#include "../kernel/sync.h"
#include "../shell/shell.h"

static ksemaphore_t work_ready;
static uint32_t     shared_value;

static void worker_thread(void *arg)
{
    (void)arg;
    for (;;) {
        ksemaphore_wait(&work_ready);
        shell_print("[worker] processing value %u\r\n", shared_value);
    }
}

void myapp_mt(void *arg)
{
    (void)arg;

    ksemaphore_init(&work_ready, 0);

    /* Create a worker thread inside this process. */
    pcb_t *proc = task_find_process((uint32_t)sys_getpid());
    if (proc == NULL ||
        task_create_thread(proc, "worker", worker_thread, NULL,
                           5u, DEFAULT_STACK_SIZE) == NULL) {
        /* Out of thread slots or heap — never wait on a worker that
         * does not exist (see src/apps/pi.c). */
        shell_print("[myapp_mt] could not create worker\r\n");
        return;
    }

    /* Main thread acts as the coordinator. */
    uint32_t n = 0u;
    for (;;) {
        sys_sleep(1000);
        shared_value = n++;
        ksemaphore_signal(&work_ready);
    }
}
```

Register this in `app_table[]` as `{ "myapp_mt", myapp_mt, 4u }`.

**Note:** Creating a worker thread costs one slot from the global `MAX_THREADS`
pool.  Check `threads` after launching to confirm no slots are exhausted.

---

## Sharing data between apps

IPC objects (`kmutex_t`, `ksemaphore_t`, `mqueue_t`, `event_flags_t`) are
plain structs.  To share one between two separately-launched apps, declare it
as a global in a common module and expose it via a header.  It must be
initialised exactly once, before either app uses it.  `demo.c` shows one way:
the producer calls `demo_ipc_init()` when it starts and then sets a
`demo_ipc_ready` flag, and the consumer waits for that flag before touching
the queue.

---

## Launching an app automatically

The shell reads a file named `config.txt` from the filesystem once, when the
shell thread starts (just after the scheduler starts).  Two kinds of line
launch apps:

| Line | Effect | Builds |
|------|--------|--------|
| `AUTORUN=<app>` | Starts `<app>` at boot | all |
| `BUTTONA=<app>`, `BUTTONB=<app>`, `BUTTONX=<app>`, `BUTTONY=<app>` | Starts `<app>` when that Display Pack button is pressed | display builds |

`<app>` is the name from `app_table[]`, exactly as you would type it after
`run`.

### Setting up AUTORUN

`fs append` adds a line and creates the file if needed; it keeps any lines
already there (for example `cray-one`'s `SSID=` settings).  `fs write` replaces
the whole file.

```
pico> fs append config.txt AUTORUN=myapp
fs append: appended 14 bytes to 'config.txt'
pico> cat config.txt
AUTORUN=myapp
pico> reboot
```

On the next boot, before the prompt appears:

```
[shell] autorun: started 'myapp' PID 50 TID 4
```

To stop autorunning, remove the line.  There is no line-delete command, so
rewrite the file without it (`fs write config.txt` in multi-line mode, or
upload a new copy with `tools/console.py --upload config.txt config.txt`), or
`rm config.txt` if nothing else is in it.

### How it works

`shell_autorun_init()` in `src/shell/shell.c`, called from `shell_init()`:

1. Opens `config.txt`.  No file means nothing happens.
2. Reads **only the first 255 bytes** and looks for the first line starting
   with `AUTORUN=`.
3. Takes the rest of the line as the app name (up to 31 characters).
4. Looks it up in `app_table[]`, creates a process with PID 50 and up and one
   thread with the table's priority and a `DEFAULT_STACK_SIZE` stack, and
   prints `[shell] autorun: started ...`.

### Rules and limits

- **One app.**  Only the first `AUTORUN=` line is used.  To start several apps,
  autorun one that starts the others with `task_create_process` /
  `task_create_thread`.
- **No argument.**  The app is started with `arg = NULL`.  Read settings from
  `config.txt` yourself if it needs them (as `cray-one` does).
- **Exact match.**  `AUTORUN=myapp ` with a trailing space looks for an app
  called `myapp ` and fails with `[shell] autorun: app 'myapp ' not found`.
  `fs append` does not add trailing spaces; watch for them in uploaded files.
- **First 255 bytes.**  Keep `AUTORUN=` (and `BUTTONx=`) lines near the top
  of `config.txt`.  A line past byte 255 is silently ignored.
- **Errors** are printed with `printf` at boot: `app '<name>' not found`,
  `process pool full`, `thread pool full`.  Connect the console before the
  shell starts (the boot waits up to 3 s for USB) to see them.
- **WiFi apps** can rely on `wifi_init()` having run, because the shell thread
  only starts after `main.c` has initialised WiFi and Bluetooth.  The radio may
  not be connected yet; the app must call `wifi_connect()` itself.

### Button bindings (Display Pack builds)

```
pico> fs append config.txt BUTTONA=pi
pico> fs append config.txt BUTTONB=sensor
pico> reboot
```

At boot the shell prints `[shell] btn A -> pi` for each binding and starts a
`btn-mon` thread (priority 3) that polls the buttons every 100 ms.  A press
starts the app with PID 200 and up; pressing again while that app is still
running is ignored.  App names for buttons are limited to 15 characters.

---

## Common mistakes

| Symptom | Likely cause |
|---------|--------------|
| App does not appear in `run` list | Entry not added to `app_table[]` in `demo.c` (standalone) or your `app_table.c` (submodule) |
| Linker error: undefined reference | Source file not added to `src/CMakeLists.txt` |
| App works once, `run` fails the second time | `MAX_THREADS` exhausted — check `threads` output |
| Console freezes when app is running | App using `sleep_ms()` instead of `sys_sleep()` |
| `mem` shows `*** OVERFLOWED ***` | Stack too small — move work to a thread created with `DEEP_STACK_SIZE`, or make large locals `static` |
| `fs_open` returns -1 on second write fd | Only one write fd allowed at a time — close it first |
| `shell_print` output appears corrupted | Missing `\r` before `\n` — use `\r\n` throughout |
| AUTORUN app does not start | Name does not match `app_table[]` exactly (check for trailing spaces), the line is past the first 255 bytes of `config.txt`, or it is not the first `AUTORUN=` line |
| WiFi app works from `run` but not from a plain-pico build | Entry or source not wrapped in the `PICOOS_WIFI_ENABLE` / `PICOOS_HAS_WIFI` guards |
