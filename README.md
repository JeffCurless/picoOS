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
| ST7789 display driver (optional) | `src/drivers/display.c` |
| RGB LED driver (optional) | `src/drivers/led.c` |

---

## Hardware

| | |
|-|-|
| **MCU** | RP2040 — dual Cortex-M0+ @ up to 133 MHz |
| **RAM** | 264 KB SRAM |
| **Flash** | 2 MB QSPI (execute-in-place via XIP) |
| **Console** | USB CDC serial (appears as `/dev/ttyACM0` on Linux) |
| **Board** | Raspberry Pi Pico or Pico W |
| **Optional** | Pimoroni Pico Display Pack (ST7789 240×135, RGB LED, 4 buttons) |

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
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk"
make -j$(nproc) -C build

# 4. Flash (hold BOOTSEL on Pico, then plug in USB)
cp build/src/picoos.uf2 /media/$USER/RPI-RP2/

# 5. Open the console
pip install pyserial
python3 tools/console.py
```

### Build options

The display and LED drivers are **enabled by default** but can be turned off when running on a plain Pico without a Display Pack attached.

| CMake flag | Default | Effect |
|------------|---------|--------|
| `PICOOS_DISPLAY_ENABLE` | `ON` | Compile the ST7789 driver; mount `/dev/display`; adds ~32 KB SRAM for the framebuffer |
| `PICOOS_DISPLAY_SHELL` | `ON` | Register the `display` shell command (requires `PICOOS_DISPLAY_ENABLE`) |
| `PICOOS_LED_ENABLE` | `ON` (when display is ON) | Compile the RGB LED driver; mount `/dev/led` |
| `PICOOS_LED_SHELL` | `ON` | Register the `led` shell command (requires `PICOOS_LED_ENABLE`) |

```bash
# Plain Pico — no Display Pack hardware
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" \
      -DPICOOS_DISPLAY_ENABLE=OFF \
      -DPICOOS_LED_ENABLE=OFF
make -j$(nproc) -C build

# Display Pack attached, but suppress the shell commands
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" \
      -DPICOOS_DISPLAY_SHELL=OFF \
      -DPICOOS_LED_SHELL=OFF
make -j$(nproc) -C build

# Full build with Display Pack (default)
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk"
make -j$(nproc) -C build
```

> Disabling `PICOOS_DISPLAY_ENABLE` automatically disables `PICOOS_LED_ENABLE` as well — both drivers target the same Pimoroni board.

---

## Shell commands

Once running, the USB shell accepts:

| Command | Description |
|---------|-------------|
| `info` | Show system version and build info |
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
| `display <subcmd>` | Drive the ST7789 display *(registered when `PICOOS_DISPLAY_SHELL=ON`)* |
| `led <r> <g> <b>` | Set RGB LED color 0–255 per channel *(registered when `PICOOS_LED_SHELL=ON`)* |

---

## Repository layout

```
picoOS/
├── CMakeLists.txt          Top-level CMake build
├── pico_sdk_import.cmake   Pico SDK discovery (standard boilerplate)
├── design.md               Architecture design document
├── docs/
│   ├── setup.md            Environment setup, build, and flash guide
│   ├── application.md      How to write and register a new app
│   ├── imperfections.md    Catalogue of deliberate teaching imperfections
│   └── studentwork.md      Suggested student exercises
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
│   ├── apps/
│   │   └── demo.[ch]       Producer/consumer/sensor demo threads + app table
│   └── drivers/
│       ├── display.[ch]    ST7789 240×135 driver — /dev/display (optional)
│       └── led.[ch]        Pimoroni RGB LED driver — /dev/led (optional)
└── tools/
    ├── console.py          Host-side terminal companion (pyserial)
    └── mem_report.py       SRAM usage report derived from the linker map
```

---

## Architecture overview

See **[design.md](design.md)** for the full design rationale.  The key points:

### Dual-core split

The RP2040 has two cores.  picoOS uses them asymmetrically to keep the design easy to follow:

- **Core 0** — USB console, SysTick, scheduler, filesystem writes, shell
- **Core 1** — user worker threads, compute tasks, background services

### Optional hardware drivers

Both drivers expose their hardware through the VFS like any other built-in device:

| Device path | Driver | Hardware |
|-------------|--------|----------|
| `/dev/display` | `drivers/display.c` | ST7789 240×135 over SPI0; DC=GPIO 16, CS=GPIO 17, SCK=GPIO 18, MOSI=GPIO 19, BL=GPIO 20; buttons on GPIO 12–15 |
| `/dev/led` | `drivers/led.c` | Active-low RGB LED; PWM on GPIO 6 (R), 7 (G), 8 (B) |

Use `ioctl` on `/dev/display` with `IOCTL_DISP_*` commands (clear, flush, draw pixel/line/rect/text, set backlight, read buttons) and on `/dev/led` with `IOCTL_LED_SET_RGB` / `IOCTL_LED_OFF`.

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

The `tools/mem_report.py` script derives live numbers from the linker map after every build:

```bash
python3 tools/mem_report.py          # full table
python3 tools/mem_report.py --brief  # one-line summary
```

Typical breakdown with display and LED enabled:

| Region | Size |
|--------|------|
| Thread stack pool (16 × 4 KB) | 64 KB |
| Kernel heap | 32 KB |
| Display framebuffer (RGB332, 240×135) | ~32 KB |
| FS write buffer + superblock cache | ~5 KB |
| .data, TCB/PCB pools, VFS tables | ~9 KB |
| SDK / other | ~11 KB |
| **Available headroom** | ~111 KB |

Without `PICOOS_DISPLAY_ENABLE`, the 32 KB framebuffer is reclaimed.

---

## Host tools

### `tools/console.py`

Connects to the Pico over USB serial:

- Auto-detection of the Pico by USB VID:PID (`2E8A:000A`)
- Interactive shell in raw terminal mode (local echo disabled; Pico echoes instead)
- `--log FILE` — tee all output to a log file
- `--upload FILE DEST` — transfer a file using the `write` shell command
- `--list-ports` — list available serial ports

```bash
pip install pyserial
python3 tools/console.py --help
```

### `tools/mem_report.py`

Parses the linker map (`build/src/picoos.elf.map`) produced by every build and prints an SRAM usage breakdown by subsystem.

```bash
python3 tools/mem_report.py                        # default map path
python3 tools/mem_report.py --map path/to/foo.map  # custom map
python3 tools/mem_report.py --brief                # one-line summary
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
3. **Multi-file write support** — the FS currently allows only one file open for writing at a time; add a per-file buffer pool
4. **Extend the shell** — add new commands by calling `shell_register_cmd()` from any module
5. **Launch Core 1 workers** — replace the Core 1 idle loop in `src/main.c` with real thread dispatch
6. **Drive the display** — write a status dashboard using `/dev/display` ioctls
