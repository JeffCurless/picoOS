# picoOS Intentional Imperfections

This document catalogs every known deliberate limitation in picoOS v1. Each
entry notes where the imperfect code lives, what a real OS would do instead,
the SRAM impact of fixing it, and a rough difficulty rating.

Students are expected to find, analyze, and improve these. The imperfections
are real — they are the same trade-offs a junior OS developer would make when
optimizing for readability over production readiness.

---

## Entry Format

| Field | Meaning |
|-------|---------|
| **Current behavior** | What the code does today |
| **File:line** | Where to look |
| **Better implementation** | What a production OS would do |
| **SRAM impact** | Savings if fixed (or N/A) |
| **Difficulty** | Low / Medium / High |

---

## 1. Fixed-size thread stacks

**Current behavior**: Every thread slot is backed by a `SERVICE_STACK_SIZE`
(4096-byte) entry in `stack_pool[MAX_THREADS][SERVICE_STACK_SIZE]` regardless
of actual need. `task_thread_create()` accepts a `stack_size` hint parameter
but ignores it — `t->stack_size` is immediately overwritten with
`SERVICE_STACK_SIZE`.

**File:line**: `src/kernel/task.c:34–38`, `src/kernel/task.c:133`

**Better implementation**: Honor the hint passed to `task_thread_create()`;
allocate stacks from a shared byte pool using `kmalloc`. Idle thread needs
~512 B; shell needs ~2 KB; only truly deep-call threads need 4 KB.

**SRAM impact**: ~16–32 KB (if 8 of 16 threads drop from 4 KB to 2 KB)

**Difficulty**: Medium

---

## 2. O(n) ready-queue scan in the SysTick handler

**Current behavior**: Every millisecond the SysTick ISR iterates all
`MAX_THREADS` TCB slots to find sleeping threads whose alarm has expired
(`isr_systick`, line 259). On every context switch `sched_next_thread` walks
priority queues, which is O(priorities × queue depth) but degrades to O(n) at
a single priority level.

**File:line**: `src/kernel/sched.c:259` (sleep scan), `src/kernel/sched.c:170`
(scheduler comment)

**Better implementation**: Maintain a sorted sleep queue (min-heap or sorted
list keyed on `wake_time_us`). Wake only threads at the head whose deadline has
passed. For scheduling, a 32-bit priority-ready bitmap (`__builtin_clz`) gives
O(1) pick.

**SRAM impact**: None (CPU/latency improvement only)

**Difficulty**: Medium

---

## 3. Global kernel lock (interrupt disable)

**Current behavior**: All kernel operations — scheduling, heap allocation,
filesystem, IPC — are serialized by disabling interrupts
(`__disable_irq` / `__enable_irq`). This prevents any real concurrency
between Core 0 and Core 1 while the kernel is active.

**File:line**: `src/kernel/sync.c:21–30` (spinlock used as the global lock),
`src/main.c:63` (kernel init)

**Better implementation**: Fine-grained per-subsystem spinlocks: one for the
scheduler ready queues, one for the heap allocator, one for the filesystem, one
for the VFS handle table. Core 1 can then run user threads without stalling on
filesystem activity happening on Core 0.

**SRAM impact**: None (correctness and throughput improvement)

**Difficulty**: High

---

## 4. Interrupt-disable spinlock (no LDREX/STREX)

**Current behavior**: `spinlock_acquire()` disables interrupts globally to
prevent races, then busy-waits on a plain `volatile uint32_t` lock word. This
is sufficient for the current simple workload but masks ISR latency and would
break under real SMP load.

**File:line**: `src/kernel/sync.c:11–21`

**Better implementation**: Use Cortex-M0+ `LDREX`/`STREX` exclusive-access
instructions for the lock word. Disable IRQs only during the critical section
that modifies shared data — not for the entire spin loop. This keeps ISR
latency bounded regardless of lock contention.

**SRAM impact**: None

**Difficulty**: Medium

---

## 5. First-fit heap allocator

**Current behavior**: `kmalloc` walks the boundary-tag free list from the
beginning and returns the first block large enough to satisfy the request.
Over time, small allocations fragment the low end of the heap and large
requests fail even when total free space is sufficient. Fragmentation is
visible via the `mem` shell command.

**File:line**: `src/kernel/mem.c:10`

**Better implementation**: A buddy allocator eliminates external fragmentation
for power-of-two sizes and has O(log n) alloc/free. A slab allocator layered
on top serves the fixed-size kernel objects (TCBs, PCBs, queue messages)
without any per-object header overhead.

**SRAM impact**: None (same pool size; reduces wasted space within it)

**Difficulty**: High

---

## 6. Oversized kernel heap

**Current behavior**: The heap is a static 32 KB array (`HEAP_SIZE`) in BSS.
This reservation is unconditional — the memory is claimed at boot whether
threads actually allocate it or not.

**File:line**: `src/kernel/mem.h` (line defining `HEAP_SIZE`)

**Better implementation**: Reduce the default to 16 KB, which is sufficient for
typical picoOS workloads. Expose `HEAP_SIZE` as a CMake cache variable so
students can tune it without editing headers. The recovered 16 KB is needed
for the WiFi/BT stack (see SRAM summary below).

**SRAM impact**: 16 KB saved

**Difficulty**: Low

---

## 7. Linear file lookup

**Current behavior**: `fs_open()` iterates all `FS_MAX_FILES` (32) directory
entries sequentially, comparing names with `strncmp`. With 32 files this is
negligible, but the pattern does not scale and illustrates a common
beginner mistake.

**File:line**: `src/kernel/fs.c:164`

**Better implementation**: A small open-addressing hash table (64 buckets,
FNV-1a hash) reduces average lookup to O(1). Alternatively a sorted directory
with binary search gives O(log n) with no extra RAM.

**SRAM impact**: None (performance improvement)

**Difficulty**: Medium

---

## 8. Single-root filesystem (no directories)

**Current behavior**: All files share a single flat namespace. `fs_open("foo")`
and `fs_open("bar/foo")` are treated identically — there is no path component
parsing and no directory inode concept.

**File:line**: `src/kernel/fs.c`, `src/kernel/fs.h`

**Better implementation**: Add a directory inode type; parse path components in
VFS before dispatching to `fs_open`. The minimum viable form is a two-level
hierarchy (root + one directory layer), which covers most embedded use cases.

**SRAM impact**: None (feature addition)

**Difficulty**: High

---

## 9. Single concurrent writer

**Current behavior**: Only one file can be open for writing at a time. A
global `scratch_buf[FS_BLOCK_SIZE]` (4 KB) accumulates write data, and
`scratch_owner` records which file currently holds it. A second `open()` for
write blocks or fails until the first writer closes.

**File:line**: `src/kernel/fs.c:55–66`

**Better implementation**: Allocate a write buffer per open file descriptor
(from `kmalloc`), released on `fs_close()`. Protect each buffer with a
per-file mutex so multiple threads can write to different files concurrently.

**SRAM impact**: None at fixed concurrency (trades static reservation for
dynamic allocation)

**Difficulty**: Medium

---

## 10. No MPU / pointer validation in syscalls

**Current behavior**: `syscall_dispatch()` casts `uint32_t` arguments directly
to pointers and dereferences them without any validation. There is no MPU
configuration, so a buggy user thread can corrupt kernel memory by passing a
bad pointer to `read`, `write`, or `mq_send`.

**File:line**: `src/kernel/syscall.c:16–18`

**Better implementation**: Configure MPU regions for each process (read-only
flash, read-write stack+heap, no-access kernel). Validate pointer arguments in
the SVC handler before entering `syscall_dispatch`. On MPU fault, kill the
offending thread rather than crashing the kernel.

**SRAM impact**: None (security improvement; small code increase)

**Difficulty**: High

---

## 11. Synchronous console I/O

**Current behavior**: The shell calls `stdio_getchar()` and `printf` directly,
blocking the shell thread for the entire duration of each USB CDC transaction.
During a long print the shell thread cannot accept new input or service
commands from other sources.

**File:line**: `src/shell/shell.c`

**Better implementation**: Decouple I/O from the shell logic with a pair of
ring buffers (RX and TX). An ISR or USB callback fills RX and drains TX;
the shell thread blocks on a semaphore when RX is empty. TX drains
asynchronously, freeing the shell to process the next command immediately.

**SRAM impact**: None (two small ring buffers added; similar total RAM)

**Difficulty**: Medium

---

## 12. Full-screen display flush (no dirty regions)

**Current behavior**: Every `display_flush_fb()` call transmits the entire
240×135 RGB332 framebuffer (32,400 bytes) over SPI regardless of how many
pixels actually changed. A single character update causes a full-screen DMA
transfer.

**File:line**: `src/drivers/display.c:273–276`

**Better implementation**: Track a dirty rectangle (min/max row and column
updated since the last flush). Transmit only the bounding rows in a single
SPI transaction using the display's partial-update window command. For typical
terminal use this cuts transfer time by 80–90%.

**SRAM impact**: None (8 bytes for the dirty rect; speed improvement)

**Difficulty**: Low–Medium

---

## 13. Linear waiter scan in event flags

**Current behavior**: When `event_post()` is called it scans a flat
`event_waiter_pool[MAX_EVENT_WAITERS]` array (sized `MAX_THREADS`) to find
threads waiting on the posted bits. This runs inside the ISR with interrupts
disabled, adding O(MAX_THREADS) latency to every event post.

**File:line**: `src/kernel/sync.c:193–196`

**Better implementation**: Embed a per-`event_flags_t` intrusive linked list of
waiters. `event_post()` traverses only the threads actually waiting on that
specific event object, which is typically 1–3 threads, giving O(waiters) not
O(MAX_THREADS).

**SRAM impact**: None (pointer overhead per waiter; removes global pool)

**Difficulty**: Medium

---

## 14. Unimplemented device read/write (flash and GPIO via VFS)

**Current behavior**: `dev_flash_read`, `dev_flash_write`, `dev_gpio_read`, and
`dev_gpio_write` are stubs that return `DEV_ERR_UNSUPPORTED`. TODO comments
mark them as Phase 5 work. Opening `/dev/flash` or `/dev/gpio` via VFS and
calling `read`/`write` does nothing useful.

**File:line**: `src/kernel/dev.c:154–180` (flash stubs),
`src/kernel/dev.c:205–220` (GPIO stubs)

**Better implementation**:
- **Flash**: sector-aligned buffered read (XIP cache flush + memcpy from flash
  base address); sector-aligned erase+program write with core-1 halt and
  interrupt disable per the RP2040 flash constraint.
- **GPIO**: encode a pin-value bitmap into `buf` for reads; parse a
  pin-direction/value struct from `buf` for writes, mapping to
  `gpio_set_dir`/`gpio_put`.

**SRAM impact**: None (implements existing stubs)

**Difficulty**: Medium

---

## SRAM Impact Summary

The three high-value fixes below are prerequisites for adding CYW43 WiFi+BT
(BLE only) without removing the display driver (~63 KB framebuffer + stack).

| Fix | SRAM Saved |
|-----|-----------|
| Reduce heap 32 KB → 16 KB (#6) | 16 KB |
| Per-thread stack sizing — 8 threads @ 2 KB instead of 4 KB (#1) | ~16 KB |
| Reduce `MAX_THREADS` 16 → 8 in `task.h` | 32 KB |
| **Total recoverable** | **~48–64 KB** |

The CYW43 driver + lwIP stack requires roughly 40–50 KB at runtime. After
these three changes picoOS fits comfortably within the RP2040's 264 KB SRAM
with WiFi+BT active.
