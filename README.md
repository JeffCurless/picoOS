# picoOS

An educational operating system for the **Raspberry Pi Pico family (RP2040 / RP2350)**.  picoOS is a small, readable teaching kernel — not a scaled-down desktop OS.  Every component is deliberately simple so students can read the code, understand how it works, and then improve it.

```
=======================================================
picoOS  v0.2.0

  Platform : RP2040, dual ARM Cortex-M0+ (133 MHz max)
  Options  : none
=======================================================

Threads created:
  TID 1  PID 1  pri 7  idle
  TID 2  PID 2  pri 2  shell

Starting scheduler...

pico> ps
PID  NAME             THREADS  ALIVE
---  ---------------  -------  -----
1    kernel           1        yes
2    shell            1        yes

pico> threads
TID  PID  PRI  STATE     CPU-ms  STACK   NAME             CANARY
---  ---  ---  --------  ------  ------  ---------------  --------
1    1    7    READY     142     2048    idle             OK
2    2    2    RUNNING   1023    2048    shell            OK
```

---

## What it demonstrates

| OS concept | Where to look |
|-----------|--------------|
| Preemptive scheduling | `src/kernel/sched.c`, `src/kernel/sched_asm.S` |
| Context switching (Cortex-M0+ and M33) | `src/kernel/sched_asm.S` |
| Thread and process abstractions | `src/kernel/task.h`, `src/kernel/task.c` |
| Mutex, semaphore, event flags, message queues | `src/kernel/sync.c` |
| First-fit heap allocator | `src/kernel/mem.c` |
| Flash-backed filesystem | `src/kernel/fs.c` |
| Device abstraction (VFS) | `src/kernel/vfs.c`, `src/kernel/dev.c` |
| Dual-core asymmetric split (RP2040 / RP2350) | `src/main.c` |
| Producer/consumer IPC demo | `src/apps/demo.c` |
| Interactive USB shell | `src/shell/shell.c` |
| WiFi (pico_w / pico2_w only) | `src/kernel/wifi.c` |
| ST7789 display driver (optional) | `src/drivers/display.c` |
| RGB LED driver (optional) | `src/drivers/led.c` |

---

## Hardware

| | |
|-|-|
| **MCU** | RP2040 — dual Cortex-M0+ @ up to 133 MHz, or RP2350 — dual Cortex-M33 @ up to 150 MHz |
| **RAM** | 264 KB SRAM (RP2040) / 520 KB SRAM (RP2350) |
| **Flash** | 2 MB QSPI (RP2040) / 4 MB QSPI (RP2350), execute-in-place via XIP |
| **Console** | USB CDC serial (appears as `/dev/ttyACM0` on Linux) |
| **Supported boards** | `pico`, `pico2`, `picow`, `pico2w` |
| **Optional** | Pimoroni Display Pack (ST7789 240×135) or Display Pack 2 (ST7789V 320×240); RGB LED, 4 buttons on both |

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

# 3. Build (choose your board)
cd picoOS
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico
make -j$(nproc) -C build

# 4. Flash (hold BOOTSEL on Pico, then plug in USB)
cp build/src/picoos_D-v0.2.0.uf2 /media/$USER/RPI-RP2/

# 5. Open the console
pip install pyserial
python3 tools/console.py
```

### Board selection

Pass `-DPICO_BOARD=<name>` to CMake.  picoOS accepts the underscore-free aliases and maps them to the SDK-canonical names internally:

| `-DPICO_BOARD=` | Board | Chip | WiFi | Output files (Display Pack example) |
|----------------|-------|------|------|--------------------------------------|
| `pico` | Raspberry Pi Pico | RP2040 | No | `picoos_D-v0.2.0.*` |
| `pico2` | Raspberry Pi Pico 2 | RP2350 | No | `pico2os_D-v0.2.0.*` |
| `picow` | Raspberry Pi Pico W | RP2040 | Yes | `picowos_D-v0.2.0.*` |
| `pico2w` | Raspberry Pi Pico 2 W | RP2350 | Yes | `pico2wos_D-v0.2.0.*` |

The output files (`.uf2`, `.bin`, `.elf`, `.elf.map`, `.dis`) are named after the board and include the version number, so builds for different boards can share the same output directory without conflict.

### Build options

The display and LED drivers are **enabled by default** but can be turned off when running on a plain Pico without a Display Pack attached.

| CMake flag | Default | Effect |
|------------|---------|--------|
| `PICOOS_DISPLAY_ENABLE` | `ON` | Compile the ST7789 driver; mount `/dev/display` |
| `PICOOS_DISPLAY_PACK2` | `OFF` | Use Display Pack 2 (320×240, ~75 KB framebuffer) instead of Display Pack (240×135, ~32 KB) |
| `PICOOS_DISPLAY_SHELL` | `ON` | Register the `display` shell command (requires `PICOOS_DISPLAY_ENABLE`) |
| `PICOOS_LED_ENABLE` | `ON` | Compile the RGB LED driver; mount `/dev/led` |
| `PICOOS_LED_SHELL` | `ON` | Register the `led` shell command (requires `PICOOS_LED_ENABLE`) |
| `PICOOS_INCLUDE_DEMO_APPS` | `ON` | Compile the built-in demo apps and their `app_table[]`; set `OFF` when providing custom apps via `PICOOS_APP_SOURCES` |

Dependency rules enforced by CMake:
- Setting `PICOOS_DISPLAY_ENABLE=OFF` cascades OFF to all sub-features — they all share the same hardware.
- Setting any sub-feature ON automatically enables `PICOOS_DISPLAY_ENABLE`.
- `PICOOS_LED_SHELL=ON` forces `PICOOS_LED_ENABLE=ON`.

```bash
# Plain Pico — no Display Pack hardware
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico \
      -DPICOOS_DISPLAY_ENABLE=OFF
make -j$(nproc) -C build

# Display Pack attached, but suppress the shell commands
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico \
      -DPICOOS_DISPLAY_SHELL=OFF \
      -DPICOOS_LED_SHELL=OFF
make -j$(nproc) -C build

# Full build with Display Pack (default)
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico
make -j$(nproc) -C build
```

---

## Shell commands

Once running, the USB shell accepts:

| Command | Description |
|---------|-------------|
| `info` | Show system version and build info |
| `help` | List all commands |
| `ps` | Show processes |
| `threads` | Show all threads with state, priority, CPU time, and stack canary status |
| `kill <tid>` | Terminate a thread; frees its stack and TCB slot; auto-frees the process when its last thread exits |
| `killproc <pid>` | Terminate all threads in a process and free the PCB |
| `mem` | Memory usage and heap stats |
| `ls` | List filesystem files |
| `cat <file>` | Print a file |
| `fs write <file> [text]` | Create or overwrite a file (omit `[text]` for multi-line mode; end with `.` alone) |
| `fs append <file> <text>` | Append a line to an existing file |
| `fs format` | Erase all files and reinitialise the filesystem |
| `rm <file>` | Delete a file |
| `run <app>` | Launch a built-in application |
| `trace on\|off` | Enable/disable scheduler trace output |
| `update` | Reboot into USB BOOTSEL mode for reflashing |
| `reboot` | Hard reboot |
| `display <subcmd>` | Drive the ST7789 display *(registered when `PICOOS_DISPLAY_SHELL=ON`)* |
| `led <r> <g> <b>` | Set RGB LED color 0–255 per channel *(registered when `PICOOS_LED_SHELL=ON`)* |
| `wifi [status\|scan\|connect\|disconnect]` | WiFi management *(registered when built for pico_w / pico2_w)* |

---

## Repository layout

```
picoOS/
├── CMakeLists.txt          Top-level CMake build (board alias mapping lives here)
├── pico_sdk_import.cmake   Pico SDK discovery (standard boilerplate)
├── docs/
│   ├── design.md              Architecture design document
│   ├── setup.md               Environment setup, build, and flash guide
│   ├── application.md         How to write and register a new app
│   ├── picoOS_API.md          Developer API reference
│   ├── imperfections.md       Catalogue of deliberate teaching imperfections
│   ├── studentwork.md         Student build guide (Fedora)
│   ├── fedora-build.md        Full Fedora cross-compile setup guide
│   ├── expandfilesystem.md    Filesystem sizing analysis and implementation notes
│   └── project-submodule.md   Tutorial: using picoOS as a git submodule
├── src/
│   ├── CMakeLists.txt      Source-level build: feature flags, sources, output naming
│   ├── main.c              Boot sequence, process/thread creation
│   ├── kernel/
│   │   ├── arch.h          SDK/CMSIS includes (ARM) + host stubs (LSP)
│   │   ├── task.[ch]       TCB / PCB pools, thread stack initialisation
│   │   ├── sched.[ch]      Preemptive priority round-robin scheduler
│   │   ├── sched_asm.S     PendSV context-switch handler (M0+ and M33 asm)
│   │   ├── mem.[ch]        First-fit boundary-tag heap allocator (64 KB)
│   │   ├── sync.[ch]       Spinlock, mutex, semaphore, event flags, mqueue
│   │   ├── syscall.[ch]    Syscall dispatch table
│   │   ├── dev.[ch]        Device abstraction layer
│   │   ├── vfs.[ch]        VFS routing (device files vs. filesystem)
│   │   ├── fs.[ch]         Flash-native persistent filesystem
│   │   └── wifi.[ch]       CYW43 WiFi module — scan, connect, poll thread (pico_w/pico2_w)
│   ├── shell/
│   │   └── shell.[ch]      USB CDC interactive shell
│   ├── apps/
│   │   ├── app_table.[ch]  Stable app registration ABI (app_entry_t, app_table extern)
│   │   └── demo.[ch]       Built-in producer/consumer/sensor demo threads + app_table[]
│   └── drivers/
│       ├── display.[ch]    ST7789 240×135 driver — /dev/display (optional)
│       └── led.[ch]        Pimoroni RGB LED driver — /dev/led (optional)
└── tools/
    ├── console.py          Host-side terminal companion (pyserial)
    ├── mem_report.py       SRAM usage report derived from the linker map
    └── add_license.py      Prepend MIT + Commons Clause header to all .c/.h files
```

---

## Architecture overview

See **[docs/design.md](docs/design.md)** for the full design rationale.  The key points:

### Dual-core split

Both RP2040 and RP2350 have two cores.  picoOS uses them asymmetrically to keep the design easy to follow:

- **Core 0** — USB console, SysTick, scheduler, filesystem writes, shell
- **Core 1** — registers as a multicore lockout victim (required for safe flash writes), then idles in `__wfi()`.  User worker threads, compute tasks, and background services can be pinned here.

### Cortex-M33 FPU support (RP2350)

On RP2350 the Cortex-M33 has a hardware FPU.  When a thread uses floating-point instructions the CPU saves an extended 26-word exception frame instead of the standard 8-word frame, and sets `LR=0xFFFFFFED`.  picoOS stores the actual `EXC_RETURN` value per-thread in the TCB (`tcb_t.exc_return`) and restores it on every context switch so the correct frame size is always used.  This also fixes USB CDC input reliability on the pico2_w when WiFi is enabled.

### Optional hardware drivers

Both drivers expose their hardware through the VFS like any other built-in device:

| Device path | Driver | Hardware |
|-------------|--------|----------|
| `/dev/display` | `drivers/display.c` | ST7789 240×135 (Display Pack) or ST7789V 320×240 (Display Pack 2) — selected by `PICOOS_DISPLAY_PACK2`; SPI0: DC=GPIO 16, CS=17, SCK=18, MOSI=19, BL=20; buttons GPIO 12–15 |
| `/dev/led` | `drivers/led.c` | Active-low RGB LED; PWM on GPIO 6 (R), 7 (G), 8 (B) |

Use `ioctl` on `/dev/display` with `IOCTL_DISP_*` commands (clear, flush, draw pixel/line/rect/text, set backlight, read buttons) and on `/dev/led` with `IOCTL_LED_SET_RGB` / `IOCTL_LED_OFF`.

### WiFi module (pico_w / pico2_w)

When built for a CYW43-equipped board (`PICO_BOARD=picow` or `pico2w`), `kernel/wifi.c` is compiled in.  `wifi_init()` — called from `main.c` — initialises the CYW43 radio, enables STA mode, spawns a low-priority `wifi-poll` thread (priority 6) in the kernel process, and registers the `wifi` shell command.  Scan results and connection state are managed internally; the poll thread calls `cyw43_arch_poll()` every 10 ms.

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

### Memory budget

The `tools/mem_report.py` script derives live numbers from the linker map after every build:

```bash
# Pass the board-named map file as a positional argument
python3 tools/mem_report.py build/src/picoos_D-v0.2.0.elf.map

# Or use the --map option
python3 tools/mem_report.py --map build/src/pico2wos_D-v0.2.0.elf.map

# One-line summary
python3 tools/mem_report.py build/src/picoos_D-v0.2.0.elf.map --brief
```

RP2040 typical breakdown with display and LED enabled:

| Region | Size |
|--------|------|
| Kernel heap (thread stacks + objects) | 64 KB |
| Display framebuffer (RGB332, 240×135) | ~32 KB |
| FS write buffer + superblock cache | ~5 KB |
| .data, TCB/PCB pools, VFS tables | ~9 KB |
| SDK / other | ~11 KB |
| **Available headroom** | ~143 KB |

Without `PICOOS_DISPLAY_ENABLE`, the 32 KB framebuffer is reclaimed.  RP2350 boards have 520 KB total SRAM so headroom is substantially larger.

---

## Host tools

### `tools/console.py`

Connects to the Pico over USB serial:

- Auto-detection of the Pico by USB VID:PID (`2E8A:000A`)
- Interactive shell in raw terminal mode (local echo disabled; Pico echoes instead)
- `--log FILE` — tee all output to a log file
- `--upload FILE DEST` — transfer a file using the `fs write` shell command
- `--list-ports` — list available serial ports

```bash
pip install pyserial
python3 tools/console.py --help
```

### `tools/mem_report.py`

Parses the linker map produced by every build and prints an SRAM usage breakdown by subsystem.  The map file is named after the board, display variant, and version (e.g. `build/src/picoos_D-v0.2.0.elf.map`).

```bash
python3 tools/mem_report.py build/src/picoos_D-v0.2.0.elf.map        # positional path
python3 tools/mem_report.py --map build/src/picoos_D-v0.2.0.elf.map  # named option
python3 tools/mem_report.py --brief                             # one-line summary (uses default path)
```

---

## Implementation phases

The codebase is structured around the six phases in [docs/design.md](docs/design.md):

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
2. **Multi-file write support** — the FS currently allows only one file open for writing at a time; add a per-file buffer pool
3. **Extend the shell** — add new commands by calling `shell_register_cmd()` from any module
4. **Launch Core 1 workers** — replace the Core 1 idle loop in `src/main.c` with real thread dispatch
5. **Drive the display** — write a status dashboard using `/dev/display` ioctls

---

## License

This project is source-available for educational and non-commercial use. You may study it, modify it, and share improvements, but you may not sell the software or remove the copyright/acknowledgement notice.
