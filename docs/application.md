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

1. Creates a new process (PCB) with a fresh PID.
2. Creates one thread (TCB) inside that process, pointing at `my_app`.
3. Adds the thread to the ready queue; the scheduler runs it preemptively.

Applications interact with the kernel through the syscall wrappers in
`kernel/syscall.h`, the synchronisation primitives in `kernel/sync.h`, and the
filesystem in `kernel/fs.h`.  Because there is no MPU enforcement, they may
also call Pico SDK functions directly, but preferring the kernel APIs keeps the
code portable and teaches the right concepts.

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

#include <stdio.h>               /* printf */
#include <stdint.h>

/* myapp — a minimal application that prints a counter every second. */
void myapp(void *arg)
{
    (void)arg;   /* unused in this example */

    uint32_t count = 0u;

    for (;;) {
        sys_sleep(1000);   /* sleep 1 second — yields CPU while waiting */
        printf("[myapp] tick %u\r\n", count);
        count++;
    }
}
```

Key points:

- The function **never returns** for persistent apps; use `for (;;)`.  If it
  does return the thread becomes a zombie and is not restarted.
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
    { "producer", demo_producer, 4u },
    { "consumer", demo_consumer, 4u },
    { "sensor",   demo_sensor,   5u },
};
```

Add your app:

```c
const app_entry_t app_table[] = {
    { "producer", demo_producer, 4u },
    { "consumer", demo_consumer, 4u },
    { "sensor",   demo_sensor,   5u },
    { "myapp",    myapp,         4u },   /* ← new entry */
};
```

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

Open `src/CMakeLists.txt` and append your file to the `PICOOS_SOURCES` list.
The list is assembled near the top of the file before `add_executable`:

```cmake
set(PICOOS_SOURCES
    main.c
    kernel/task.c
    ...
    shell/shell.c
)

# Add your app source here:
list(APPEND PICOOS_SOURCES apps/myapp.c)
```

No other CMake changes are needed.  The include paths for all kernel and app
headers are already set by the `target_include_directories` directive pointing
at `${CMAKE_CURRENT_SOURCE_DIR}` (i.e. `src/`).

---

## Step 4 — Build and flash

The build process is identical to the standard project build:

```bash
cd /path/to/picoOS/build
make -j$(nproc)
```

The output is in `build/src/` and named after the board and display variant,
for example `picoos_D-v0.2.0.uf2` for a pico + Display Pack build.

Flash to the Pico:

```bash
# Option A: drag-and-drop (hold BOOTSEL while plugging in USB)
cp build/src/picoos_D-v0.2.0.uf2 /media/$USER/RPI-RP2/

# Option B: from the running shell (reboots into BOOTSEL automatically)
pico> update
# then copy the UF2 file
```

---

## Step 5 — Run the app

After flashing, connect with the console tool and use the `run` command:

```
pico> run myapp
run: started 'myapp' as PID 101 TID 6

[myapp] tick 0
[myapp] tick 1
...
```

List available apps with `run` (no arguments):

```
pico> run
Usage: run <appname>
Available apps:
  producer
  consumer
  sensor
  myapp
```

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
printf("got %u\r\n", in.value);
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
        printf("read: %s\r\n", (char *)buf);
    }
    fs_close(fd);
}
```

Files survive a reboot.  The filesystem is stored in external QSPI flash
starting at 1 MB from the base address; up to 32 files, 4 KB each.

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

### Output

Standard `printf` works out of the box — it routes to the USB CDC serial port
via the Pico SDK's stdio layer.  Use `\r\n` line endings so the host terminal
renders correctly.

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
| 3 | High-priority user services (e.g. a real-time sensor loop) |
| 4–5 | Normal user applications (recommended default) |
| 6 | Low-priority background tasks |
| 7 | Idle thread — do not use |

Use priority **4** unless your app has a specific reason to be higher or lower.
Giving an app priority 2 or lower will compete directly with the shell and may
make the console unresponsive.

### Stack size

Pass one of the constants from `kernel/task.h` as the stack size when the
thread is created by `run`.  The `run` command always uses `DEFAULT_STACK_SIZE`
(2 KB).  If your app uses deep call chains, large local arrays, or heavy printf
formatting, it may need more stack — in that case launch it programmatically
from `main.c` using `task_create_thread` with `DEEP_STACK_SIZE` (3 KB)
instead.

| Constant | Size | Suitable for |
|----------|------|--------------|
| `DEFAULT_STACK_SIZE` | 2 KB | Simple loops, small local variables |
| `DEEP_STACK_SIZE` | 3 KB | Apps with deep call chains or heavy printf |
| `IDLE_STACK_SIZE` | 512 B | Idle thread only |

A stack canary (`0xDEADBEEF`) is placed at the base of every stack.  Check it
with the `mem` shell command.  If it reads anything other than `0xDEADBEEF` the
stack has overflowed.

### Thread limits

`MAX_THREADS` is 16.  At boot the system creates two threads:

| Thread | Priority |
|--------|----------|
| idle | 7 |
| shell | 2 |

That leaves **14 free thread slots**.  Each `run` invocation consumes one slot.
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
#include <stdio.h>

static ksemaphore_t work_ready;
static uint32_t     shared_value;

static void worker_thread(void *arg)
{
    (void)arg;
    for (;;) {
        ksemaphore_wait(&work_ready);
        printf("[worker] processing value %u\r\n", shared_value);
    }
}

void myapp_mt(void *arg)
{
    (void)arg;

    ksemaphore_init(&work_ready, 0);

    /* Create a worker thread inside this process. */
    pcb_t *proc = task_find_process((uint32_t)sys_getpid());
    if (proc != NULL) {
        task_create_thread(proc, "worker", worker_thread, NULL,
                           5u, DEFAULT_STACK_SIZE);
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
as a `static` global in a common module (e.g. `src/apps/ipc.c`) and expose it
via a header.  Initialise it from `main.c` before the scheduler starts,
following the same pattern used by `demo_ipc_init()` in `demo.c`.

---

## Common mistakes

| Symptom | Likely cause |
|---------|--------------|
| App does not appear in `run` list | Entry not added to `app_table[]` in `demo.c` (standalone) or your `app_table.c` (submodule) |
| Linker error: undefined reference | Source file not added to `src/CMakeLists.txt` |
| App works once, `run` fails the second time | `MAX_THREADS` exhausted — check `threads` output |
| Console freezes when app is running | App using `sleep_ms()` instead of `sys_sleep()` |
| `mem` shows `*** OVERFLOWED ***` | Stack too small — switch to `SERVICE_STACK_SIZE` |
| `fs_open` returns -1 on second write fd | Only one write fd allowed at a time — close it first |
| `printf` output appears corrupted | Missing `\r` before `\n` — use `\r\n` throughout |
