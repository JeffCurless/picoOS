# picoOS

An educational operating system for the **Raspberry Pi Pico (RP2040)**.  picoOS is a small, readable teaching kernel — not a scaled-down desktop OS.  Every component is deliberately simple so students can read the code, understand how it works, and then improve it.

```
=== picoOS ===
RP2040 dual-core educational OS
Build: Mar  9 2026 14:23:01

pico> ps
PID  NAME             THREADS  ALIVE
---  ---------------  -------  -----
1    kernel           1        yes
2    shell            1        yes
3    demos            3        yes

pico> threads
TID  NAME       PRI  STATE    CPU(ms)
---  ---------  ---  -------  -------
1    idle        7   READY        142
2    shell       2   RUNNING     1023
3    producer    4   SLEEPING      87
4    consumer    4   BLOCKED       34
5    sensor      5   SLEEPING      12
```

---

## What it demonstrates

| OS concept | Where to look |
|-----------|--------------|
| Preemptive scheduling | `src/kernel/sched.c`, `src/kernel/sched_asm.S` |
| Context switching (ARM Cortex-M0+) | `src/kernel/sched_asm.S` |
| Thread and process abstractions | `src/kernel/task.h`, `src/kernel/task.c` |
| Mutex, semaphore, event flags, message queues | `src/kernel/sync.c` |
| First-fit heap allocator | `src/kernel/mem.c` |
| Flash-backed filesystem | `src/kernel/fs.c` |
| Device abstraction (VFS) | `src/kernel/vfs.c`, `src/kernel/dev.c` |
| Dual-core asymmetric split (RP2040) | `src/main.c` |
| Producer/consumer IPC demo | `src/apps/demo.c` |
| Interactive USB shell | `src/shell/shell.c` |

---

## Hardware

| | |
|-|-|
| **MCU** | RP2040 — dual Cortex-M0+ @ up to 133 MHz |
| **RAM** | 264 KB SRAM |
| **Flash** | 2 MB QSPI (execute-in-place via XIP) |
| **Console** | USB CDC serial (appears as `/dev/ttyACM0` on Linux) |
| **Board** | Raspberry Pi Pico or Pico W |

---

## Quick start

Full build and flash instructions are in **[docs/setup.md](docs/setup.md)**.
How to write and register a new application is in **[docs/application.md](docs/application.md)**.
The short version:

```bash
# 1. Install prerequisites (Debian/Ubuntu)
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential

# 2. Get the Pico SDK
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init --recursive
export PICO_SDK_PATH="$HOME/pico-sdk"

# 3. Build
cd picoOS
mkdir build && cd build
cmake ..
make -j$(nproc)

# 4. Flash (hold BOOTSEL on Pico, then plug in USB)
cp build/src/picoos.uf2 /media/$USER/RPI-RP2/

# 5. Open the console
pip install pyserial
python3 tools/console.py
```

---

## Shell commands

Once running, the USB shell accepts:

| Command | Description |
|---------|-------------|
| `help` | List all commands |
| `ps` | Show processes |
| `threads` | Show all threads with state, priority, CPU time |
| `kill <tid>` | Terminate a thread |
| `mem` | Memory usage and heap stats |
| `ls` | List filesystem files |
| `cat <file>` | Print a file |
| `write <file> <data>` | Create or overwrite a file |
| `rm <file>` | Delete a file |
| `format` | Erase all files and reinitialise the filesystem |
| `run <app>` | Launch a built-in application |
| `trace on\|off` | Enable/disable scheduler trace output |
| `update` | Reboot into USB BOOTSEL mode for reflashing |
| `reboot` | Hard reboot |

---

## Repository layout

```
picoOS/
├── CMakeLists.txt          Top-level CMake build
├── pico_sdk_import.cmake   Pico SDK discovery (standard boilerplate)
├── design.md               Architecture design document
├── docs/
│   └── setup.md            Environment setup, build, and flash guide
├── src/
│   ├── main.c              Boot sequence, process/thread creation
│   ├── kernel/
│   │   ├── arch.h          SDK/CMSIS includes (ARM) + host stubs (LSP)
│   │   ├── task.[ch]       TCB / PCB pools, thread stack initialisation
│   │   ├── sched.[ch]      Preemptive priority round-robin scheduler
│   │   ├── sched_asm.S     PendSV context-switch handler (Cortex-M0+ asm)
│   │   ├── mem.[ch]        First-fit boundary-tag heap allocator
│   │   ├── sync.[ch]       Spinlock, mutex, semaphore, event flags, mqueue
│   │   ├── syscall.[ch]    Syscall dispatch table
│   │   ├── dev.[ch]        Device abstraction layer
│   │   ├── vfs.[ch]        VFS routing (device files vs. filesystem)
│   │   └── fs.[ch]         Flash-native persistent filesystem (XIP reads, erase/program on write)
│   ├── shell/
│   │   └── shell.[ch]      USB CDC interactive shell
│   └── apps/
│       └── demo.[ch]       Producer/consumer/sensor demo threads + app table
└── tools/
    └── console.py          Host-side terminal companion (pyserial)
```

---

## Architecture overview

See **[design.md](design.md)** for the full design rationale.  The key points:

### Dual-core split

The RP2040 has two cores.  picoOS uses them asymmetrically to keep the design easy to follow:

- **Core 0** — USB console, SysTick, scheduler, filesystem writes, shell
- **Core 1** — user worker threads, compute tasks, background services

### Deliberately imperfect

Several parts of v1 are intentionally suboptimal — these are teaching opportunities, not bugs:

| What | Why it's imperfect | How to improve it |
|------|--------------------|-------------------|
| Global kernel lock | Simple to reason about | Per-subsystem fine-grained locks |
| O(n) ready-queue scan | Readable | Priority bitmap, skip list |
| Fixed-size thread stacks | No fragmentation complexity | Stack-usage analysis, adaptive sizing |
| First-fit heap | Fragmentation is visible | Buddy allocator, slab allocator |
| Linear file lookup | Fine at ≤ 32 files | Hash table, B-tree index |
| Synchronous console I/O | Easy to trace | Lock-free ring buffer |
| Single-root filesystem | Simpler directory model | Hierarchical namespace |

### Memory budget (264 KB SRAM)

| Region | Size |
|--------|------|
| Thread stack pool (16 × 4 KB) | 64 KB |
| Kernel heap | 32 KB |
| FS superblock cache (`superblock_ram`) | ~1 KB |
| FS write scratch buffer (`scratch_buf`) | 4 KB |
| Kernel code, data, SDK overhead | ~50 KB |
| **Available headroom** | **~113 KB** |

---

## Host console tool

`tools/console.py` connects to the Pico over USB serial and provides:

- Auto-detection of the Pico by USB VID:PID (`2E8A:000A`)
- Interactive shell in raw terminal mode (local echo disabled; Pico echoes instead)
- `--log FILE` — tee all output to a log file
- `--upload FILE DEST` — transfer a file using the `write` shell command
- `--list-ports` — list available serial ports

```bash
pip install pyserial
python3 tools/console.py --help
```

---

## Implementation phases

The codebase is structured around the six phases in [design.md](design.md):

| Phase | Status | Description |
|-------|--------|-------------|
| 1 — Bring-up | ✅ Complete | Boot banner, USB console, single-core scheduler, demo threads |
| 2 — Kernel basics | ✅ Complete | Mutex, semaphore, queues, process/thread management, mem stats |
| 3 — Second core | ✅ Complete (stub) | Core 1 launched with lockout-victim init; work distribution left as student exercise |
| 4 — Filesystem | ✅ Complete | Flash-backed persistent FS: XIP reads, sector erase/program on write, survives reboot |
| 5 — User services | 🔲 Planned | Logger service, app launcher, background worker |
| 6 — Polish | 🔲 Planned | Scheduler visualiser, panic dumps, host-side loader |

---

## Contributing / extending

The project is designed to be modified.  Suggested starting points for students:

1. **Improve the scheduler** — add priority inheritance to fix priority inversion in `mutex_lock`
2. **Add stack overflow detection** — check the canary on every context switch in `sched_asm.S`, not just in `mem`
3. **Multi-file write support** — the FS currently allows only one file open for writing at a time; add a per-file scratch buffer pool
4. **Extend the shell** — add new commands by calling `shell_register_cmd()` from any module
5. **Launch Core 1 workers** — replace the Core 1 idle loop in `src/main.c` with real thread dispatch
