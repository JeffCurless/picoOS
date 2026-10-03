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

## 1. O(n) ready-queue scan in the SysTick handler

**Current behavior**: Every millisecond the SysTick ISR iterates all
`MAX_THREADS` TCB slots to find sleeping threads whose alarm has expired
(`isr_systick`, core 0 only). On every context switch `sched_next_thread` walks
priority queues, which is O(priorities × queue depth) but degrades to O(n) at
a single priority level.

**File:line**: `src/kernel/sched.c:441` (sleep scan in `isr_systick`),
`src/kernel/sched.c:311` (`sched_next_thread`)

**Better implementation**: Maintain a sorted sleep queue (min-heap or sorted
list keyed on `wake_time_us`). Wake only threads at the head whose deadline has
passed. For scheduling, a 32-bit priority-ready bitmap (`__builtin_clz`) gives
O(1) pick.

**SRAM impact**: None (CPU/latency improvement only)

**Difficulty**: Medium

---

## 2. First-fit heap allocator

**Current behavior**: `kmalloc` walks the boundary-tag free list from the
beginning and returns the first block large enough to satisfy the request.
Over time, small allocations fragment the low end of the heap and large
requests fail even when total free space is sufficient. Fragmentation is
visible via the `mem` shell command.

**File:line**: `src/kernel/mem.c:80` (`kmalloc`; first-fit search at line 92)

**Better implementation**: A buddy allocator eliminates external fragmentation
for power-of-two sizes and has O(log n) alloc/free. A slab allocator layered
on top serves the fixed-size kernel objects (TCBs, PCBs, queue messages)
without any per-object header overhead.

**SRAM impact**: None (same pool size; reduces wasted space within it)

**Difficulty**: High

---

## 3. Linear file lookup

**Current behavior**: `fs_open()` calls `find_file()`, which iterates all
`FS_MAX_FILES` directory entries sequentially, comparing names with `strncmp`. `FS_MAX_FILES` is 64
on RP2040 and 127 on RP2350 — at these sizes the scan is still fast in
practice, but the linear pattern does not scale and illustrates a common
beginner mistake.

**File:line**: `src/kernel/fs.c:194` (`find_file`)

**Better implementation**: A small open-addressing hash table (64 buckets,
FNV-1a hash) reduces average lookup to O(1). Alternatively a sorted directory
with binary search gives O(log n) with no extra RAM.

**SRAM impact**: None (performance improvement)

**Difficulty**: Medium

---

## 4. Single-root filesystem (no directories)

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

## 5. Single concurrent writer

**Current behavior**: Only one file can be open for writing at a time. A
global `fs_buffer[FS_BLOCK_SIZE]` (4 KB) accumulates write data, and
`scratch_owner` records which file currently holds it. A second `fs_open()`
for write fails (returns -1) until the first writer closes.

**File:line**: `src/kernel/fs.c:88–98` (buffer), `src/kernel/fs.c:346–353`
(the check in `fs_open`)

**Better implementation**: Allocate a write buffer per open file descriptor
(from `kmalloc`), released on `fs_close()`. Protect each buffer with a
per-file mutex so multiple threads can write to different files concurrently.

**SRAM impact**: None at fixed concurrency (trades static reservation for
dynamic allocation)

**Difficulty**: Medium

---

## 6. No MPU / pointer validation in syscalls

**Current behavior**: `syscall_dispatch()` casts `uint32_t` arguments directly
to pointers and dereferences them without any validation. There is no SVC
instruction and no MPU configuration: the `sys_*` wrappers call
`syscall_dispatch()` as an ordinary function, so a buggy user thread can
corrupt kernel memory by passing a bad pointer to `read`, `write`, or
`mq_send`.

**File:line**: `src/kernel/syscall.c:34` (`syscall_dispatch`)

**Better implementation**: Configure MPU regions for each process (read-only
flash, read-write stack+heap, no-access kernel). Validate pointer arguments in
the SVC handler before entering `syscall_dispatch`. On MPU fault, kill the
offending thread rather than crashing the kernel.

**SRAM impact**: None (security improvement; small code increase)

**Difficulty**: High

---

## 7. Synchronous console I/O

**Current behavior**: The shell polls `getchar_timeout_us(0)` (sleeping 1 ms
between polls) and writes with `printf`/`vprintf` (`shell_print`) directly,
blocking the calling thread for the entire duration of each USB CDC transaction.
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

## 8. Linear waiter scan in event flags

**Current behavior**: When `event_flags_set()` is called it scans a flat
`event_waiter_pool[MAX_EVENT_WAITERS]` array (sized `MAX_THREADS`) to find
threads waiting on the posted bits. This runs with interrupts disabled while
holding both the object's stripe lock and `event_pool_lock`, adding
O(MAX_THREADS) latency to every event post.

**File:line**: `src/kernel/sync.c:591` (`event_flags_set`), `src/kernel/sync.c:543`
(`event_waiter_pool`)

**Better implementation**: Embed a per-`event_flags_t` intrusive linked list of
waiters. `event_flags_set()` traverses only the threads actually waiting on that
specific event object, which is typically 1–3 threads, giving O(waiters) not
O(MAX_THREADS).

**SRAM impact**: None (pointer overhead per waiter; removes global pool)

**Difficulty**: Medium

---

## 9. Unimplemented device read/write (flash and GPIO via VFS)

**Current behavior**: the `read`/`write` handlers of the flash and GPIO devices
(`flash_read`, `flash_write`, `gpio_read`, `gpio_write`) are stubs that return
-1. TODO comments mark them as Phase 5 work. Opening `/dev/flash` or
`/dev/gpio` via VFS and calling `read`/`write` does nothing useful; only the
ioctls work (flash UID/geometry, GPIO direction/value).

**File:line**: `src/kernel/dev.c:175–192` (flash stubs),
`src/kernel/dev.c:237–252` (GPIO stubs)

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

## 10. Scan-result pointer getters share a live buffer with an IRQ

**Current behavior**: `wifi_get_scan_results()` and `bt_get_scan_results()`
return a pointer to the kernel's `g_scan[]` array and its current count, with
no lock and no copy. The array is written by the CYW43 async context, a
low-priority IRQ on core 0. A reader on core 1 runs in parallel with it, and a
reader on core 0 can be interrupted by it partway through a read. Either way
the reader can see a half-written SSID or name, a Classic device name being
filled in, or the buffer being reset by a new scan. Both getters are
deprecated and kept as an example of this bug.

**File:line**: `src/kernel/wifi.c` (`wifi_get_scan_results`),
`src/kernel/bluetooth.c` (`bt_get_scan_results`)

**Better implementation**: already provided as `wifi_copy_scan_results()` and
`bt_copy_scan_results()`, which copy the buffer while holding the async
context lock (`cyw43_arch_lwip_begin/end`). The writers also fill a slot
before publishing it by bumping the count. Exercises for students:
- Run `run scantest raw` and compare it with `run scantest`. Which checks catch
  the race, and which kinds of tearing does publish-after-fill already prevent?
- `cyw43_arch_lwip_begin()` is a recursive mutex owned by a *core*, not a
  thread. Two picoOS threads on the same core do not exclude each other
  through it. Find the places where that matters (lwIP calls from several
  threads) and fix them with a picoOS mutex taken around the lwIP lock.

**SRAM impact**: None. Callers need their own result buffer (704 B for WiFi,
about 1 KB for BT), which should be `static`.

**Difficulty**: Low

---

## 11. One continuous-scan subscriber per radio; killed lock holders

**Current behavior**: `wifi_scan_start()` / `bt_scan_start()` accept one
subscriber per radio and refuse a second with `*_ERR_BUSY`. Every window is a
single list handed over by a buffer swap, so there is nothing to share between
two readers. Separately, a thread killed while it holds a kernel `kmutex_t`
(including the scan-swap lock, held for a few instructions in
`*_scan_wait()`) never releases it, and the next thread to lock it blocks
forever.

**File:line**: `src/kernel/wifi.c` / `src/kernel/bluetooth.c`
(`*_scan_start`, `*_scan_wait`), `src/kernel/task.c` (`task_kill_process`)

**Better implementation**:
- Fan-out: give each subscriber its own held list (N+2 lists for N
  subscribers) and publish a reference-counted ready list that is recycled
  when the last subscriber swaps it out.
- Kill safety: have `task_kill_process()` defer the kill of a thread that
  owns a kernel mutex until it unlocks (a per-TCB "locks held" count), or
  release such mutexes as part of the kill.

**SRAM impact**: One extra list per subscriber (~700 B WiFi, ~1 KB BT).

**Difficulty**: Medium

---

## SRAM Impact Summary

None of the fixes above saves SRAM. Thread stacks are already `kmalloc`'d from
the 64 KB heap at creation and `kfree`'d on exit, so an unused TCB slot costs
only its TCB (about 72 bytes), and the TCB and PCB pools together are about
3 KB.

The large, adjustable items are:

| Item | SRAM | How to change it |
|------|------|------------------|
| Kernel heap | 64 KB | `HEAP_SIZE` in `mem.h` — limits how many threads (2–3 KB stacks) can exist at once |
| Display framebuffer | ~32 KB (Display Pack) / ~75 KB (Display Pack 2) | Build with `-DPICOOS_DISPLAY_ENABLE=OFF` |
| FS write buffer + superblock mirror | ~6 KB (RP2040) / ~8 KB (RP2350) | Fixed by `FS_BLOCK_SIZE` and `FS_MAX_FILES` |

Run `python3 tools/mem_report.py <map>` on a build's `.elf.map` for the current
numbers; a pico + Display Pack build at v0.3.4 has about 146 KB free.
