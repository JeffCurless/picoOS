# picoOS Outstanding Issues

Known bugs and loose ends in picoOS, as of v0.5.3 (2026-11-19). Every entry was
checked against the code at that version.

This is a different list from [imperfections.md](imperfections.md). An
imperfection is a deliberate simplification left for students to improve. An
issue here is something that misbehaves, or that is wrong or stale. A few
entries are both, and say so.

## How this list works

- **Numbers are permanent.** An issue keeps its number for life, and a number
  is never reused. New issues take the next free number (**#41**).
- **Severity:** *medium* means a user can hit it and it loses work or
  resources; *low* means narrow, cosmetic, or only reachable in unusual
  setups.
- **Fixing one:** put "Fixes #N" in the commit message, remove the entry from
  the tables below, and add a line to [Recently fixed](#recently-fixed).
- Items marked **L*n*** are findings from the locking review; their full write-up is
  in [locking.md](locking.md) under "Review findings and recommendations".

## Summary

| # | Severity | Area | Issue |
|---|---|---|---|
| 9 | Medium | bluetooth | One-shot `bt scan` drops Classic name requests |
| 10 | Medium | apps | Re-running `producer` strands a waiting `consumer` |
| 13 | Medium | sync | L2 ignored waiter-pool failure, L3 same-stripe nesting deadlock, L4 `spinlock_init` panic |
| 27 | Low–medium | kernel | A kill can hit an unrelated thread that reused a freed slot |
| 15 | Low | wifi | The first report of a network can store a bogus RSSI |
| 16 | Low | devices | `/dev/gpio` ioctls do not check the pin or the argument |
| 19 | Low | apps | Two `pi` runs at once share the same static state |
| 20 | Low | sync | L5 no owner check in `kmutex_unlock`, L6 queue copy with IRQs off, L8 unused functions |
| 22 | Low | kernel | Panic paths call `tud_task()` directly |
| 23 | Low | shell | `config.txt`: unknown button apps accepted, no trimming, 255-byte limit |
| 25 | Low | kernel | Idea: slab allocator for thread stacks |
| 28 | Low | kernel | `sched_next_thread` returns a stale thread when nothing is ready |
| 29 | Low | kernel | The first thread on each core is never reaped if it returns |
| 30 | Low | syscalls | `SYS_EXIT` / `thread_exit` disagree; `SYS_EXIT` walks threads unlocked; `SYS_PS` uses bare `\n` |
| 32 | Low | devices | `/dev/gpio` never initialises the pin and can drive reserved pins |
| 33 | Low | sync | Mutex/semaphore barging; no priority inheritance (teaching point) |
| 34 | Low | shell | `run <app> <arg>` leaks the argument; misleading "pool full" |
| 35 | Low | docs | Stale comments and docs (thread count, "boundary-tag" heap) |
| 36 | Low | size | Float printf and lwIP asserts still built in |
| 38 | Low | cleanup | Duplicated code in the shell and `sync.c` |
| 40 | Low | RAM | `scantest` keeps ~13 KB of static buffers when not running |

---

## Medium

### #9 — One-shot `bt scan` drops Classic name requests

**Where:** `name_request()`, `name_request_next()`, `packet_handler()` in
`src/kernel/bluetooth.c`.

Only one remote-name request can be outstanding. In continuous mode a busy
request is queued in the name cache and sent by `name_request_next()` when the
previous one completes. In one-shot mode (`bt scan`) `name_request_next()` is
never called, so a request that finds the controller busy is just dropped, and
those Classic devices show no name.

**Fix idea:** call `name_request_next()` on name completion in one-shot mode
too, or queue the pending addresses for the one-shot scan.

### #10 — Re-running `producer` strands a waiting `consumer`

**Where:** `demo_producer()` / `demo_ipc_init()` in `src/apps/demo.c`.

`demo_producer()` calls `demo_ipc_init()` every time it starts, and that calls
`mqueue_init()` on the shared queue. If a `consumer` is already blocked in
`mqueue_recv()`, re-initialising the queue clears its waiter list. The
consumer stays BLOCKED forever, and the new producer's messages never wake it.

**Fix idea:** initialise the queue once (a flag checked under a lock, or at
first use by either side), never while someone may be waiting on it.

### #13 — Locking findings L2, L3, L4

**Where:** `src/kernel/sync.c`; details in [locking.md](locking.md).

- **L2:** `event_flags_wait()` ignores the result of `event_waiter_alloc()`. If
  the pool is full, the waiter has no mask entry, so `event_flags_set()`
  treats its mask as 0 and it is never woken.
- **L3 (latent):** two blocking primitives whose locks map to the same
  hardware spinlock stripe (`prim_pool_assign()`) deadlock if one is taken
  while the other is held: the core spins on a lock it already owns, with IRQs
  off. No code does this today, but nothing prevents it.
- **L4:** `spinlock_init()` calls `spin_lock_claim_unused(true)`, which panics
  when the claim-free hardware spinlocks (IDs 24–31) run out.

**Fix ideas:** see the locking review. Moving each waiter's mask into `tcb_t`
removes the pool and fixes L2 (also part of #38).

### #27 — A kill can hit an unrelated thread that reused a freed slot (low–medium)

**Where:** `task_kill_process()` (`src/kernel/task.c`), `cmd_kill` /
`cmd_killproc` (`src/shell/shell.c`), `SYS_KILL` (`src/kernel/syscall.c`).

`task_kill_process()` copies the process's thread pointers under `task_lock`,
releases the lock, then kills each one. If a thread exits in between and a new
thread is created in the same TCB slot, the new, unrelated thread is killed.
The shell's `kill`/`killproc` and `SYS_KILL` have the same find-then-kill gap.

**Fix idea:** snapshot `(pointer, tid)` pairs and have the kill check the TID
under the lock before acting.

---

## Low

### #15 — The first report of a network can store a bogus RSSI

**Where:** `scan_result_cb()` in `src/kernel/wifi.c`.

The CYW43 firmware sometimes reports RSSI ≥ 0, which is not a real reading.
Repeat reports of a known BSSID filter it out (`if (r->rssi < 0)`), but the
first report is stored as is, so a network can show a positive RSSI until it
is heard again.

**Fix idea:** apply the same filter when a slot is first filled (store a floor
value, or skip the report).

### #16 / #32 — `/dev/gpio` checks nothing and initialises nothing

**Where:** `gpio_ioctl()` in `src/kernel/dev.c`.

- No range check on the pin number (`val & 0xFF`), and no NULL check on `arg`
  for any of the three ioctls.
- The pin is never `gpio_init()`ed, so its function is not set to SIO and
  `gpio_put()` may have no effect.
- Apps can drive pins the system owns: the Display Pack SPI, backlight and
  buttons, and the CYW43 radio pins on W boards.

**Fix idea:** check `arg`, refuse pins outside the board's range and a
reserved-pin mask, and `gpio_init()` the pin on first use.

### #19 — Two `pi` runs at once share the same static state

**Where:** `pi_args[]` and `pi_done` in `src/apps/pi.c`.

The worker arguments and the completion semaphore are file-scope statics. A
second `run pi` while the first is still running reinitialises them under the
first run's workers, mixing up their results and completion count.

**Fix idea:** allocate the state per run (kmalloc), or refuse a second
concurrent run.

### #20 — Locking findings L5, L6, L8

**Where:** `src/kernel/sync.c`; details in [locking.md](locking.md).

- **L5:** `kmutex_unlock()` does not check that the caller owns the mutex, so
  any thread can release it. A recursive `kmutex_lock()` blocks forever.
- **L6:** `mqueue` copies up to 64 bytes with IRQs off and a stripe held (a
  latency teaching point).
- **L8:** `spinlock_acquire()` / `spinlock_release()` (the variants that leave
  IRQs alone) have no callers.

### #22 — Panic paths call `tud_task()` directly

**Where:** `stack_overflow_panic()` and `lock_deadlock_panic()` in
`src/kernel/sched.c`.

Everywhere else, threads service USB through `dev_console_poll()`, which
serialises with the SDK. The two panic handlers pump `tud_task()` directly to
get their message out. The system is halted at that point, so the impact is
small.

### #23 — `config.txt` parsing gaps

**Where:** `shell_autorun_init()` and `shell_btn_init()` / `parse_btn_config()`
in `src/shell/shell.c`.

- `BUTTONx=` naming an app that is not in `app_table[]` is accepted at boot;
  pressing the button then silently does nothing.
- No key or value is trimmed, so `AUTORUN= pi` or a trailing space does not
  match.
- Only the first 255 bytes of the file are read.

### #25 — Idea: slab allocator for thread stacks

Stacks come from the general heap (`kmalloc`), so threads of different stack
sizes fragment it. A slab of fixed-size stack blocks would avoid that. This is
an improvement idea rather than a bug; it belongs in
[imperfections.md](imperfections.md) when written up.

### #28 — `sched_next_thread` returns a stale thread when nothing is ready

**Where:** `sched_next_thread()` in `src/kernel/sched.c`.

When no thread is eligible, it returns `current_tcb[core]`, which may be a
zombie that was just reaped (its TCB zeroed) or a BLOCKED thread. The comment
says the assembly then spins in WFI, but `isr_pendsv` (`sched_asm.S`) has no
such path: it resumes whatever TCB it is given, and a zeroed one loads
`saved_sp = NULL` and faults. Unreachable while the idle threads exist.

**Fix idea:** return the core's idle thread, and correct the comment.

### #29 — The first thread on each core is never reaped if it returns

**Where:** `sched_start()` / `sched_start_core1()` in `src/kernel/sched.c`.

The first thread on each core is entered by a direct call to `entry(arg)`, not
through an exception return, so its return address is not `thread_exit`. If it
returns, the core loops in WFI and the thread is never reaped. Today the first
thread is the highest-priority eligible one (the shell on core 0), and none of
the threads that can start first ever returns.

### #30 — `SYS_EXIT` and `thread_exit` disagree

**Where:** `SYS_EXIT` / `SYS_PS` in `src/kernel/syscall.c`, `thread_exit()` in
`src/kernel/task.c`.

- `SYS_EXIT` records `exit_code` and clears `alive` when the last thread
  exits; returning from the entry function (`thread_exit`) does neither.
- `SYS_EXIT` walks `proc->threads[]` without `task_lock`.
- `SYS_PS` prints with `printf` and bare `\n`, unlike the shell's `ps`.

### #33 — Mutex and semaphore barging (teaching point)

**Where:** `kmutex_lock()` / `ksemaphore_wait()` in `src/kernel/sync.c`.

A woken waiter must retry, and a thread arriving at the same moment can take
the mutex or count first; the woken thread then queues again at the tail. FIFO
order is not guaranteed and starvation is possible. There is no priority
inheritance either. Both are worth keeping as teaching points, but should be
listed in imperfections.md.

### #34 — `run <app> <arg>` leaks the argument

**Where:** `cmd_run()` in `src/shell/shell.c`; the apps in `src/apps/`.

`run` copies its argument to the heap and the app owns it. `pi`, `scantest`
and `hwtest` free it; `producer`, `consumer` and `sensor` ignore it and leak
it, and so does any app that is killed. Separately, when the `run` PID range
is full, `cmd_run` reports "cannot create process (pool full)".

**Fix idea:** free the argument in the launcher once the app has copied it, or
in the thread-exit path; report a full PID range as such.

### #35 — Stale comments and docs

- `src/kernel/sync.c` (event flags): "MAX_THREADS is small (16)"; it is 32.
- `docs/locking.md`: `event_waiter_pool[16]`; it is `MAX_THREADS` (32).
- `docs/imperfections.md` and `docs/design.md` call the heap "boundary-tag".
  Blocks have only a header, no footer: `kfree()` walks the list from the
  start to merge with the previous block, O(n), with IRQs off, and it also
  runs from PendSV (when a zombie is reaped).

### #36 — Float printf and lwIP asserts still built in

**Where:** build configuration (`src/CMakeLists.txt`, `src/lwipopts.h`).

The build uses `-Os` since 0.5.0. Two measured savings remain:

| Change | Saves |
|---|---|
| `PICO_PRINTF_SUPPORT_FLOAT=0`, `PICO_PRINTF_SUPPORT_EXPONENTIAL=0` (nothing in `src/` prints floats) | ~3.8 KB flash |
| `LWIP_NOASSERT` (lwIP assert messages and checks) | ~6.5 KB of strings plus code |

`LWIP_TCP` stays on: other projects built on picoOS use TCP.

### #38 — Duplicated code

- `src/shell/shell.c`: `shell_run()` repeats `shell_readline()`; app launch is
  written three times (`run`, `AUTORUN=`, `BUTTONx=`); `config.txt` is read
  and parsed twice; `fs write` and `fs append` each join their arguments.
- `src/kernel/sync.c`: each of the five `*_dbg` variants copies its blocking
  loop (debug builds only).
- The event-waiter pool could live in `tcb_t`, which removes
  `event_pool_lock` (one hardware spinlock) and fixes L2 (#13).

### #40 — `scantest` keeps ~13 KB of static buffers

**Where:** `s_buf` and the `tmp` buffers in `src/apps/scantest.c` (W builds).

About 13 KB of BSS is reserved even when `scantest` is not running. That
matters on Pico W + Display Pack 2 builds, which have about 26 KB of SRAM
left. Allocating the buffers when `scantest` starts would return it.

---

## Recently fixed

| # | Issue | Version | Commit |
|---|---|---|---|
| 5 | A thread killed while blocked leaked if its primitive was never signalled | 0.5.2 | (pending) |
| 7 | Names of 16+ characters stored cut short, duplicated on each create | 0.5.1 | 240f197 |
| 8 | fs fd of a deleted file freed under the VFS, aliasing the next file | 0.5.1 | 240f197 |
| 18 | `O_TRUNC` + close without a write: size 0 in RAM only | 0.5.1 | 240f197 |
| 21 | `docs/testing.md` single-writer note stale | 0.5.1 | 240f197 |
| 31 | Readers saw a pending write's size; fd checks outside the lock; superblock lacked `max_files` | 0.5.1 | 240f197 |
| 37 | Dead VFS fields, `sched_tick()`, count functions, unreachable `kill` branch | 0.5.1 | 240f197 |
| 39 | 127-char line lost its newline; full-sector programs; format erased every sector | 0.5.1 | 240f197 |
| 36 (part) | Build switched from `-O3` to `-Os` | 0.5.0 | e45f691 |
