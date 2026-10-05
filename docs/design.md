A good way to think about this is as a **small teaching kernel for the Raspberry Pi Pico family (RP2040 / RP2350)**, not a scaled-down desktop OS.

That matters because the Pico’s hardware is very capable for a microcontroller, but it is still a microcontroller: RP2040 gives you **two Cortex-M0+ cores up to 133 MHz, 264 KB SRAM, USB 1.1 device/host support, and execute-in-place from external QSPI flash** (2 MB on Pico, 4 MB on Pico 2). RP2350 raises this to two Cortex-M33 cores up to 150 MHz, 520 KB SRAM, and 4 MB flash. Both chips share the same dual-core SMP programming model via the Pico SDK. The boot ROM supports **UF2 USB boot**, and the SDK includes libraries for **USB, timers, synchronization, and multicore programming**. ([pip.raspberrypi.com][1])

## 1. Overall design direction

The overall design structure is a **monolithic microkernel-style teaching OS** with these pieces:

* **Boot / hardware bring-up**
* **Kernel core**
* **Scheduler and threading**
* **Console and shell**
* **Flash-backed mini filesystem**
* **User program model**
* **Host-side development tool**
* **Firmware update path**

It should feel like an OS to students, but internally stay simple enough that they can read most of the code.

## 2. What “process” should mean on Pico

On this platform, there is no true protected processes in the Unix sense.

Instead, define two execution concepts:

* **Kernel threads**
  Scheduled entities with their own stack, register context, priority, state, and accounting info.

* **Processes as containers**
  A process owns:

  * one or more threads
  * a handle table
  * current working directory
  * heap region
  * open files
  * metadata such as name, PID, parent PID

This gives us the **teaching abstraction** of processes and threads, even though hardware-enforced isolation is not really the point here. In other words: the system can support `ps`, `kill`, `spawn`, and multithreaded applications, while still being honest that this is an embedded teaching OS.

## 3. Recommended kernel architecture

### Boot layer

Responsibilities:

* initialize clocks
* set up SRAM layout
* initialize USB console
* initialize timer interrupt
* initialize flash filesystem metadata
* create the idle task and init task
* launch second core

### Kernel core

Core kernel modules:

* task/thread manager
* scheduler
* syscall dispatcher
* memory allocator
* synchronization primitives
* IPC/message queues
* VFS-lite layer
* device abstraction

Keep the first version very explicit and readable:

* structs in header files
* mostly static allocation at first
* predictable control flow
* no clever metaprogramming

That makes it teachable.

## 4. Dual-core SMP scheduling

Both cores run the **same preemptive priority round-robin scheduler** concurrently.
Each core has its own SysTick, PendSV, time-slice counter, and `current_tcb` slot
(`current_tcb[0]` for Core 0, `current_tcb[1]` for Core 1).  Both cores pull
threads from the same shared ready queues.

SMP correctness is provided by RP2040/RP2350 **hardware SIO spinlocks** (the
Cortex-M0+ has no LDREX/STREX).  A spinlock disables interrupts on the local core
only, for as long as it is held:

* dedicated hardware spinlocks guard the scheduler ready queues, the kernel heap
  allocator and the event-waiter pool
* mutexes, semaphores, event flags and message queues share a 4-entry striped pool
  of hardware locks (`prim_pool`), so any number of them can exist
* kernel mutexes serialise VFS and filesystem operations

[locking.md](locking.md) describes the design and the hardware-lock budget in detail.

**Practical split:**

* **Core 0** — USB (`dev_console_poll()` in the idle thread), SysTick sleep/wake scan, deadlock
  scanner, the CYW43 WiFi/BT interrupt on pico_w / pico2_w, filesystem writes (flash
  erase/program via the SDK's `flash_safe_execute()`, which pauses Core 1).
* **Core 1** — registers as a multicore lockout victim so flash writes on Core 0 can
  safely pause it; runs its own SysTick and PendSV; executes any thread eligible for Core 1.

**Thread affinity** controls placement:

| Constant | Value | Effect |
|----------|-------|--------|
| `THREAD_AFFINITY_ANY` | -1 | Scheduled on whichever core picks it up (default) |
| `THREAD_AFFINITY_C0` | 0 | Core 0 only |
| `THREAD_AFFINITY_C1` | 1 | Core 1 only |

An app sets affinity immediately at thread start via `CURRENT_TCB->affinity`.
The scheduler's `sched_next_thread()` skips threads whose affinity does not match
the calling core.

This design is a teaching step up from the initial single-core version: students can
observe SMP races, reason about spinlock granularity, and measure dual-core speedup
(e.g., the `pi` Monte Carlo app shows ~2× improvement when workers split across cores).

## 5. Scheduler model

picoOS uses a **preemptive priority round-robin scheduler** running on both cores.

Thread states:

* NEW
* READY
* RUNNING
* BLOCKED
* SLEEPING
* ZOMBIE

Per-thread control block (TCB):

* TID, owning PID
* stack base / size / canary
* saved SP and EXC_RETURN (for Cortex-M33 FPU frame detection on RP2350)
* priority (0 = highest, 7 = lowest)
* affinity (THREAD_AFFINITY_ANY / C0 / C1 — enforced by the scheduler)
* state, wake timestamp, accumulated CPU time
* thread name (16 chars)

Behavior:

* SysTick fires every 1 ms on each core independently
* fixed 10 ms time slices (`TIME_SLICE_MS`); each core maintains its own slice counter.
  A thread woken from `sys_sleep()` becomes READY at once but, unless it outranks the
  running thread, only runs at the next slice boundary
* priority-based linked-list ready queues (one per priority level, shared across cores)
* sleep implemented with absolute wake timestamps; Core 0 SysTick scans sleeping threads
* blocking on mutexes/semaphores/event flags/queues

For teaching, students can observe:

* context switching (PendSV assembly in `sched_asm.S`)
* priority inversion and starvation
* SMP scheduling races when affinity is `ANY`
* the cost of cross-core synchronization vs. keeping work on one core
* tick overhead and time-slice tuning

## 6. Synchronization and IPC

Essential primitives:

* spinlock
* mutex
* semaphore
* event flag
* message queue
* pipe/ring buffer

Because the console will matter a lot, I decided to make **message passing** a first-class concept. Most services can then look like:

* shell sends request
* service handles it
* reply comes back via queue

That gives students a clear mental model without needing a huge syscall layer right away.

## 7. Console and teaching interface

The host environment will be connected over USB.  This allows a simple serial console access to the OS.

### First implementation

Use **USB CDC serial** as the console:

* line input
* line output
* simple shell
* log messages
* panic dump

The Pico SDK already provides USB support and higher-level libraries for USB and multicore work. ([pip.raspberrypi.com][3])

### Shell commands

Built-in commands:

* `info` — version and build info
* `help` — list registered commands
* `ps` — show processes
* `threads` — show all threads with state, core affinity, CPU time, stack canary
* `kill <tid>` — terminate a thread
* `killproc <pid>` — terminate all threads in a process
* `mem` — heap usage and stats
* `ls` — list filesystem files
* `cat <file>` — print a file
* `fs write <file> [text]` — create or overwrite a file
* `fs append <file> <text>` — append to a file
* `fs format` — erase and reinitialise the filesystem
* `rm <file>` — delete a file
* `run <name> [arg]` — launch a registered built-in app; optional arg is passed to the entry function
* `trace [on [name]|off|show [n]]` — record scheduler events (switches, wake-ups, kills) into a 128-event ring under the scheduler lock; `show` prints them from the shell thread
* `reboot` — hard reboot
* `update` — reboot into USB BOOTSEL mode for reflashing

Optional commands registered at init:

* `wifi [status|scan|connect|disconnect]` — pico_w / pico2_w only
* `bt [status|scan]` — pico_w / pico2_w only
* `display <subcmd>` — ST7789 display control (when `PICOOS_DISPLAY_SHELL`)
* `led <r> <g> <b>` — RGB LED control (when `PICOOS_LED_SHELL`)

At start-up the shell also reads `config.txt` from the filesystem: `AUTORUN=<app>`
launches an app at boot, and `BUTTONA/B/X/Y=<app>` bind Display Pack buttons to apps.

### Host-side companion tool

A small Python terminal program (`tools/console.py`) that:

* connects to the Pico over USB serial (auto-detected by VID:PID)
* provides a console window
* uploads files (`--upload`, via `fs write` multi-line mode)
* tees the session to a log file (`--log`)

`tools/scantest.py` drives the on-device `scantest` app for scripted testing, and
`tools/mem_report.py` reports memory use from a build's linker map.

That lets the OS remain simple while the development experience still feels polished.

## 8. Firmware update path

We will use the RP2040 boot ROM’s existing update model:

* from the shell, issue an `update` command
* reboot into USB BOOTSEL mode
* drag/drop UF2 from the host

RP2040’s boot ROM already supports USB UF2 boot, and the SDK exposes a ROM helper to reboot into USB boot mode. ([pip.raspberrypi.com][1])

That keeps the kernel simpler and leverages a reliable path already supported by the hardware.

## 9. Filesystem design

This is where the project becomes very interesting, because flash and RAM are tight, we have a **very small flash-native filesystem** rather than FAT, or other small file systems.

### Important hardware constraint

RP2040 executes code from external flash via XIP, and **flash erase/program operations are unsafe if another core or interrupt handler is executing from flash at the same time**; the official docs say you must synchronize cores and disable interrupts appropriately during flash programming. ([Raspberry Pi][4])

That means your filesystem layer should be designed around this reality.

### Recommended filesystem approach

Use:

* one reserved flash region for filesystem
* fixed-size blocks
* append/log-structured metadata
* tiny directory model
* wear leveling later

Store:

* superblock
* file table
* block allocation map
* file data blocks

Initial constraints:

* max 32 or 64 files
* short filenames
* single root directory
* fixed metadata slots
* sequential file growth

**As implemented** (`src/kernel/fs.c`): the region starts 1 MB into flash (1 MB long on
RP2040, 3 MB on RP2350).  Sector 0 holds the superblock (magic, version, and a fixed
table of `FS_MAX_FILES` entries: 64 on RP2040, 127 on RP2350).  Each file owns exactly
one 4 KB sector, so files are at most 4 KB and there is no allocation map.  Names are up
to 15 characters in one flat directory.  Reads come straight from XIP flash; writes go
to a single 4 KB RAM buffer and are erased/programmed on close, so only one file can be
open for writing at a time.  There is no wear levelling or crash recovery yet.

This is not elegant, but it is perfect for teaching because students can later improve:

* fragmentation
* wear leveling
* directory hierarchy
* caching
* crash recovery

## 10. Memory model

You need to be very deliberate here because 264 KB SRAM disappears fast. ([pip.raspberrypi.com][1])

Suggested memory breakdown:

* kernel text/data/bss
* scheduler/kernel objects
* per-thread stacks
* message buffers
* console buffers
* file cache
* user heaps

A practical approach:

* **static kernel object pools**

  * TCB pool
  * PCB pool
  * file descriptor pool
  * queue pool

* **simple heap allocator**

  * first-fit or boundary-tag allocator
  * one global heap first
  * per-process heaps later

* **dynamic thread stacks** — allocated from the kernel heap via `kmalloc` at thread
  creation and freed on exit (no static pool):

  * 2 KB (`DEFAULT_STACK_SIZE`) — general-purpose threads
  * 3 KB (`DEEP_STACK_SIZE`) — deep call chains, heavy printf
  * 512 B (`IDLE_STACK_SIZE`) — idle threads only

For debugging, add:

* stack canaries
* heap watermarking
* memory stats command

## 11. System call surface

Keep it intentionally small.

Implemented syscalls (`src/kernel/syscall.h`, numbers 0–17):

* `spawn(name, entry, arg)`
* `thread_create(pid, entry, arg)`
* `exit(code)`
* `yield()`
* `sleep(ms)`
* `open(path, mode)`
* `read(fd, buf, n)`
* `write(fd, buf, n)`
* `close(fd)`
* `mq_send(q, msg)`
* `mq_recv(q, msg)`
* `mutex_lock(m)`
* `mutex_unlock(m)`
* `getpid()`, `gettid()`, `getcore()`
* `ps()`, `kill(tid)`

All go through `syscall_dispatch()`, which is an ordinary function call: there is no
SVC instruction and no argument validation.  Apps normally use the `sys_*` inline
wrappers (`sys_sleep`, `sys_yield`, `sys_exit`, `sys_getpid`, `sys_gettid`,
`sys_getcore`).

This gives enough surface area to feel like an OS without exploding complexity.

## 12. User program model

Applications are **compiled into the firmware image** and registered in `app_table[]`
(defined in `src/apps/demo.c` for standalone builds, or supplied by the parent project
when picoOS is used as a git submodule).

The shell `run <name> [arg]` command looks up the name in `app_table[]`, creates a new
process and thread, and passes the optional argument to the entry function as `void *arg`.
The argument is heap-allocated by the shell and owned by the app, which must `kfree(arg)`
when done.

Built-in apps included with picoOS:

* **producer / consumer / sensor** — cross-core IPC demo using semaphores and message queues
* **pi** — Monte Carlo π estimation; `run pi` uses all four workers split across both cores,
  `run pi single` pins all workers to Core 0 for a single-core baseline comparison
* **cray-one** — multi-node WiFi multicast colour-grid demo (pico_w / pico2_w with a display)
* **scantest** — on-device stress test for the WiFi/BT scan-result buffers (pico_w / pico2_w)

Besides `run`, an app can be started at boot with `AUTORUN=<name>` in `config.txt`, or
by a Display Pack button with `BUTTONA=<name>` (etc.).

This is the only application model — there is no bytecode interpreter or VM.
The teaching value comes from reading and modifying the C source directly.

## 13. Device model

Keep devices simple.  Implemented devices (`src/kernel/dev.c`, `src/drivers/`):

* console device (`/dev/console`)
* timer device (`/dev/timer` — tick count and microseconds via ioctl)
* flash device (`/dev/flash` — unique ID via ioctl; raw read/write not implemented)
* GPIO device (`/dev/gpio` — direction/value via ioctl)
* display (`/dev/display`) and RGB LED (`/dev/led`) on Display Pack builds
* optional UART/SPI/I2C devices later

Expose a minimal device API:

* `open`
* `read`
* `write`
* `ioctl`

This gives students a recognizable OS abstraction without requiring a full driver framework.

## 14. What to make intentionally “not the best”

The main concept of this system is to make everything functional, but suboptimal.  This allows for students to experiment, and expand areas to make the system better.

The following is intentionally left as imperfect:

* **O(n) ready queue scanning**

  * easy to implement
  * later optimization: priority queues / bitmaps

* **fixed-size stacks**

  * easy to reason about
  * later optimization: adaptive sizing or analysis tools

* **simple first-fit heap**

  * fragmentation becomes a teaching point

* **linear file lookup**

  * fine at small scale
  * later optimization: indexed directories

* **synchronous console I/O**

  * later optimization: buffered async I/O

* **single-root filesystem**

  * later optimization: hierarchical namespace

That gives students real, visible optimization opportunities.

## 15. Project phases

### Phase 1 — bring-up ✅ Complete

* Boot banner over USB
* Basic shell with `help`, `ps`, `threads`, `mem`, `reboot`, `update`
* 1 ms SysTick, preemptive priority round-robin scheduler
* Demo producer/consumer/sensor threads

### Phase 2 — kernel basics ✅ Complete

* Mutex, semaphore, event flags, message queues
* Process/thread lifecycle: `kill`, `killproc`, `sleep`, `yield`
* Memory stats, stack canaries
* Host-native unit test suites (mem, sync, fs, vfs)

### Phase 3 — second core ✅ Complete

* Full SMP: both cores run independent SysTick/PendSV schedulers
* Thread affinity (`THREAD_AFFINITY_ANY/C0/C1`) enforced by `sched_next_thread()`
* RP2040 hardware SIO spinlocks protecting ready queues, heap, and VFS
* `idle1` thread pinned to Core 1; `CURRENT_TCB` macro for per-core TCB access
* Cross-core producer/consumer demo; Monte Carlo π SMP benchmark (`run pi [single]`)
* Deadlock detection instrumentation (`PICOOS_LOCK_DEBUG`)

### Phase 4 — filesystem ✅ Complete

* Flash-backed persistent filesystem (1 MB offset, XIP reads, sector erase/program on write)
* `fs_open/read/write/close/delete/list/format` with O_CREAT, O_TRUNC, O_APPEND flags
* Shell commands: `ls`, `cat`, `rm`, `fs write`, `fs append`, `fs format`
* FS_MAX_FILES: 64 (RP2040) / 127 (RP2350); 4 KB per file

### Phase 5 — user services 🔲 Planned

* Logger service
* App launcher enhancements (boot-time `AUTORUN=` and Display Pack button bindings via
  `config.txt` already work)
* Background worker threads

### Phase 6 — teaching polish 🔲 Planned

* Scheduler visualiser
* Enhanced panic dumps
* Host-side loader / console tool improvements

## 16. Recommended implementation language

Use **C with the Pico SDK** for the kernel.

That is the most natural fit for RP2040, and the SDK is explicitly intended for building programs and even larger runtime environments on RP-series microcontrollers. ([pip.raspberrypi.com][3])

Use **Python on the host side** for:

* terminal UI
* file transfer helper
* log capture
* test automation

That split will keep the target code small and the tooling pleasant.

## 17. Design approach


> **picoOS**: a dual-core SMP educational operating system for the RP2040 / RP2350 with a USB console shell, preemptive threads on both cores, thread affinity, SMP-safe synchronization primitives, software-defined processes, a flash-native filesystem, and deliberately imperfect internals meant to be profiled, debugged, and improved by students.

This is realistic, teachable, and rich enough for a full course or multi-semester project.

The single biggest architectural rule I would enforce is this:

> **Treat flash writes as a special operation that temporarily suspends normal execution assumptions.**

That rule follows directly from how RP2040 uses external flash/XIP and from the official warning that flash operations require careful synchronization across cores and interrupts. ([pip.raspberrypi.com][1])

The second biggest rule:

> **Make “process” a software abstraction, not a promise of hardware isolation.**

That keeps the design honest and achievable.

[1]: https://pip.raspberrypi.com/documents/RP-008371-DS-rp2040-datasheet.pdf "RP2040 Datasheet: A microcontroller by Raspberry Pi."
[2]: https://www.raspberrypi.com/documentation/microcontrollers/microcontroller-chips.html "Microcontroller chips - Raspberry Pi Documentation"
[3]: https://pip.raspberrypi.com/documents/RP-009085-KB-raspberry-pi-pico-c-sdk.pdf "Raspberry Pi Pico-series C/C++ SDK: Libraries and tools for C/C++ development on Raspberry Pi microcontrollers."
[4]: https://www.raspberrypi.com/documentation/pico-sdk/hardware.html?utm_source=chatgpt.com "Hardware APIs - Raspberry Pi Documentation"

