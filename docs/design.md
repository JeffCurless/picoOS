A good way to think about this is as a **small teaching kernel for RP2040/Pico**, not a scaled-down desktop OS.

That matters because the Pico’s hardware is very capable for a microcontroller, but it is still a microcontroller: RP2040 gives you **two Cortex-M0+ cores up to 133 MHz, 264 KB of SRAM in six banks, USB 1.1 device/host support, and execute-in-place from external QSPI flash**. On a standard Pico/Pico W board, that typically means **2 MB of on-board flash**. The boot ROM already supports **UF2 USB boot**, and the Pico SDK includes libraries for **USB, timers, synchronization, and multicore programming**. ([pip.raspberrypi.com][1])

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

## 4. Core split across the two RP2040 cores

RP2040 supports a symmetric dual-core design, but for a teaching OS we would to avoid “full SMP everywhere” at first and use an **asymmetric policy** on top of the hardware. ([Raspberry Pi][2])

Recommended split:

* **Core 0**

  * USB console
  * timer tick
  * scheduler coordination
  * filesystem writes
  * shell / init services

* **Core 1**

  * user worker threads
  * compute-heavy or blocking tasks
  * background services

This is easier to explain than fully shared SMP, and students can later improve it into a more balanced design.

## 5. Scheduler model

Start with a **preemptive priority round-robin scheduler**.

Thread states:

* NEW
* READY
* RUNNING
* BLOCKED
* SLEEPING
* ZOMBIE

Per-thread control block:

* TID
* owning PID
* stack base / size
* saved registers / SP
* priority
* affinity hint
* state
* wake time
* CPU time used

Suggested behavior:

* system tick every 1 ms
* fixed-size time slice, maybe 5–20 ms
* priority-based ready queues
* sleep implemented with wake timestamps
* blocking on queues/semaphores/mutexes

For teaching, this is ideal because students can see:

* context switching
* starvation
* priority inversion
* fairness tradeoffs
* tick overhead

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

Start with:

* `help`
* `ps`
* `kill`
* `run`
* `threads`
* `mem`
* `ls`
* `cat`
* `write`
* `rm`
* `reboot`
* `update`
* `trace on/off`

### Host-side companion tool

A small Python terminal program that:

* connects to the Pico over USB serial
* provides a console window
* uploads files
* downloads logs
* maybe wraps commands in a nicer UI

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

* **fixed thread stack sizes**

  * 2 KB default
  * 4 KB for shell/service tasks
  * optional compile-time overrides

For debugging, add:

* stack canaries
* heap watermarking
* memory stats command

## 11. System call surface

Keep it intentionally small.

Example syscall set:

* `spawn(name, entry, args)`
* `thread_create(proc, entry, arg)`
* `exit(code)`
* `yield()`
* `sleep(ms)`
* `open(path, mode)`
* `read(fd, buf, n)`
* `write(fd, buf, n)`
* `close(fd)`
* `mq_send(q, msg)`
* `mq_recv(q, msg)`
* `mutex_lock(id)`
* `mutex_unlock(id)`

This gives enough surface area to feel like an OS without exploding complexity.

## 12. User program model

Supported in two phases.

### Phase 1: built-in applications

Applications are linked into the firmware image and registered in an app table:

* shell
* test task
* logger
* editor
* demo producer/consumer
* sensor service

This avoids binary loading complexity early.

### Phase 2: pseudo-executables

Add a tiny bytecode or interpreted format:

* command language
* stack VM
* tiny ELF-like metadata if you want a challenge

That would be an excellent teaching extension: students could compare native tasks vs interpreted tasks.

## 13. Device model

Keep devices simple:

* console device
* timer device
* flash filesystem device
* GPIO device
* optional UART/SPI/I2C devices later

Expose a minimal device API:

* `open`
* `read`
* `write`
* `ioctl`

This gives students a recognizable OS abstraction without requiring a full driver framework.

## 14. What to make intentionally “not the best”

The main concept of this system is to make everything functional, but suboptimal.  This allows for students to experiment, and expand areas to make the system better.

The following is intentionally left as imperfect in version 1:

* **global kernel lock**

  * easy to understand
  * later optimization: finer-grained locking

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

### Phase 1 — bring-up

* boot to banner over USB
* basic shell
* timer tick
* single-core scheduler
* two demo threads

### Phase 2 — kernel basics

* mutexes, semaphores, queues
* process/thread structs
* `ps`, `kill`, `sleep`
* memory stats

### Phase 3 — second core

* launch core 1
* pin some work there
* add cross-core ready queue or dispatch

### Phase 4 — filesystem

* reserve flash area
* file create/read/write/delete
* shell commands for files

### Phase 5 — user services

* logger service
* shell service
* app launcher
* background worker

### Phase 6 — teaching polish

* tracing
* panic dumps
* scheduler visualizer
* host-side loader/console tool

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


> **picoOS**: a dual-core educational operating system for RP2040 with a USB console shell, preemptive threads, software-defined processes, a tiny flash-native filesystem, and a deliberately imperfect first implementation meant to be profiled, debugged, and improved by students.

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

