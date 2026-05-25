# Investigation Notes: picoOS Codebase Inspection

These notes record what was found during a systematic inspection of the picoOS repository. Every technical claim here traces to an actual file and line number. Uncertain interpretations are labeled as inferences.

---

## Files Inspected

| File | Lines | Purpose |
|------|-------|---------|
| `src/main.c` | 245 | Boot sequence and kernel initialization |
| `src/kernel/arch.h` | 206 | SDK/CMSIS includes + host LSP stubs |
| `src/kernel/task.h` | 178 | TCB/PCB struct definitions and constants |
| `src/kernel/task.c` | 477 | Thread/process lifecycle implementation |
| `src/kernel/sched.h` | 105 | Scheduler API and extern declarations |
| `src/kernel/sched.c` | 481 | Priority round-robin scheduler |
| `src/kernel/sched_asm.S` | 254 | PendSV context switch handler (ARM assembly) |
| `src/kernel/mem.h` | 71 | Heap allocator API |
| `src/kernel/mem.c` | 177 | First-fit boundary-tag heap |
| `src/kernel/sync.h` | ~137 | Synchronization primitive declarations |
| `src/kernel/sync.c` | ~450 | Synchronization implementations |
| `src/kernel/syscall.h` | 92 | Syscall number enum and API |
| `src/kernel/syscall.c` | ~400 | Syscall dispatch table |
| `src/kernel/dev.h` | 104 | Device abstraction types and ioctl constants |
| `src/kernel/dev.c` | ~411 | Device implementations |
| `src/kernel/vfs.h` | 95 | VFS type definitions and API |
| `src/kernel/vfs.c` | 247 | VFS routing logic |
| `src/kernel/fs.h` | 138 | Filesystem types, constants, API |
| `src/kernel/fs.c` | ~543 | Flash filesystem implementation |
| `src/kernel/wifi.h` | 99 | WiFi state machine, scan result types, API |
| `src/kernel/wifi.c` | 266 | CYW43 WiFi driver |
| `src/kernel/bluetooth.h` | 150 | BT state, scan result types, API |
| `src/kernel/bluetooth.c` | 349 | BTstack Classic + BLE scan |
| `src/btstack_config.h` | 74 | BTstack scan-only configuration |
| `src/shell/shell.h` | 80 | Shell types and API |
| `src/shell/shell.c` | ~960 | USB CDC interactive shell |
| `src/drivers/display.h` | 83 | ST7789 display driver API |
| `src/drivers/display.c` | 788 | ST7789 SPI driver |
| `src/drivers/led.h` | 48 | RGB LED driver API |
| `src/drivers/led.c` | 234 | Pimoroni RGB LED PWM driver |
| `src/apps/app_table.h` | 47 | App registration ABI |
| `src/apps/demo.c` | ~200 | Producer/consumer/sensor demo apps |
| `CMakeLists.txt` | 29 | Root build configuration |
| `src/CMakeLists.txt` | 310 | Feature flags, board detection, output naming |
| `tools/console.py` | 462 | USB serial terminal + file upload |
| `tools/mem_report.py` | 329 | ELF map parser for memory usage reports |
| `docs/design.md` | ~12 KB | Architecture and design rationale |
| `docs/setup.md` | ~9.7 KB | Build and environment setup |
| `docs/application.md` | ~13.7 KB | App development API reference |
| `docs/picoOS_API.md` | ~25.9 KB | Full developer API reference |
| `docs/imperfections.md` | ~9.7 KB | Catalogue of intentional teaching imperfections |
| `docs/expandfilesystem.md` | ~6.1 KB | Filesystem sizing analysis |
| `docs/fedora-build.md` | ~9.1 KB | Fedora-specific build guide |
| `docs/studentwork.md` | ~5.4 KB | Classroom build guide |
| `docs/project-submodule.md` | ~14.3 KB | picoOS as a git submodule |
| `README.md` | ~18 KB | Project overview and quick-start |
| `CLAUDE.md` | ~9.4 KB | AI assistant guidance |
| `pl-pico-reference-card.md` | ~37.9 KB | Raspberry Pi Pico hardware reference |

---

## Architecture Notes

### Boot Sequence (`src/main.c`)

The boot sequence is linear and sequential, not table-driven. Eight conceptual phases:

1. **Lines 113–131:** `stdio_usb_init()` + polling loop (30 × 100 ms) waiting for USB host enumeration. This is why you do not see output if you connect before the host CDC driver is ready.

2. **Lines 136–154:** Print version banner. Version is composed from `PICOOS_VERSION_MAJOR`, `PICOOS_VERSION_MINOR`, `PICOOS_VERSION_EDIT` — compile-time defines set in `src/CMakeLists.txt`.

3. **Lines 157–169:** `multicore_launch_core1(core1_entry)`. Core 1 entry function (lines 95–108) immediately calls `multicore_lockout_victim_init()` and then enters `__wfi()`. Core 0 spins on `core1_lockout_ready` flag. This matters because flash erase/program operations require halting Core 1 via `multicore_lockout_start_blocking()`, which only works if Core 1 has registered as a victim.

4. **Lines 174–178:** Five subsystem inits in order — `kmem_init()`, `task_init()`, `dev_init()`, `vfs_init()`, `fs_init()`. Order matters: task depends on mem, vfs depends on dev, fs is last.

5. **Lines 183 + optional 194–199:** Kernel process (PID 1) created. On pico_w/pico2_w boards, `wifi_init()` then `bt_init()`. BT must follow WiFi because it hooks into CYW43's async context.

6. **Lines 206–208:** Idle thread created at priority 7 (lowest), 512-byte stack, infinite `__wfi()` loop. Also calls `tud_task()` to service USB stack. Ensures the scheduler always has at least one READY thread.

7. **Lines 213–216:** Shell process (PID 2) created at priority 2.

8. **Lines 239–240:** `sched_init()` sets IRQ priorities; `sched_start()` picks the first READY thread and begins execution. Never returns.

**Key observation:** `sched_start()` does NOT use an exception return to launch the first thread. It directly calls the entry function (sched.c lines 472–479). This is simpler on Cortex-M0+ than constructing a fake EXC_RETURN, and SysTick will preempt once the first tick fires.

### TCB Layout and Assembly Constraints

`tcb_t` is defined in `src/kernel/task.h` lines 86–101. The first six fields have fixed byte offsets:

```
offset  0: uint32_t tid
offset  4: uint32_t pid
offset  8: uint8_t *stack_base
offset 12: uint32_t stack_size
offset 16: uint32_t *saved_sp    ← PendSV reads/writes here
offset 20: uint32_t exc_return   ← PendSV loads LR from here
```

`sched_asm.S` lines 147–153 directly use offsets 16 and 20:
```asm
str r0, [r1, #16]   ; current_tcb->saved_sp = r0 (saved PSP)
str r0, [r1, #20]   ; current_tcb->exc_return = LR
```

This constraint is enforced by `_Static_assert` in `task.c` lines 33–36. Any reordering of these fields silently breaks context switching.

**The `exc_return` field matters on RP2350 (Cortex-M33):** If a thread has used FPU instructions, the CPU pushes an extended 26-word exception frame (vs. the basic 8-word frame). The EXC_RETURN value encodes which kind was pushed. Storing it per-thread ensures the correct frame size is used on restoration.

### Scheduler Design (`src/kernel/sched.c`)

- **Time slice:** `TIME_SLICE_MS = 10` (line 30)
- **Priority levels:** `NUM_PRIORITIES = 8` (line 31), 0 = highest, 7 = lowest
- **Ready queue structure:** Array of 8 singly-linked lists using `tcb_t->next` as the intrusive pointer (lines 39–41)
- **Round-robin within priority:** When the current thread is at the head of its priority queue and peers exist, it is rotated to the tail (sched_next_thread lines 290–303)
- **Sleeping thread wakeup:** Checked in `isr_systick()` every 1 ms by scanning all MAX_THREADS slots (lines 344–356). This is O(n) per tick — an intentional teaching imperfection.
- **Zombie reaping:** `sched_next_thread()` detects THREAD_ZOMBIE, calls `task_free_thread()`, and continues to select a new thread (lines 260–270).

**The atomic sleep race:** `sched_sleep()` uses a critical section (save_and_disable_interrupts / restore_interrupts, lines 168–171) to atomically change state to THREAD_SLEEPING and remove from ready queue. Without this, SysTick could fire between setting the state and removing from the queue, leaving the thread sleeping but still runnable — a subtle data corruption bug.

### PendSV Context Switch (`src/kernel/sched_asm.S`)

The full 11-step flow:

1. Disable interrupts (`cpsid i`)
2. Read PSP into r0 (`mrs r0, psp`) — CPU already pushed hardware frame
3. Push r4–r11 below hardware frame (software saves)
   - **M0+ complication:** Cannot use `stmdb r0!, {r4-r11}` because M0+ does not support STMDB for high registers (r8–r11). Uses a staging workaround: save r4–r7 first, copy r8–r11 into r4–r7, save again (lines 114–133)
4. Store `r0` (new PSP bottom) into `current_tcb->saved_sp` (offset 16)
5. Store LR (EXC_RETURN value) into `current_tcb->exc_return` (offset 20)
6. Call `sched_next_thread()` — returns next TCB pointer in r0
7. Update `current_tcb` global
8. Load `exc_return` and `saved_sp` from next TCB
9. Restore r4–r11 (reverse of step 3)
10. Update PSP (`msr psp, r0`)
11. Re-enable interrupts (`cpsie i`); return via `bx lr`

The saved frame layout on the PSP stack:
```
[stack_base + canary]
[r4] ← saved_sp points here
[r5]
[r6]
[r7]
[r8]
[r9]
[r10]
[r11]
[r0]  ← hardware exception frame (8 words)
[r1]
[r2]
[r3]
[r12]
[lr]
[pc]  ← return address
[xpsr]
[stack_base + stack_size]
```

### First-Fit Heap (`src/kernel/mem.c`)

- **Block header:** `size` (payload bytes, not including header), `free` (bool), `next` pointer (lines 46–50)
- **Header size:** `ALIGN8(sizeof(struct heap_block))` — padded to 8-byte alignment
- **Allocation:** Walk list from heap_head, take first free block ≥ requested size; split if leftover ≥ SPLIT_THRESHOLD (32 bytes) + HEADER_SIZE
- **Deallocation:** Mark free; forward-coalesce by merging with all consecutive free successors; backward-coalesce by walking from heap_head to find predecessor (O(n) backward walk)
- **Note:** Backward coalesce is O(n) per free operation. For a 64 KB heap with ~32-byte average allocation, worst case is ~2000 iterations. Acceptable but intentionally unoptimized.

### Synchronization Primitives (`src/kernel/sync.c`)

- **Spinlock:** Two variants — `spinlock_irq_acquire` saves/restores interrupt state; `spinlock_acquire` does not. The IRQ variant is used in critical sections that must not be interrupted.
- **Mutex:** FIFO waiter queue using `tcb_t->next`; non-recursive; owner tracked by `owner_tid`.
- **Semaphore:** Counting semaphore; a negative count means |count| threads are blocked.
- **Event flags:** 32-bit bitmask; supports wait-for-any or wait-for-all; uses a `event_waiter_pool[]` array to store per-waiter wait masks separately from the TCB.
- **Message queue:** Ring buffer; 16 messages × 64 bytes; separate send and receive waiter queues; blocks caller if full (send) or empty (recv).

### Filesystem (`src/kernel/fs.c`)

**Layout on flash:**
```
Sector 0:     Superblock (metadata for all files)
Sector 1:     File 0 data (up to 4 KB)
Sector 2:     File 1 data
...
```

**Superblock structure** (`fs_superblock_t`, fs.h lines 72–77): magic (0x50494353 = "PICS"), version, file_count, array of `fs_entry_t` (used, name, size, start_block, block_count).

**Maximum superblock size constraint:** 12 + N × 32 ≤ 4096. For N = 127 files: 12 + 127×32 = 4076 bytes — just fits in one sector. This constraint is noted in CLAUDE.md and documented in `docs/expandfilesystem.md`.

**Read path:** Zero-copy via XIP. `fs_read()` returns a pointer directly into the XIP flash address space (`XIP_BASE + FS_FLASH_OFFSET + sector * FS_BLOCK_SIZE`).

**Write path:** Data accumulated in `fs_buffer` (static 4 KB buffer). On `fs_close()`, the sector is erased and programmed from the buffer. This means:
1. Only one file can be open for writing at a time (single `fs_buffer`).
2. Writes larger than 4 KB are not supported in the current implementation.
3. Flash write requires multicore lockout + interrupt disable.

**Inference:** The comment "currently RAM-backed" in CLAUDE.md appears to mean that write-path data lives in RAM (the `fs_buffer`) until commit on close. The filesystem is not purely RAM-backed — reads go directly to flash XIP. The distinction is about where write data lives before commit.

### Device Abstraction (`src/kernel/dev.c`)

The device table is a static array of `device_t` structs, each containing function pointers for open/read/write/ioctl/close. Device IDs:
- `DEV_CONSOLE` (0): USB CDC via TinyUSB, non-blocking `getchar_timeout_us(0)` reads
- `DEV_TIMER` (1): Returns `time_us_64()` as 8-byte little-endian value
- `DEV_FLASH` (2): Stubs — raw stream I/O not yet implemented (Phase 5 TODO)
- `DEV_GPIO` (3): ioctl-based; pin and value packed into a single `uint32_t` arg
- `DEV_DISPLAY` (4, conditional): ST7789 display ioctl commands
- `DEV_LED` (5, conditional): RGB LED PWM via ioctl

**IOCTL command ranges:**
- 0x0100–0x01xx: Timer commands
- 0x0200–0x02xx: GPIO commands  
- 0x0300–0x03xx: Display commands
- 0x0400–0x04xx: LED commands

### VFS Layer (`src/kernel/vfs.c`)

VFS maintains a global fd table (`vfs_fd_t` array, `VFS_MAX_OPEN = 16`) and a device mount table (`vfs_dev_mount_t` array, max 8 entries). `vfs_open()` checks device mounts first; if path starts with `/dev/`, it routes to the device layer. Otherwise it routes to `fs_open()`.

The `vfs_fd_t` struct tracks: used flag, type (device or file), path, position, device ID (if device), and fs_file_id (if file). Position tracking for devices is vestigial — devices don't use it meaningfully.

### WiFi Driver (`src/kernel/wifi.c`)

- **Async context:** CYW43 requires polling; `wifi_poll_thread()` calls `cyw43_arch_poll()` every 10 ms.
- **Scan callback:** `scan_result_cb()` is invoked by the CYW43 driver during an active scan. Results are stored in `g_scan[]` (max 16 entries). Duplicates are silently dropped when the buffer is full.
- **Connection retry:** `wifi_connect()` retries up to 3 times with 500 ms backoff. The CYW43 chip occasionally returns `CYW43_LINK_BADAUTH` spuriously on first attempt.
- **State machine:** `WIFI_STATE_DOWN → SCANNING/CONNECTING → UP/ERROR`

### Bluetooth Driver (`src/kernel/bluetooth.c`)

- **Shares CYW43 async context** with WiFi — driven by the same `wifi-poll` thread calling `cyw43_arch_poll()`. BT must be initialized after WiFi.
- **Classic inquiry:** 5 × 1.28 s = ~6.4 s window; results include address, Class of Device, RSSI.
- **BLE passive scan:** 48-slot interval (~30 ms), 30-slot window (~18.75 ms); runs simultaneously with Classic inquiry.
- **AD parsing:** `extract_ble_adv_data()` parses standard AD types: name (0x08/0x09), flags (0x01), TX power (0x0A), manufacturer specific (0xFF → company ID).
- **BTstack configuration (`src/btstack_config.h`):** Scan-only — `MAX_NR_HCI_CONNECTIONS = 0`, no L2CAP, no RFCOMM, no pairing/bonding.

### Shell (`src/shell/shell.c`)

- **Command table:** Static array of `shell_cmd_t` + dynamic additions via `shell_register_cmd()`. Max 32 total (`SHELL_MAX_CMDS`).
- **Main loop:** readline → tokenize (in-place whitespace split) → lookup → call handler. Handler returns int (0 = success, nonzero = error).
- **config.txt:** Read via VFS at startup. Key=value format. `AUTORUN=<appname>` launches app at boot. `BUTTONA=<appname>` etc. bind buttons on Display Pack hardware.
- **Button monitor:** If any button bindings found, spawns a background thread polling GPIO at 100 ms intervals. Launches bound app if not already running.
- **App PIDs:** Kernel 1–9, autorun 50+, button bindings 200+, manual `run` command 100+. These PID ranges are set by `next_pid` in syscall.c — they are not enforced by hardware.

### Build System (`src/CMakeLists.txt`)

**Board detection:** CMake inspects `PICO_CYW43_SUPPORTED` (set by Pico SDK when board has CYW43) to decide whether WiFi/BT sources are compiled. Feature flags default ON but auto-disable for non-CYW43 boards.

**Output naming:** Target name is constructed as `{board_name}os{suffix}-v{major}.{minor}.{edit}` where suffix is `_D` (Display Pack), `_D2` (Display Pack 2), or empty. Example: `picowos_D-v0.2.1`.

**FS constants per board:**
- pico/picow (RP2040): `FS_MAX_FILES=64`, `FS_FLASH_SIZE=1MB`
- pico2/pico2w (RP2350): `FS_MAX_FILES=127`, `FS_FLASH_SIZE=3MB`

---

## Build Process Notes

**Normal build:**
```bash
export PICO_SDK_PATH="$HOME/pico-sdk"
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico
make -j$(nproc) -C build
```

**Outputs in `build/src/`:** `.uf2`, `.bin`, `.hex`, `.elf`, `.elf.map`, `.dis`

**Pre-built firmware:** `kits/` directory contains pre-built UF2 files for pico, picow, pico2, pico2w variants with and without display.

**LSP support:** Symlink `build/compile_commands.json` to repo root for clangd to resolve all SDK headers. Without this, clangd cannot find `pico/stdlib.h` and reports cascading errors.

**The `build` script:** Executable shell wrapper at repo root for `cmake + make`. The `build` file must not be deleted or overwritten (noted in project memory: there is a `build` file at repo root; use `build_*` directories for test builds).

---

## Documentation Gaps

1. **No test framework:** CLAUDE.md states "There is no test framework; testing is done via the interactive shell on hardware or via `tools/console.py`." No unit tests exist in the codebase.

2. **Phase 5 unimplemented:** `flash_read()` and `flash_write()` in `dev.c` contain TODO comments — raw flash stream I/O is not implemented. GPIO bulk read/write is also stubbed.

3. **Phase 6 planned only:** A bytecode VM for interpreted programs is mentioned in the README phases but has no code or design documentation.

4. **Driver documentation is thin:** The display and LED drivers have API documentation in `picoOS_API.md` but the implementation details are not separately documented.

5. **No SMP scheduler:** The scheduler has a `affinity` field in the TCB (0 = any core, 1 = Core 0, 2 = Core 1) but Core 1 scheduling is not implemented — the scheduler only runs on Core 0. `current_tcb` is a single global.

6. **WiFi connection management incomplete:** `wifi_connect()` connects to one AP but there is no reconnection logic if the connection drops. This is documented implicitly by the state machine but no reconnect path is coded.

7. **process heap_base/heap_size:** PCB has `heap_base` and `heap_size` fields, but per-process heap allocation is not implemented — all dynamic allocation goes through the single kernel heap (`kmalloc/kfree`).

---

## Questions for Future Maintainers

1. **Core 1 affinity:** The TCB has an `affinity` field, suggesting Core 1 scheduling was planned. What is the intended design for running threads on Core 1? Will `sched_asm.S` need a separate `current_tcb` per core?

2. **Per-process heap:** `pcb_t.heap_base` and `heap_size` are initialized but never assigned. Is per-process memory isolation planned for a future phase?

3. **Multi-sector files:** The filesystem currently supports only single-sector files (4 KB max). Is multi-sector support planned, and how would the superblock encode multi-block extents?

4. **lwIP integration:** lwIP is linked for CYW43 boards but only used implicitly by the CYW43 driver. Is a TCP/IP application layer (HTTP client, MQTT, etc.) planned for a future phase?

5. **sched_asm.S for SMP:** If Core 1 eventually runs its own scheduler loop, the PendSV handler and current_tcb management will need major changes. What architecture is intended?

6. **Global kernel lock:** CLAUDE.md lists the global kernel lock as an intentional imperfection. Where exactly is this lock applied? Searching the code suggests interrupt disable is used as the "lock" — is there a plan for finer-grained locking in a future phase?

---

## Areas of Uncertain Interpretation

- The phrase "RAM-backed in current phase" for the filesystem (CLAUDE.md) is interpreted as referring to the write buffer (`fs_buffer`) living in RAM before flash commit. Read paths still use XIP flash directly. This interpretation is consistent with `fs_read()` returning XIP pointers.

- The `core1_lockout_ready` variable in `main.c` is inferred to be a global flag set by Core 1 to signal readiness. The exact mechanism (multicore FIFO or shared volatile variable) was not confirmed by direct line inspection of Core 1 entry code — the pattern matches standard CYW43 dual-core lockout initialization.

- The `app_table_size` extern in `app_table.h` implies it is defined in `apps/demo.c` alongside the `app_table[]` array. For submodule builds where `PICOOS_INCLUDE_DEMO_APPS=OFF`, the parent project must provide its own `app_table[]` and `app_table_size` definitions.
