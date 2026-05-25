# Testing Guide

picoOS ships four host-native test suites that compile kernel source files directly against the host toolchain — no Pico hardware, no Pico SDK, and no cross-compiler needed.  Every suite is registered with CTest so the whole collection runs in one command.

---

## Running the tests

### Quickest path (from the project root)

```bash
./build tests
```

This configures `tests/build/` with CMake (if not already done), compiles all four test binaries, and runs them via CTest.

### Manual path (from the `tests/` directory)

```bash
cd tests
cmake -B build
make -C build
ctest --test-dir build --output-on-failure
```

### Cleaning

```bash
./clean        # removes tests/build as well as all firmware build directories
# or just the test build:
rm -rf tests/build
```

---

## How host compilation works

The kernel modules under test (`mem.c`, `sync.c`, `fs.c`, `vfs.c`) are compiled directly by the host `gcc` toolchain.  Hardware dependencies are handled in two ways:

| Dependency | How it is satisfied |
|---|---|
| ARM CMSIS intrinsics (`__dmb`, `__disable_irq`, …) | No-op `static inline` stubs in `src/kernel/arch.h` (the `#else` host-stub block already present for LSP support) |
| `time_us_64()` | Returns real wall-clock time via `clock_gettime(CLOCK_MONOTONIC)` — this is required so the `PICOOS_LOCK_DEBUG` spinlock timeout test can actually expire |
| Flash hardware (`XIP_BASE`, `flash_range_erase`, `flash_range_program`) | `HOST_TEST` guard in `arch.h` redirects these to `tests/fs/mock_flash.c`, which operates on a `malloc`-backed RAM buffer |
| Multicore lockout | No-op stubs already in `arch.h` |
| Scheduler (`sched_block`, `sched_unblock`, `sched_yield`, `current_tcb[2]`) | Minimal stubs in `tests/sync/mock_sched.c`; `current_tcb` is a 2-element array matching the SMP layout (`current_tcb[0]` is a static dummy `tcb_t` with `tid = 1`; `current_tcb[1]` is `NULL`); the yield stub also fires a one-shot `mock_yield_hook` callback used by deadlock tests |
| `lock_deadlock_panic()` | Test stub in `mock_sched.c` captures panic arguments and `longjmp`s back to the test instead of halting |
| Device layer (`dev_*`) | Call-counting stubs in `tests/vfs/mock_dev.c` |
| Filesystem layer (`fs_*`) for VFS tests | Call-counting stubs in `tests/vfs/mock_fs.c` |

---

## Test suites

### 1 — `test_mem` (14 tests) — heap allocator

**Source**: `tests/mem/test_mem.c`  
**Module under test**: `src/kernel/mem.c`  
**Dependencies**: `src/kernel/sync.c` (heap spinlock), `tests/sync/mock_sched.c` (scheduler stubs)

`mem.c` uses the SMP-safe heap spinlock from `sync.c`, which in turn needs scheduler stubs.  No hardware mocking is required beyond those stubs.  Each test calls `kmem_init()` to reset the heap, and most capture the initial `kmem_stats()` values to verify the heap is completely restored after all allocations are freed.

| Test | What it verifies |
|---|---|
| `zero_size_returns_null` | `kmalloc(0)` must return NULL |
| `null_free_is_safe` | `kfree(NULL)` is a no-op; heap state unchanged |
| `single_byte_allocation` | 1-byte alloc succeeds, is writable, heap restored |
| `pointer_alignment_8_bytes` | Every returned pointer is 8-byte aligned regardless of requested size |
| `over_heap_size_returns_null` | `kmalloc(HEAP_SIZE + 1)` returns NULL without corruption |
| `max_single_allocation` | Allocate the entire heap payload in one block; write end-to-end; heap restored on free |
| `heap_exhaustion_and_restore` | Fill heap with 64-byte blocks; verify next alloc fails; free all; heap restored |
| `write_patterns_no_corruption` | 14 preset sizes live simultaneously; byte-fill patterns intact across all allocations |
| `random_size_allocations` | 512 random-size blocks (fixed seed); patterns verified; heap restored |
| `external_fragmentation` | Fill heap with 8-byte blocks; free alternating blocks; 16-byte alloc fails; free all; heap restored |
| `reverse_free_order_coalescing` | Free 64 blocks in reverse order; exercises backward O(n) coalesce path; heap restored |
| `repeated_alloc_free_cycles` | 1 000 alloc-then-free cycles of the same block; no fragmentation accumulation |
| `interleaved_alloc_free_mixed_sizes` | 5 different sizes freed out of order; exercises both forward and backward coalescing |
| `alignment_boundary_sizes` | Sizes 1–513 straddling every 8-byte boundary; per-size heap restore verified |

---

### 2 — `test_sync` (37 tests) — synchronisation primitives

**Source**: `tests/sync/test_sync.c`  
**Module under test**: `src/kernel/sync.c`  
**Mock**: `tests/sync/mock_sched.c`  
**Compile definitions**: `PICOOS_LOCK_DEBUG=1`, `PICOOS_LOCK_TIMEOUT_MS=100`

The mock provides `current_tcb[2]` — a 2-element array matching the SMP kernel layout, where `current_tcb[0]` points to a static dummy `tcb_t` with `tid = 1` and `current_tcb[1]` is `NULL` — and stub implementations of `sched_block` (sets `THREAD_BLOCKED`), `sched_unblock` (sets `THREAD_READY`), and `sched_yield` (fires the `mock_yield_hook` callback once if set, then clears it).

The mock also provides `lock_deadlock_panic()`, which instead of halting captures the panic arguments into public globals (`mock_lock_panic_type`, `mock_lock_panic_wait_tid`, `mock_lock_panic_hold_tid`, …) and `longjmp`s back to the test's `setjmp` recovery point.

**Scope of non-debug tests**: Because `sched_yield` would otherwise be a no-op, the core sync tests exercise only the **non-blocking fast paths**.  The deadlock detection tests use `mock_yield_hook` to simulate a second thread releasing a lock, allowing the blocked path to complete in a single-threaded host process.

#### Spinlock (5 tests)

| Test | What it verifies |
|---|---|
| `spinlock_default_state_is_unlocked` | Zero-initialised spinlock has `lock == 0` |
| `spinlock_acquire_sets_lock_to_1` | `acquire` sets `lock` to 1 |
| `spinlock_release_clears_lock_to_0` | `release` clears `lock` to 0 |
| `spinlock_irq_acquire_release_round_trip` | IRQ-save variant sets and clears `lock` correctly |
| `spinlock_reacquire_after_release` | `acquire → release → acquire` succeeds |

#### Mutex (4 tests)

| Test | What it verifies |
|---|---|
| `mutex_init_state` | `owner_tid == -1`, `count == 0`, `waiters == NULL`, `spin.lock == 0` after init |
| `mutex_lock_when_free` | Lock a free mutex; `owner_tid` becomes `current_tcb->tid`; `count == 1` |
| `mutex_unlock_clears_owner` | Unlock after lock; `owner_tid == -1`, `count == 0` |
| `mutex_relock_after_unlock` | Lock → unlock → lock again sets `owner_tid` again |

#### Semaphore (5 tests)

| Test | What it verifies |
|---|---|
| `semaphore_init_count` | `count` equals the initial value after init |
| `semaphore_signal_increments_count` | Each `signal` increments `count` by 1 |
| `semaphore_wait_decrements_count` | `wait` when `count > 0` decrements `count` by 1 |
| `semaphore_signal_then_wait_restores_count` | `signal` then `wait` returns count to its starting value |
| `semaphore_multi_signal_multi_wait` | N signals followed by N waits returns count to 0 |

#### Event flags (8 tests)

| Test | What it verifies |
|---|---|
| `event_flags_init_zero` | `flags == 0`, `waiters == NULL` after init |
| `event_flags_set_individual_bits` | Bits are OR'd in; unrelated bits are unaffected |
| `event_flags_clear_bits` | Cleared bits become 0; other bits are preserved |
| `event_flags_set_does_not_clear_other_bits` | Two consecutive `set` calls produce the union of both masks |
| `event_flags_all_32_bits_set_and_clear` | All 32 bits can be set and cleared independently |
| `event_flags_wait_any_condition_already_met` | `wait(any)` returns immediately when at least one mask bit is set |
| `event_flags_wait_all_condition_already_met` | `wait(all)` returns immediately when every mask bit is set |
| `event_flags_wait_any_partial_match_satisfied` | `wait(any)` with one of three mask bits set returns immediately |

#### Message queue (8 tests)

| Test | What it verifies |
|---|---|
| `mqueue_init_state` | `count`, `head`, `tail` all 0; `msg_size` matches request; `send_waiters` and `recv_waiters` NULL |
| `mqueue_size_clamped_to_MQ_MSG_SIZE` | Requesting a size larger than `MQ_MSG_SIZE` clamps to `MQ_MSG_SIZE` |
| `mqueue_send_recv_single_message` | One message round-trip; count goes 0→1→0; bytes match |
| `mqueue_fifo_order_preserved` | Four messages received in the same order sent |
| `mqueue_fill_to_MQ_MAX_MSG_capacity` | `MQ_MAX_MSG` sends succeed; `count == MQ_MAX_MSG` |
| `mqueue_drain_to_empty` | Fill then drain; count returns to 0 |
| `mqueue_message_content_integrity` | All `MQ_MAX_MSG` unique payloads match exactly on receive |
| `mqueue_interleaved_send_recv` | Alternating sends and receives preserve order and empty the queue |

#### Deadlock detection — `PICOOS_LOCK_DEBUG` (7 tests)

These tests verify the instrumentation added to `sync.c` when `PICOOS_LOCK_DEBUG` is defined.  They are compiled into the same `test_sync` binary via the `PICOOS_LOCK_DEBUG=1` definition in `tests/CMakeLists.txt`; they are omitted from non-debug builds automatically via `#ifdef`.

`PICOOS_LOCK_TIMEOUT_MS` is set to **100 ms** for the test build.  This keeps the spinlock timeout test fast (the spin loop busy-waits for 100 ms of wall-clock time before the panic fires) while remaining reliable on slow machines.

| Test | What it verifies |
|---|---|
| `lock_debug_mutex_init_zeros_all_debug_fields` | `kmutex_init()` zeroes all debug fields: spinlock `acq_file/line/tid` and mutex `acq_file/line/time_us` |
| `lock_debug_semaphore_init_zeros_spinlock_debug_fields` | `ksemaphore_init()` zeroes the embedded spinlock's debug fields |
| `lock_debug_spinlock_records_and_clears_holder_tid` | `spinlock_irq_acquire` sets `acq_tid` to current TID; `spinlock_irq_release` clears it to -1 and nulls `acq_file` |
| `lock_debug_mutex_sets_acq_fields_on_lock_and_clears_on_unlock` | `kmutex_lock_dbg(m, file, line)` records `acq_file`, `acq_line`, and a non-zero `acq_time_us`; `kmutex_unlock` clears all three |
| `lock_debug_kmutex_lock_macro_calls_dbg_variant` | The `kmutex_lock(m)` macro expands to `kmutex_lock_dbg(m, __FILE__, __LINE__)` and produces a non-NULL `acq_file` |
| `lock_debug_mutex_dbg_sets_tcb_blk_fields_before_sched_block` | When a mutex is held by another thread, the `_dbg` variant sets `blk_time_us`, `blk_file`, `blk_line`, `blk_what == "mutex"`, and `blk_holder_tid` on the TCB before calling `sched_block()`; fields are cleared after the lock is acquired on retry |
| `lock_debug_spinlock_timeout_calls_deadlock_panic` | When `spinlock_irq_acquire` spins on a held lock for `PICOOS_LOCK_TIMEOUT_MS`, it calls `lock_deadlock_panic("spinlock", …)` with the correct waiter TID and holder TID; the test catches the panic via `setjmp`/`longjmp` |

---

### 3 — `test_fs` (17 tests) — flash filesystem

**Source**: `tests/fs/test_fs.c`  
**Module under test**: `src/kernel/fs.c`  
**Mock**: `tests/fs/mock_flash.c`

The flash mock provides a `malloc`-backed buffer sized `(1 + FS_MAX_FILES) × 4 KB`, initialised to `0xFF` (erased state).  `flash_range_erase` does `memset` to `0xFF`; `flash_range_program` does `memcpy`.  `host_xip_base` is set so that XIP pointer arithmetic in `fs.c` (`XIP_BASE + FS_FLASH_OFFSET + offset`) resolves to the correct position in the RAM buffer.

The suite uses `FS_MAX_FILES=8` (set at compile time) to keep the RAM buffer under 40 KB.

Each test calls `fs_reset()` (helper in the test file) which calls `mock_flash_init()` to wipe the buffer back to `0xFF`, then `fs_format()` to write a fresh superblock.

| Test | What it verifies |
|---|---|
| `format_creates_valid_superblock` | Raw flash contains magic `0x50494353`, version `1`, file_count `0` after format |
| `format_idempotent` | Calling `fs_format()` twice produces the same valid empty filesystem |
| `corrupt_superblock_triggers_reformat` | Writing bad magic to flash then calling `fs_init()` triggers a reformat; all files are gone |
| `create_file_persists_metadata_across_reinit` | Create and close a file; call `fs_init()` to re-read flash; file is still listed |
| `write_read_small` | Write 32 bytes, close, reopen for read; exact bytes returned |
| `write_read_full_sector` | Write exactly `FS_MAX_FILE_DATA` (4 096) bytes; full round-trip matches |
| `write_over_max_file_size_fails` | Writing past the 4 KB per-file cap returns -1 |
| `read_at_eof_returns_zero` | Reading past end-of-file returns 0, not an error code |
| `delete_removes_file_from_directory` | `fs_delete` removes the file; directory listing is empty |
| `delete_reclaims_directory_slot` | Creating a file with a previously deleted name succeeds |
| `max_files_limit` | Creating `FS_MAX_FILES` files all succeed; creating one more returns -1 |
| `file_name_at_max_length_boundary` | A name of `FS_NAME_MAX - 1` characters stores and is found correctly |
| `trunc_clears_file_content` | Write data; reopen with `O_TRUNC` and close without writing; file size is 0 |
| `append_extends_file` | Write N bytes, close; reopen with `O_APPEND`; write M bytes; total size is N+M and content is contiguous |
| `write_to_rdonly_fd_fails` | `fs_write` on a read-only fd returns -1 |
| `scratch_owner_single_writer_enforcement` | A second write-mode open while the first is still open returns -1; after the first is closed (with data written so dirty=true) the next write open succeeds |
| `reopen_resets_position_to_zero` | Closing and reopening a file for read starts at byte 0 regardless of how far the previous fd read |

> **Note on single-writer behaviour**: `fs.c` uses a single 4 KB scratch buffer for all writes.  The scratch buffer is released on `fs_close()` only when `dirty == true` (i.e. at least one `fs_write` was called on that fd).  Opening a file for write and closing it without writing any data does not release the buffer — this is a documented teaching imperfection.  Tests that verify write-open lifecycle therefore write at least one byte before closing.

---

### 4 — `test_vfs` (10 tests) — VFS routing layer

**Source**: `tests/vfs/test_vfs.c`  
**Module under test**: `src/kernel/vfs.c`  
**Mocks**: `tests/vfs/mock_dev.c`, `tests/vfs/mock_fs.c`

The device mock (`mock_dev.c`) returns success for all operations and exposes `mock_dev_open_calls`, `mock_dev_close_calls`, and `mock_last_dev_opened` counters so tests can verify VFS dispatching.  The filesystem mock (`mock_fs.c`) returns incrementing fd values from `fs_open` and exposes `mock_fs_open_calls` and `mock_fs_close_calls`.

Each test calls `vfs_reset()` (helper in the test file) which calls `vfs_init()` to re-register the four standard device mounts (`/dev/console`, `/dev/timer`, `/dev/flash`, `/dev/gpio`) and resets both mock call counters.

| Test | What it verifies |
|---|---|
| `dev_open_calls_device_layer` | Opening `/dev/console` invokes `dev_open` with `DEV_CONSOLE` |
| `file_open_calls_fs_layer` | Opening a non-device path invokes `fs_open` |
| `fd_table_exhaustion` | Opening `VFS_MAX_OPEN` fds all succeed; the next returns -1 |
| `close_reclaims_fd_slot` | Closing one of a full table of fds allows another open to succeed |
| `invalid_fd_read_write_close_return_minus_one` | `fd = -1` and `fd = VFS_MAX_OPEN` all return -1 for read, write, and close |
| `null_path_open_returns_minus_one` | `vfs_open(NULL, …)` returns -1 |
| `double_close_second_returns_minus_one` | Closing the same fd twice: second close returns -1 |
| `dev_close_calls_device_layer` | `vfs_close` on a device fd increments `mock_dev_close_calls` |
| `file_close_calls_fs_layer` | `vfs_close` on a file fd increments `mock_fs_close_calls` |
| `mount_table_exhaustion` | Adding mounts up to `VFS_MAX_DEV_MOUNTS` succeeds; one more returns -1 |

---

## Test file layout

```
tests/
├── CMakeLists.txt          Standalone host CMake project; four add_test() targets
├── compile_flags.txt       clangd include-path hint (editor LSP only)
├── framework.h             Minimal BEGIN_TEST / CHECK / END_TEST / SUMMARY macros
├── mem/
│   └── test_mem.c          Heap allocator tests
├── sync/
│   ├── mock_sched.c        Scheduler stubs (current_tcb, sched_block/unblock/yield,
│   │                       mock_yield_hook, lock_deadlock_panic for PICOOS_LOCK_DEBUG)
│   └── test_sync.c         Sync primitive tests + deadlock detection tests
├── fs/
│   ├── mock_flash.h        RAM-backed flash interface
│   ├── mock_flash.c        malloc buffer + flash_range_erase/program implementations
│   └── test_fs.c           Filesystem tests
└── vfs/
    ├── mock_dev.h / .c     Device-layer call-counting stubs
    ├── mock_fs.h / .c      Filesystem-layer call-counting stubs
    └── test_vfs.c          VFS routing tests
```

---

## Lock deadlock detection (`PICOOS_LOCK_DEBUG`)

The `PICOOS_LOCK_DEBUG` build option adds timeout-based deadlock detection to every blocking lock primitive.  It is **off by default** in firmware builds and **on** in the `test_sync` host build.

### Enabling in a firmware build

```bash
# Build the pico_w + Display Pack variant with deadlock detection:
cmake -B build_picow_D \
      -DPICO_BOARD=pico_w \
      -DPICOOS_DISPLAY_ENABLE=ON \
      -DPICOOS_LOCK_DEBUG=ON \
      -DPICOOS_LOCK_TIMEOUT_MS=5000   # optional — default is 5000 ms
make -j$(nproc) -C build_picow_D
```

When any lock wait exceeds `PICOOS_LOCK_TIMEOUT_MS`, the system prints:

```
!!! DEADLOCK DETECTED !!!
  Lock     : mutex
  Waiting  : TID 3 "shell"  at shell/shell.c:156  (> 5000 ms)
  Holder   : TID 1 "wifi-poll"  at kernel/sync.c:142
System halted. Reboot required.
```

and pumps USB for ~500 ms before halting (so the message reaches the host terminal).

### How it works

| Mechanism | Covers |
|---|---|
| **Spinlock spin-loop timeout** | `spinlock_irq_acquire` and `spinlock_acquire` record a `time_us_64()` deadline before entering the spin loop and call `lock_deadlock_panic()` if it expires |
| **SysTick BLOCKED-thread scanner** | Every 1 ms SysTick checks all `THREAD_BLOCKED` threads; if any has `blk_time_us` set and has waited longer than the threshold, it sets a victim pointer and pends PendSV |
| **`sched_next_thread()` check** | Called from PendSV with PRIMASK set (same environment as `stack_overflow_panic`); reads the victim pointer and calls `lock_deadlock_panic()` before returning |

### Per-call-site information

When `PICOOS_LOCK_DEBUG` is defined, the public lock API is replaced by macros that inject `__FILE__` and `__LINE__` at each call site:

```c
// sync.h (debug build only):
#define kmutex_lock(m)   kmutex_lock_dbg(m, __FILE__, __LINE__)
// etc. for ksemaphore_wait, event_flags_wait, mqueue_send, mqueue_recv
```

Before calling `sched_block()`, each `_dbg` variant records on the blocking thread's TCB:

| Field | Contents |
|---|---|
| `blk_time_us` | Absolute timestamp when blocking began (`time_us_64()`) |
| `blk_file` / `blk_line` | Source location of the blocking lock call |
| `blk_what` | Lock type tag: `"mutex"`, `"semaphore"`, `"event_flags"`, `"mqueue_send"`, `"mqueue_recv"` |
| `blk_holder_tid` | TID of the current lock holder at block time |
| `blk_holder_file` / `blk_holder_line` | Where the holder acquired the lock |

These fields are cleared when the thread is unblocked and re-acquires the lock.

---

## Adding a new test

### Adding a test case to an existing suite

1. Write a `static void test_<name>(void)` function in the relevant `test_*.c` file.
2. Use `BEGIN_TEST(<name>)` at the top and `END_TEST()` at the bottom.
3. Assert conditions with `CHECK(condition, "failure message")`.
4. Add the function call to `main()` before `SUMMARY()`.

```c
static void test_my_new_case(void)
{
    BEGIN_TEST(my_new_case);
    kmem_init();

    void *p = kmalloc(128);
    CHECK(p != NULL, "128-byte alloc must succeed");
    kfree(p);

    END_TEST();
}
```

### Adding a new test suite

1. Create a `tests/<subsystem>/` directory.
2. Write a `test_<subsystem>.c` file following the pattern above.
3. Write any mock/stub files needed to satisfy hardware or scheduler dependencies.
4. Add a new `add_executable` + `add_test` block to `tests/CMakeLists.txt` following the existing pattern.

When `HOST_TEST` compile-time dependencies are needed (e.g., redirecting XIP reads to a RAM buffer), add them via `target_compile_definitions` in `CMakeLists.txt` rather than modifying production source files.
