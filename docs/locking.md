# picoOS Locking Mechanisms

As of 2026-10-02 (commit 02ea2b3)

## Overview

picoOS locks in two layers: a few RP2040 hardware spinlocks give short, cross-core atomic sections, and every blocking primitive (mutex, semaphore, event flags, message queue) is a plain memory structure guarded by one of those hardware locks. As of commit 02ea2b3, the blocking primitives no longer claim a hardware spinlock each. They share a pool of 4 striped hardware locks, so there is no limit on how many of them can be created.

The one remaining exhaustion path is `spinlock_init()` itself. Each call claims a new hardware lock from the SDK's 8-slot claim-free range and panics when that range is empty.

| Layer | Type | Where | Holds a HW spinlock? | Blocks or spins |
| --- | --- | --- | --- | --- |
| Hardware | `spin_lock_t` (SIO register) | RP2040 silicon | is one | spins |
| Kernel spinlock | `spinlock_t` | `src/kernel/sync.c:190` | dedicated (claimed) or aliased to a stripe | spins, IRQs off |
| Scheduler lock | `sched_lock` | `src/kernel/sched.c:97` | dedicated | spins |
| Heap lock | `heap_lock` | `src/kernel/mem.c:60` | dedicated | spins |
| Event waiter pool | `event_pool_lock` | `src/kernel/sync.c:80` | dedicated | spins |
| Mutex | `kmutex_t` | `src/kernel/sync.c:351` | stripe from `prim_pool` | blocks |
| Semaphore | `ksemaphore_t` | `src/kernel/sync.c:451` | stripe from `prim_pool` | blocks |
| Event flags | `event_flags_t` | `src/kernel/sync.c:583` | stripe from `prim_pool` | blocks |
| Message queue | `mqueue_t` | `src/kernel/sync.c:721` | stripe from `prim_pool` | blocks |

## RP2040 hardware spinlocks

The RP2040 SIO block has 32 spinlock registers, and only 8 of them can be handed out on demand. Cortex-M0+ has no LDREX/STREX, so these registers are the only atomic test-and-set the two cores share.

- **Acquire:** read `SPINLOCKn`. A non-zero result means this core now owns it; zero means it was already held.
- **Release:** write any value to `SPINLOCKn`.
- **No owner, no nesting:** the hardware does not record which core holds the lock. A core that reads its own held lock again gets zero and spins forever.
- **IRQs are separate:** a held hardware lock does not mask interrupts. `spin_lock_blocking()` disables IRQs first, then spins, then issues a `dmb`.

| IDs | SDK use | Can picoOS run out? |
| --- | --- | --- |
| 0–13 | Reserved for the SDK (IRQ, timer, claim bitmap, etc.) | No, not ours |
| 14–15 | `PICO_SPINLOCK_ID_OS1/OS2`, reserved for an OS | No: picoOS claims both for `prim_pool[0..1]` |
| 16–23 | SDK striped pool for `mutex_init`, `sem_init`, `critical_section_init` (lwIP, BTstack, async_context) | No: shared, never claimed |
| 24–31 | Claim-free range for `spin_lock_claim_unused()` | **Yes**: 8 slots, panics when empty |

picoOS currently takes 5 of the 8 claim-free slots: `heap_lock`, `sched_lock`, `event_pool_lock` and `prim_pool[2..3]`. The remaining 3 are shared with anything else in the build that calls `spin_lock_claim_unused()`. The shell `info` command shows the live count through `sync_spinlock_report()`.

## Kernel spinlock (`spinlock_t`)

`spinlock_t` is a thin wrapper around one hardware lock pointer (`hw`) plus a software word (`lock`) used only before a hardware lock is attached. It has two modes, picked by whether `hw` is NULL.

| Call | `hw != NULL` (normal) | `hw == NULL` (early boot fallback) |
| --- | --- | --- |
| `spinlock_irq_acquire()` | `spin_lock_blocking(hw)`: IRQs off, spin on the register, return saved IRQ state | IRQs off, spin on `lock`, set it to 1 |
| `spinlock_irq_release()` | `spin_unlock(hw, saved)`: write the register, restore IRQs | clear `lock`, restore IRQs |
| `spinlock_acquire()` | spin on `*hw` with IRQs untouched, then `dmb` | spin on `lock` (single core only) |
| `spinlock_release()` | `spin_unlock_unsafe(hw)` | clear `lock` |

There are three ways a `spinlock_t` gets its `hw` pointer:

1. `spinlock_init()` (`sync.c:190`) claims a fresh lock with `spin_lock_claim_unused(true)`. This is the call that can exhaust the 24–31 range.
2. `claim_spinlock_by_id()` (`sync.c:97`) claims a fixed ID. Used only for OS1/OS2.
3. `prim_pool_assign()` (`sync.c:117`) copies the `hw` pointer of a pool stripe. Nothing new is claimed.

Every kernel path uses the IRQ variant. That matters because PendSV (the context switch) is an interrupt: with IRQs off, the current thread cannot be switched out while it holds a hardware lock, so the other core never waits on a lock whose owner is asleep.

Lock order, from `sched.c:90`: a primitive's stripe first, then `event_pool_lock`, then `sched_lock`, then `heap_lock`. No path takes them in reverse.

## Blocking primitives

All four blocking primitives follow the same pattern: the hardware lock guards a few words of state for a few dozen cycles, and a thread that must wait is parked on a FIFO waiter list instead of spinning. The waiter list is threaded through `tcb_t.next`, which is safe because a BLOCKED thread is not on any ready queue.

The wait path, shared by `kmutex_lock`, `ksemaphore_wait`, `event_flags_wait`, `mqueue_send` and `mqueue_recv`:

1. `spinlock_irq_acquire(&obj->spin)`: IRQs off, stripe held.
2. Test the condition (owner free, count > 0, flags match, queue not full/empty). If it holds, update state, release, return.
3. Otherwise `waiter_enqueue()` the current TCB.
4. `sched_block()`: take `sched_lock`, set BLOCKED, remove from the ready queue.
5. `spinlock_irq_release()`: stripe released, IRQs back on.
6. `sched_yield()`: pend PendSV; the context switch happens here.
7. When woken, loop back to step 1 and re-test. Wakeups are hints, not hand-offs.

The wake path (`kmutex_unlock`, `ksemaphore_signal`, `event_flags_set`, the other side of a queue) takes the same stripe, changes state, `waiter_dequeue()`s the first waiter and calls `sched_unblock()`, which sets it READY and appends it to its ready queue.

| Primitive | State under the lock | Waiter lists | Notes |
| --- | --- | --- | --- |
| `kmutex_t` | `owner_tid`, `count` | 1 | Non-recursive; unlock does not check the caller is the owner; no priority inheritance; a woken waiter can lose the race to a new caller |
| `ksemaphore_t` | `count` | 1 | Count never goes negative here, despite the header comment saying it does |
| `event_flags_t` | `flags` | 1, plus a global `event_waiter_pool[16]` holding each waiter's mask | `event_flags_set` nests `event_pool_lock` inside the stripe and wakes every satisfied waiter |
| `mqueue_t` | ring of 16 × 64 B, `head`, `tail`, `count` | 2 (senders, receivers) | `memcpy` of up to 64 B runs with IRQs off; `try_` variants never block |

CLAUDE.md lists pipes and ring buffers under sync, but there is no pipe type in `sync.h` today.

## Spinlock sharing and the exhaustion problem

The kernel can no longer exhaust hardware spinlocks on its own; only a direct `spinlock_init()` call from application or driver code still can. The fix arrived in three steps.

| Commit | Change | Effect |
| --- | --- | --- |
| 02ea2b3 | All four primitive types draw from `prim_pool[4]`; stripes 0–1 use OS1/OS2 | Unlimited mutexes, semaphores, event flags and queues |
| aa38e26 | Semaphores, event flags and queues share one HW lock | Fixed the halt, but each `kmutex_t` still claimed its own |
| 9a4ac10 | Each primitive claims its own HW lock | A handful of objects panics `spin_lock_claim_unused(true)` and halts silently |

A primitive's stripe is chosen from its address: `idx = (addr >> 4) % 4` (`sync.c:119`). No counter or lock is needed to pick it, and objects spread across stripes. Two unrelated objects on the same stripe only contend for the few cycles their state updates take.

```
 Blocking primitives          prim_pool[4]          RP2040 HW spinlock IDs
 (any number of objects,
  no HW lock each)
 +----------------+         +------------+        +-----------------------------+
 | kmutex_t       |         | stripe 0   |------->| OS1, ID 14 (never exhausts) |
 | ksemaphore_t   |  addr   | stripe 1   |------->| OS2, ID 15 (never exhausts) |
 | event_flags_t  |-------->| stripe 2   |---+    +-----------------------------+
 | mqueue_t       |  hash   | stripe 3   |-+ |    | Claim-free IDs 24-31: 8     |
 +----------------+         +------------+ | +--->|   prim_pool[2]   heap_lock  |
                                           +----->|   prim_pool[3]   sched_lock |
                                                  |   event_pool_lock  3 free   |
                                                  +-----------------------------+
                                                                ^
       spinlock_init()  -- each call takes a free slot ---------+
       (public in sync.h; panics when no slot is left)
```

Only the `spinlock_init()` path can fail: every primitive lands on one of 4 stripes, while each direct `spinlock_init()` call consumes one of the 3 remaining claim-free slots.

Two consequences of sharing to keep in mind:

- **Never hold two primitives' stripes at once.** If object A and object B map to the same stripe, taking B's lock while holding A's reads a register this core already owns, and the core spins forever with IRQs off. No current code nests two primitive locks, but nothing prevents a future change from doing so.
- **`spinlock_init()` is still public.** It is declared in `sync.h`, and each call claims a new lock from IDs 24–31. An application that creates its own `spinlock_t` for each object it protects will reach the 8-slot limit and halt, which is the gap the proposal below targets.

## Proposal: an application spinlock backed by memory words

The idea is sound and worth building. It closes the last exhaustion path, and done right it also removes the same-stripe nesting deadlock. It is the spinning counterpart of what `prim_pool` already does for the blocking primitives: the lock state lives in RAM, and a hardware lock only makes the test-and-set on that RAM atomic across cores.

The key rule is to hold the hardware lock only long enough to test and set the word, never while waiting. A waiter that cannot get the word releases the stripe and tries again. Because no core ever holds a stripe while waiting for another lock, two application locks on the same stripe can be nested safely.

```c
typedef struct {
    spin_lock_t      *hw;     /* stripe, from prim_pool_assign() */
    volatile int32_t  owner;  /* -1 = free, else (core << 16) | tid */
} aspinlock_t;

uint32_t aspin_lock(aspinlock_t *l)
{
    int32_t me = ((int32_t)get_core_num() << 16) | (int32_t)CURRENT_TCB->tid;
    for (;;) {
        uint32_t irq = spin_lock_blocking(l->hw);   /* IRQs off, stripe held */
        if (l->owner == -1) {
            l->owner = me;
            spin_unlock_unsafe(l->hw);              /* drop stripe, keep IRQs off */
            return irq;
        }
        if (l->owner == me) panic("aspin: recursive lock");
        spin_unlock(l->hw, irq);                    /* drop stripe, IRQs back on */
        while (l->owner != -1) tight_loop_contents(); /* spin on RAM, no HW lock */
    }
}

void aspin_unlock(aspinlock_t *l, uint32_t irq)
{
    __dmb();
    l->owner = -1;              /* aligned 32-bit store is atomic */
    restore_interrupts(irq);
}
```

Design choices to settle:

- **Embed the word in the object, not in a global array.** A fixed array just moves the limit from 8 to N. With the word inside `aspinlock_t`, there is no limit. If you do want a handle table (for example, for syscalls), size it generously and return an error when it is full instead of panicking.
- **Keep IRQs off while the lock is held.** If the holder is preempted, another thread on the same core spins for a whole 5–20 ms time slice. If an ISR on the same core takes the lock, the core deadlocks. Between retries, IRQs come back on so USB and SysTick keep running.
- **Store the owner, not just a flag.** A recursive acquire then panics with a message instead of hanging silently, and an unlock from the wrong thread can be caught.
- **Consider a ticket lock.** Two words (`next`, `serving`): increment `next` under the stripe, then spin until `serving` matches. Waiters are served in order, so one core cannot starve the other.
- **Keep hot kernel locks dedicated.** `sched_lock` and `heap_lock` should stay on their own hardware locks; the extra stripe round-trip is not worth it there.

Cost: an uncontended acquire adds one stripe acquire/release and a load and store over a raw hardware spinlock. That is roughly tens of cycles, an estimate not yet measured on hardware. Under contention, the waiter spins on RAM instead of the SIO register, which is about the same cost.

A cheap first step that delivers the "slow down rather than crash" goal: make `spinlock_init()` call `spin_lock_claim_unused(false)`, and when it returns -1, fall back to the pooled memory-word lock instead of panicking.

## Continuous scan buffers (WiFi and BT)

`wifi_scan_start()` and `bt_scan_start()` hand scan windows to an app through a triple buffer (`src/kernel/scanbuf.[ch]`): three lists take turns as *fill* (written by the CYW43 async-context IRQ), *ready* (the newest finished window) and *held* (owned by the app until its next `*_scan_wait()`). Roles move and no data is copied.

| Operation | Runs on | Locks taken | Why |
| --- | --- | --- | --- |
| IRQ writes `lists[fill]` | async-context IRQ, core 0 | none (it is the IRQ) | Reads only `fill` |
| `scanbuf_publish()` (fill ↔ ready) | `wifi-poll` thread | `g_cont_lock` (kmutex), then `cyw43_arch_lwip_begin()` | Moves `fill`, so the IRQ must not run mid-swap |
| `scanbuf_take()` (ready ↔ held) | subscriber thread | `g_cont_lock` only | Never touches `fill`, so the IRQ is not involved |
| wake the subscriber | `wifi-poll` thread | `event_flags_set()` after both locks are dropped | Scheduler calls stay out of the IRQ |

Both locks are needed because the async-context lock is a recursive mutex owned by a **core**. Two picoOS threads on the same core would both get in through it, so the kmutex orders threads and the async lock keeps the IRQ out. Lock order is always kmutex → async lock. The subscriber reads its held list with no lock at all, since neither the IRQ nor publish can reach it.

Waking up: `*_scan_wait()` waits on `CONT_EV_READY | CONT_EV_STOP`, then, under the kmutex, clears `READY` *before* it calls `take`. A publish that lands after the take sets the flag again, so no window is missed. A wake-up that finds nothing fresh just waits again.

Known hazard: if a subscriber thread is killed while it holds `g_cont_lock` (a few instructions inside `*_scan_wait()`), the mutex is never released and `wifi-poll` blocks on it. This is the general picoOS problem of killing a thread that holds a kernel mutex (see `docs/imperfections.md` §11).

## Review findings and recommendations

The most serious finding was an SMP race in the block/wake path (fixed 2026-10-05). Between `spinlock_irq_release()` (step 5) and the PendSV switch (step 6), a thread is still running on its core. If the other core called `sched_unblock()` on it in that window, the thread became READY, and `sched_next_thread()` could resume it from a stale `saved_sp` while it was still running. The same happened when the SysTick wake loop woke a thread that had just gone to sleep. Now `sched_next_thread()` records `current_tcb[core]` under `sched_lock` and never picks a thread that is still current on the other core; that core switches away at its next PendSV, and then the thread can run anywhere. The wake loop and `sched_start_core1()` also run under `sched_lock`.

| # | Finding | Location | Severity | Suggested fix |
| --- | --- | --- | --- | --- |
| 1 | ~~Woken thread can be scheduled on the other core before it has switched out~~ | `sync.c` wait loops, `sched_next_thread()` | Fixed 2026-10-05 | `sched_next_thread()` skips `t == current_tcb[other core]`; `current_tcb[]` is set under `sched_lock` |
| 2 | `event_waiter_alloc()` return value ignored; if the pool is full the waiter has mask 0 and is never woken | `sync.c:668` | Medium | Check for -1 and fail or panic with a message |
| 3 | Nesting two primitives that share a stripe deadlocks with IRQs off | `prim_pool_assign`, `sync.c:117` | Medium (latent) | The memory-word design above, or a debug-build check that the stripe is not already held by this core |
| 4 | `spinlock_init()` still panics when IDs 24–31 are gone | `sync.c:192` | Medium | Claim with `false` and fall back to a pooled memory-word lock |
| 5 | `kmutex_unlock()` does not check the caller owns the mutex; a recursive lock blocks forever | `sync.c:387`, `sync.c:364` | Low (teaching point) | Compare `owner_tid` with the caller; panic or return an error |
| 6 | `mqueue` copies up to 64 B with IRQs off and a stripe held | `sync.c:740` | Low | Acceptable at 64 B; note it as a latency teaching point |
| 7 | ~~Stale comments: sync.h says atomicity comes from IRQ disable or LDREX; the semaphore header says count goes negative; the `spinlock_init` comment sits above `sync_init`~~ | `sync.h` | Fixed 2026-10-02 | Comments now describe the hardware-lock design |
| 8 | `spinlock_acquire()` / `spinlock_release()` (no IRQ change) have no callers | `sync.c:263` | Low | Remove them, or document that IRQ handlers on the same core must never take the same lock |

Suggested order of work:

- [x] Fix finding 1 (2026-10-05); a stress test (two cores, one mutex, many lock/unlock cycles, `PICOOS_LOCK_DEBUG` on) is still worth running on hardware
- [ ] Make `spinlock_init()` fall back instead of panicking (finding 4)
- [ ] Add `aspinlock_t` with the owner word inside the object and expose it to applications
- [ ] Fix the ignored `event_waiter_alloc()` result (finding 2)
- [x] Clean up the stale comments (finding 7)
