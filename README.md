# picoOS

An educational operating system for the **Raspberry Pi Pico family (RP2040 / RP2350)**.  picoOS is a small, readable teaching kernel — not a scaled-down desktop OS.  Every component is deliberately simple so students can read the code, understand how it works, and then improve it.

```
=======================================================
picoOS  v0.3.6

  Platform : RP2040, dual ARM Cortex-M0+ (133 MHz max)
  Options  : DISPLAY_PACK
=======================================================

Threads created:
  TID 1  PID 1  pri 7  idle
  TID 2  PID 1  pri 7  idle1
  TID 3  PID 2  pri 2  shell

Starting scheduler...

pico> ps
PID  NAME             THREADS  ALIVE
---  ---------------  -------  -----
1    kernel           2        yes
2    shell            1        yes

pico> threads
TID  PID  PRI  CORE  STATE     Time    STACK   NAME            CANARY
---  ---  ---  ----  --------  ------  ------  ---------------  --------
1    1    7    0     READY     142     512     idle             OK
2    1    7    1     READY     138     512     idle1            OK
3    2    2    *     RUNNING   1023    2048    shell            OK
```

---

## What it demonstrates

| OS concept | Where to look |
|-----------|--------------|
| Preemptive scheduling | `src/kernel/sched.c`, `src/kernel/sched_asm.S` |
| Context switching (Cortex-M0+ and M33) | `src/kernel/sched_asm.S` |
| Thread and process abstractions | `src/kernel/task.h`, `src/kernel/task.c` |
| **SMP dual-core scheduling** — both cores run threads concurrently | `src/kernel/sched.c`, `src/main.c` |
| **Thread affinity** — pin threads to Core 0, Core 1, or either | `src/kernel/task.h` (`THREAD_AFFINITY_*`) |
| Mutex, semaphore, event flags, message queues | `src/kernel/sync.c` |
| SMP-safe synchronization — RP2040 hardware spinlocks | `src/kernel/sync.c`, `src/kernel/mem.c` |
| First-fit heap allocator | `src/kernel/mem.c` |
| Flash-backed filesystem | `src/kernel/fs.c` |
| Device abstraction (VFS) | `src/kernel/vfs.c`, `src/kernel/dev.c` |
| Cross-core producer/consumer IPC demo | `src/apps/demo.c` |
| Boot-time app launch and button bindings (`config.txt`) | `src/shell/shell.c` — see [docs/application.md](docs/application.md#launching-an-app-automatically) |
| **Monte Carlo π estimation** — SMP worker threads with argument passing | `src/apps/pi.c` |
| Interactive USB shell | `src/shell/shell.c` |
| WiFi — scan, connect, multicast UDP (pico_w / pico2_w only) | `src/kernel/wifi.c` |
| Sharing data with an IRQ-context writer (scan-result copy-out) | `src/kernel/wifi.c`, `src/apps/scantest.c` |
| Bluetooth scanning + device-type detection (pico_w / pico2_w only) | `src/kernel/bluetooth.c` |
| ST7789 display driver (optional) | `src/drivers/display.c` |
| RGB LED driver (optional) | `src/drivers/led.c` |
| Host-native kernel unit tests | `tests/` — see [docs/testing.md](docs/testing.md) |
| On-device stress test (pico_w / pico2_w) | `src/apps/scantest.c`, `tools/scantest.py` — see [docs/testing.md](docs/testing.md) |

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
How the test suites work is in **[docs/testing.md](docs/testing.md)**.
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
cmake -B build_pico -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico
make -j$(nproc) -C build_pico

# 4. Flash (hold BOOTSEL on Pico, then plug in USB)
cp build_pico/src/picoos_D-v0.3.6.uf2 /media/$USER/RPI-RP2/

# 5. Open the console
pip install pyserial
python3 tools/console.py

# Run the host-native test suites (no hardware required)
./build tests

# On-device WiFi/BT scan test (pico_w / pico2_w; see docs/testing.md)
./build wifi            # then flash a picowos*/pico2wos* image from kits/
python3 tools/scantest.py
```

### Board selection

Pass `-DPICO_BOARD=<name>` to CMake.  picoOS accepts the underscore-free aliases and maps them to the SDK-canonical names internally:

| `-DPICO_BOARD=` | Board | Chip | WiFi + BT | Output files (Display Pack example) |
|----------------|-------|------|-----------|--------------------------------------|
| `pico` | Raspberry Pi Pico | RP2040 | No | `picoos_D-v0.3.6.*` |
| `pico2` | Raspberry Pi Pico 2 | RP2350 | No | `pico2os_D-v0.3.6.*` |
| `picow` | Raspberry Pi Pico W | RP2040 | Yes | `picowos_D-v0.3.6.*` |
| `pico2w` | Raspberry Pi Pico 2 W | RP2350 | Yes | `pico2wos_D-v0.3.6.*` |

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
| `PICOOS_BT_ENABLE` | `ON` | Compile Bluetooth scanning support; auto-disabled on boards without CYW43 |
| `PICOOS_INCLUDE_DEMO_APPS` | `ON` | Compile the built-in demo apps and their `app_table[]`; set `OFF` when providing custom apps via `PICOOS_APP_SOURCES` |
| `PICOOS_LOCK_DEBUG` | `OFF` | Deadlock detection: blocking locks time out after `PICOOS_LOCK_TIMEOUT_MS` (5000) and print diagnostics |
| `PICOOS_SCAN_RACE_INJECT` | `OFF` | **Test only.** Re-opens the scan-buffer race so `scantest` can show it failing; images get an `_INJ` suffix |

Dependency rules enforced by CMake:
- Setting `PICOOS_DISPLAY_ENABLE=OFF` cascades OFF to all sub-features — they all share the same hardware.
- Setting any sub-feature ON automatically enables `PICOOS_DISPLAY_ENABLE`.
- `PICOOS_LED_SHELL=ON` forces `PICOOS_LED_ENABLE=ON`.

```bash
# Plain Pico — no Display Pack hardware
cmake -B build_pico -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico \
      -DPICOOS_DISPLAY_ENABLE=OFF
make -j$(nproc) -C build_pico

# Display Pack attached, but suppress the shell commands
cmake -B build_pico -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico \
      -DPICOOS_DISPLAY_SHELL=OFF \
      -DPICOOS_LED_SHELL=OFF
make -j$(nproc) -C build_pico

# Full build with Display Pack (default)
cmake -B build_pico -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico
make -j$(nproc) -C build_pico
```

---

## Shell commands

Once running, the USB shell accepts:

| Command | Description |
|---------|-------------|
| `info` | Show system version and build info |
| `help` | List all commands |
| `ps` | Show processes |
| `threads` | Show all threads with state, priority, **core affinity**, CPU time, and stack canary status |
| `kill <tid>` | Terminate a thread; frees its stack and TCB slot; auto-frees the process when its last thread exits |
| `killproc <pid>` | Terminate all threads in a process and free the PCB |
| `mem` | Memory usage and heap stats |
| `ls` | List filesystem files |
| `cat <file>` | Print a filesystem file (reads through `fs_open`, so `/dev/*` paths are not supported) |
| `fs write <file> [text]` | Create or overwrite a file (omit `[text]` for multi-line mode; end with `.` alone) |
| `fs append <file> <text>` | Append a line to a file (creates it if missing) |
| `fs format` | Erase all files and reinitialise the filesystem |
| `rm <file>` | Delete a file |
| `run [<app> [arg]]` | Launch a built-in application; optional argument is passed to the app's entry function as `void *arg`. With no app name, lists the available apps |
| `trace on\|off` | Enable/disable scheduler trace output |
| `update` | Reboot into USB BOOTSEL mode for reflashing |
| `reboot` | Hard reboot |
| `display <subcmd>` | Drive the ST7789 display *(registered when `PICOOS_DISPLAY_SHELL=ON`)* |
| `led <r> <g> <b>` | Set RGB LED color 0–255 per channel *(registered when `PICOOS_LED_SHELL=ON`)* |
| `wifi [status\|scan\|connect\|disconnect]` | WiFi management *(registered when built for pico_w / pico2_w)* |
| `bt [status\|scan]` | Bluetooth scan — lists nearby Classic and BLE devices with device type *(registered when built for pico_w / pico2_w)* |

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
│   ├── locking.md             How the locking mechanisms work (HW spinlocks, striping, primitives)
│   ├── testing.md             Host unit tests, on-device scantest, PICOOS_LOCK_DEBUG
│   ├── studentwork.md         Student build guide (Fedora)
│   ├── fedora-build.md        Full Fedora cross-compile setup guide
│   ├── project-submodule.md   Tutorial: using picoOS as a git submodule
│   └── book/                  Notes and outline for a companion book
├── build                   Script: build all firmware variants into kits/, or run host tests
├── clean                   Script: remove build directories and tests/build
├── src/
│   ├── CMakeLists.txt      Source-level build: feature flags, sources, output naming
│   ├── main.c              Boot sequence, process/thread creation
│   ├── btstack_config.h    Minimal BTstack configuration (scan only)
│   ├── lwipopts.h          lwIP configuration for the CYW43 builds
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
│   │   ├── wifi.[ch]       CYW43 WiFi module — scan, connect, MAC, multicast UDP, wifi-poll state thread (pico_w/pico2_w)
│   │   └── bluetooth.[ch]  CYW43 Bluetooth module — Classic + BLE scan, device-type detection (pico_w/pico2_w)
│   ├── shell/
│   │   └── shell.[ch]      USB CDC interactive shell
│   ├── apps/
│   │   ├── app_table.h     Stable app registration ABI (app_entry_t, app_table extern)
│   │   ├── demo.[ch]       Built-in producer/consumer/sensor demo threads + app_table[]
│   │   ├── pi.[ch]         Monte Carlo π estimation — SMP worker threads, run-time arg
│   │   ├── cray_one.c      Multi-node WiFi multicast color-grid demo (pico_w/pico2_w + display)
│   │   ├── scantest.[ch]   On-device WiFi/BT scan-buffer race test (pico_w/pico2_w)
│   │   └── wifi_test.c     WiFi multicast test (not currently built)
│   └── drivers/
│       ├── display.[ch]    ST7789 driver, Display Pack 240×135 or Pack 2 320×240 — /dev/display (optional)
│       └── led.[ch]        Pimoroni RGB LED driver — /dev/led (optional)
├── tests/
│   ├── CMakeLists.txt      Standalone host CMake project; four CTest targets
│   ├── framework.h         Minimal BEGIN_TEST / CHECK / END_TEST / SUMMARY macros
│   ├── mem/
│   │   └── test_mem.c      Heap allocator tests (14 cases)
│   ├── sync/
│   │   ├── mock_sched.c    Scheduler stubs for host compilation
│   │   └── test_sync.c     Spinlock, mutex, semaphore, event flags, mqueue, lock debug (37 cases)
│   ├── fs/
│   │   ├── mock_flash.c/h  RAM-backed flash mock (erase, program, XIP reads)
│   │   └── test_fs.c       Filesystem CRUD, limits, persistence, corruption (17 cases)
│   └── vfs/
│       ├── mock_dev.c/h    Device-layer call-counting stubs
│       ├── mock_fs.c/h     Filesystem-layer call-counting stubs
│       └── test_vfs.c      VFS routing, FD exhaustion, delegation (10 cases)
└── tools/
    ├── console.py          Host-side terminal companion (pyserial)
    ├── mem_report.py       SRAM usage report derived from the linker map
    ├── scantest.py         Run the on-device scantest over USB; exit 0 on PASS
    └── add_license.py      Prepend MIT + Commons Clause header to all .c/.h files
```

---

## Architecture overview

See **[docs/design.md](docs/design.md)** for the full design rationale.  The key points:

### Dual-core SMP scheduling

Both RP2040 and RP2350 have two cores.  picoOS runs a **full SMP scheduler** — both cores independently select and execute threads from the same shared priority-ready queues:

- **Core 0** — USB console, filesystem writes, shell, SysTick sleep/wake scan.  Runs the `idle` thread when nothing else is eligible.
- **Core 1** — registers as a multicore lockout victim so Core 0's flash writes (`flash_safe_execute()`) can safely pause it.  Runs its own SysTick, PendSV, and `idle1` thread.  Executes any thread with `affinity == THREAD_AFFINITY_C1` or `THREAD_AFFINITY_ANY`.

Thread affinity is set per-TCB with one of three constants:

| Constant | Value | Meaning |
|----------|-------|---------|
| `THREAD_AFFINITY_ANY` | -1 | Eligible on either core (default) |
| `THREAD_AFFINITY_C0` | 0 | Core 0 only |
| `THREAD_AFFINITY_C1` | 1 | Core 1 only |

SMP correctness is provided by the SIO hardware spinlocks (the Cortex-M0+ has no LDREX/STREX).  Dedicated hardware locks guard the scheduler ready queues, the kernel heap and the event-waiter pool.  Mutexes, semaphores, event flags and message queues share a small striped pool of hardware locks, so applications can create as many as they like.  A spinlock disables interrupts on the local core only while it is held.  See [docs/locking.md](docs/locking.md) for the full design.

The `threads` command shows which core each thread is bound to (`0`, `1`, or `*` for any-core).

### Cortex-M33 FPU support (RP2350)

On RP2350 the Cortex-M33 has a hardware FPU.  When a thread uses floating-point instructions the CPU saves an extended 26-word exception frame instead of the standard 8-word frame, and sets `LR=0xFFFFFFED`.  picoOS stores the actual `EXC_RETURN` value per-thread in the TCB (`tcb_t.exc_return`) and restores it on every context switch so the correct frame size is always used.  This also fixes USB CDC input reliability on the pico2_w when WiFi is enabled.

### Optional hardware drivers

Both drivers expose their hardware through the VFS like any other built-in device:

| Device path | Driver | Hardware |
|-------------|--------|----------|
| `/dev/display` | `drivers/display.c` | ST7789 240×135 (Display Pack) or ST7789V 320×240 (Display Pack 2) — selected by `PICOOS_DISPLAY_PACK2`; SPI0: DC=GPIO 16, CS=17, SCK=18, MOSI=19, BL=20; buttons GPIO 12–15 |
| `/dev/led` | `drivers/led.c` | Active-low RGB LED; PWM on GPIO 6 (R), 7 (G), 8 (B) |

Use `ioctl` on `/dev/display` with `IOCTL_DISP_*` commands (clear, flush, draw pixel/line/rect/text, set backlight, read buttons) and on `/dev/led` with `IOCTL_LED_SET_RGB` / `IOCTL_LED_OFF`.

### WiFi and Bluetooth modules (pico_w / pico2_w)

When built for a CYW43-equipped board (`PICO_BOARD=picow` or `pico2w`), `kernel/wifi.c` is compiled in.  `wifi_init()` — called from `main.c` — initialises the CYW43 radio, enables STA mode, spawns a low-priority `wifi-poll` thread (priority 6) in the kernel process, and registers the `wifi` shell command.

picoOS links `pico_cyw43_arch_lwip_threadsafe_background`, so the CYW43 driver, lwIP and BTstack all run in the SDK's async context: a low-priority interrupt on core 0.  `cyw43_arch_poll()` does nothing in this mode.  The `wifi-poll` thread only watches for the end of a scan and for link drops, every 10 ms.  Because scan results are written from that interrupt, applications read them with `wifi_copy_scan_results()` / `bt_copy_scan_results()`, which copy under the async-context lock.

When `PICOOS_BT_ENABLE` is also on (the default for CYW43 boards), `kernel/bluetooth.c` is compiled in and `bt_init()` is called immediately after `wifi_init()`.  It hooks BTstack into the same async context that WiFi uses, so no extra thread is needed.  `bt_init()` registers the `bt` shell command and powers on the BT radio asynchronously.

The `bt scan` command runs a simultaneous Classic BR/EDR inquiry (~6.4 s) and BLE passive scan, then prints a table of discovered devices:

```
Address            RSSI  Type     Class       Name
-----------------  ----  -------  ----------  ----
AA:BB:CC:DD:EE:FF   -52  Classic  phone       iPhone
11:22:33:44:55:66   -71  Classic  audio       JBL Flip 5
77:88:99:AA:BB:CC   -85  BLE      unknown     Tile
```

Classic device type is derived from the Bluetooth **Class of Device (CoD)** major class field.  BLE device type defaults to `unknown`; the advertised local name is shown when present in the advertising data.

### Deliberately imperfect

Several parts of v1 are intentionally suboptimal — these are teaching opportunities, not bugs:

| What | Why it's imperfect | How to improve it |
|------|--------------------|-------------------|
| O(n) ready-queue scan | Readable | Priority bitmap, skip list |
| Fixed-size thread stacks | No fragmentation complexity | Stack-usage analysis, adaptive sizing |
| First-fit heap | Fragmentation is visible | Buddy allocator, slab allocator |
| Linear file lookup | Fine at ≤ 127 files | Hash table, B-tree index |
| Synchronous console I/O | Easy to trace | Lock-free ring buffer |
| Single-root filesystem | Simpler directory model | Hierarchical namespace |
| Single concurrent writer | One 4 KB write buffer | Per-file buffer pool |

The full list, with file locations and suggested fixes, is in [docs/imperfections.md](docs/imperfections.md).

### Memory budget

The `tools/mem_report.py` script derives live numbers from the linker map after every build:

```bash
# Pass the board-named map file as a positional argument
python3 tools/mem_report.py build_pico/src/picoos_D-v0.3.6.elf.map

# Or use the --map option
python3 tools/mem_report.py --map build_pico2w/src/pico2wos_D-v0.3.6.elf.map

# One-line summary
python3 tools/mem_report.py build_pico/src/picoos_D-v0.3.6.elf.map --brief
```

RP2040 breakdown for `picoos_D-v0.3.4` (Pico with Display Pack, no WiFi):

| Region | Size |
|--------|------|
| Kernel heap (thread stacks + objects) | 64 KB |
| Display framebuffer (RGB332, 240×135) | ~32 KB |
| FS write buffer + superblock cache | ~6 KB |
| .data, TCB/PCB pools, VFS tables | ~10 KB |
| SDK / other | ~6 KB |
| **Available headroom** | ~146 KB |

Without `PICOOS_DISPLAY_ENABLE`, the 32 KB framebuffer is reclaimed.  The WiFi/BT builds add the CYW43 driver, lwIP and BTstack, which take a large share of the headroom.  RP2350 boards have 520 KB total SRAM so headroom is substantially larger.

---

## Host tools

### `tools/console.py`

Connects to the Pico over USB serial:

- Auto-detection of the Pico by USB VID:PID (`2E8A:000A` RP2040, `2E8A:0009` RP2350)
- Interactive shell in raw terminal mode (local echo disabled; Pico echoes instead)
- `--log FILE` — tee all output to a log file
- `--upload FILE DEST` — transfer a file using the `fs write` shell command
- `--list-ports` — list available serial ports

```bash
pip install pyserial
python3 tools/console.py --help
```

### `tools/mem_report.py`

Parses the linker map produced by every build and prints an SRAM usage breakdown by subsystem.  The map file is named after the board, display variant, and version (e.g. `build_pico/src/picoos_D-v0.3.6.elf.map`).

```bash
python3 tools/mem_report.py build_pico/src/picoos_D-v0.3.6.elf.map        # positional path
python3 tools/mem_report.py --map build_pico/src/picoos_D-v0.3.6.elf.map  # named option
python3 tools/mem_report.py --brief                             # one-line summary (uses default path)
```

### `tools/scantest.py`

Runs the on-device `scantest` app on a pico_w / pico2_w over USB, echoes its output, and exits 0 on PASS or 1 on FAIL/timeout.  See [docs/testing.md](docs/testing.md).

```bash
python3 tools/scantest.py            # copy-out API
python3 tools/scantest.py --raw      # deprecated pointer getters (control run)
```

---

## Implementation phases

The codebase is structured around the six phases in [docs/design.md](docs/design.md):

| Phase | Status | Description |
|-------|--------|-------------|
| 1 — Bring-up | ✅ Complete | Boot banner, USB console, single-core scheduler, demo threads |
| 2 — Kernel basics | ✅ Complete | Mutex, semaphore, queues, process/thread management, mem stats |
| 3 — Second core | ✅ Complete | Full SMP: both cores schedule threads; per-core SysTick/PendSV; thread affinity; RP2040 HW spinlock protection; cross-core producer/consumer demo; Monte Carlo π SMP benchmark |
| 4 — Filesystem | ✅ Complete | Flash-backed persistent FS: XIP reads, sector erase/program on write, survives reboot |
| 5 — User services | 🔲 Planned | Logger service, background worker.  Boot-time app launch (`AUTORUN=`) and Display Pack button bindings already work through `config.txt` |
| 6 — Polish | 🔲 Planned | Scheduler visualiser, panic dumps, host-side loader |

---

## Contributing / extending

The project is designed to be modified.  Suggested starting points for students:

1. **Improve the scheduler** — add priority inheritance to fix priority inversion in `kmutex_lock`
2. **Multi-file write support** — the FS currently allows only one file open for writing at a time; add a per-file buffer pool
3. **Extend the shell** — add new commands by calling `shell_register_cmd()` from any module
4. **Drive the display** — write a status dashboard using `/dev/display` ioctls

---

## License

This project is source-available for educational and non-commercial use. You may study it, modify it, and share improvements, but you may not sell the software or remove the copyright/acknowledgement notice.
