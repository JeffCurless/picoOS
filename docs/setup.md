# Environment Setup, Build, and Flash Guide

This guide covers everything needed to go from a fresh machine to a running picoOS image on a Raspberry Pi Pico or Pico 2.

---

## 1. Hardware required

| Item | Notes |
|------|-------|
| Raspberry Pi Pico, Pico W, Pico 2, or Pico 2 W | Any supported RP2040 or RP2350 board |
| Micro-USB cable | Must carry data, not just power |
| Development machine | Linux, macOS, or Windows with WSL2 |

---

## 2. Install the toolchain

### Linux (Debian / Ubuntu / Raspberry Pi OS)

```bash
sudo apt update
sudo apt install -y \
    cmake \
    gcc-arm-none-eabi \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    build-essential \
    python3 \
    git \
    libusb-1.0-0-dev \
    pkg-config
```

`libusb-1.0-0-dev` and `pkg-config` are only needed to build `picotool` with USB support (section 3).  Do **not** `apt install picotool`: see [Install a matching picotool](#install-a-matching-picotool).

Verify the compiler is reachable:

```bash
arm-none-eabi-gcc --version
# arm-none-eabi-gcc (15:13.2.rel1-2) 13.2.1 ...
cmake --version
# cmake version 3.25.x  (3.13 minimum required)
```

### macOS

```bash
brew install cmake python3
brew install --cask gcc-arm-embedded
```

### Windows

Use WSL2 running Ubuntu and follow the Linux steps above.  Native Windows builds are possible via the Pico SDK installer but are not covered here.

---

## 3. Get the Pico SDK

Clone the SDK alongside this repository (or anywhere permanent — the path is recorded by CMake):

```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk
git submodule update --init --recursive
```

Set the environment variable so CMake and the SDK import script can find it.  Add this line to your `~/.profile` (environment variables belong in the login file, which every program you start inherits; `~/.bashrc` is for aliases and prompt settings):

```bash
export PICO_SDK_PATH="$HOME/pico-sdk"
```

Load it into the current shell (new logins pick it up automatically):

```bash
source ~/.profile
```

### Install a matching picotool

The SDK turns every firmware ELF into a `.uf2` with `picotool uf2 convert`, so the build needs picotool even if you never flash with it.  The SDK insists on an **exact** version: SDK 2.3.1 requires picotool 2.3.0 (`picotool_VERSION_REQUIRED` in `$PICO_SDK_PATH/tools/CMakeLists.txt`).  For another SDK release, check that line for the version to build:

```bash
grep -m1 picotool_VERSION_REQUIRED "$PICO_SDK_PATH/tools/CMakeLists.txt"
#     set(picotool_VERSION_REQUIRED 2.3.0)
```

The SDK finds picotool through its CMake package config (`picotoolConfig.cmake`), **not** through `PATH`.  It looks in, in order:

1. `$PICOTOOL_FETCH_FROM_GIT_PATH/picotool/` (environment or `-D` variable)
2. the system CMake package paths (e.g. `/usr/lib/cmake/picotool/` from a distro package)
3. if nothing was found at all: it downloads and builds picotool into each build directory (without USB support), which is slow and repeated for every variant

If step 2 finds the wrong version, configuration stops with *"Incompatible picotool installation found: Requires version 2.3.0, you have version 2.1.1"*.  The Debian / Raspberry Pi OS `picotool` package is 2.1.1, so remove it if it is installed (`sudo apt remove picotool`) and build the matching version:

```bash
cd ~/workspace
git clone --branch 2.3.0 https://github.com/raspberrypi/picotool.git picotool-src
cmake -S picotool-src -B picotool-src/build \
      -DPICO_SDK_PATH="$PICO_SDK_PATH" \
      -DPICOTOOL_FLAT_INSTALL=1 \
      -DCMAKE_INSTALL_PREFIX="$HOME/workspace/picotool-install"
make -j$(nproc) -C picotool-src/build install
```

`PICOTOOL_FLAT_INSTALL=1` puts the binary and its CMake config side by side in `~/workspace/picotool-install/picotool/`.

Tell the SDK where it is, and put the binary on your `PATH` for flashing (section 7).  Add to `~/.profile`:

```bash
export PICOTOOL_FETCH_FROM_GIT_PATH="$HOME/workspace/picotool-install"
```

and link the binary into `~/.local/bin` (Debian's default `~/.profile` already adds that directory to `PATH` when it exists):

```bash
mkdir -p ~/.local/bin
ln -sf ~/workspace/picotool-install/picotool/picotool ~/.local/bin/picotool
source ~/.profile
hash -r            # forget any old /usr/bin/picotool location bash cached
picotool version   # picotool v2.3.0 ...
```

The `./build` script exports `PICOTOOL_FETCH_FROM_GIT_PATH` itself when `~/workspace/picotool-install/picotool` exists, so `./build` works without the `~/.profile` line; a manual `cmake -B …` does not.

So `picotool load`/`reboot` can reach a Pico without `sudo`, install picotool's udev rules (they cover the RP2040 and RP2350 BOOTSEL and runtime product IDs):

```bash
sudo cp ~/workspace/picotool-src/udev/60-picotool.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

When you update the SDK, rebuild picotool at the matching tag and run `./clean` so no build directory keeps the old one cached.

---

## 4. Clone this repository

```bash
git clone <repo-url> picoOS
cd picoOS
```

---

## 5. Build

### Choose your board

Pass `-DPICO_BOARD=<name>` on the CMake command line.  picoOS accepts underscore-free aliases and maps them internally to SDK-canonical names:

| `-DPICO_BOARD=` | Board | Chip | WiFi + BT |
|----------------|-------|------|-----------|
| `pico` | Raspberry Pi Pico | RP2040 | No |
| `pico2` | Raspberry Pi Pico 2 | RP2350 | No |
| `picow` | Raspberry Pi Pico W | RP2040 | Yes |
| `pico2w` | Raspberry Pi Pico 2 W | RP2350 | Yes |

If `-DPICO_BOARD` is omitted, CMake defaults to `pico`.

### Build every variant with the `build` script

The repository root has a `build` script (a file, not a directory) that configures and builds each board/display combination in its own `builds/<board>_<ND|D|D2>/` directory and copies the `.uf2` images into `kits/`:

```bash
./build            # all 12 variants: pico, picow, pico2, pico2w × no display / Display Pack / Display Pack 2
./build pico       # pico + picow (6 variants)
./build pico2      # pico2 + pico2w (6 variants)
./build wifi       # picow + pico2w (6 variants) + 2 scantest fault-injection images (*_INJ)
./build tests      # host-native unit tests (see section 6)
./clean            # remove the builds/<board>_<variant>/ directories and builds/tests
```

It uses `$PICO_SDK_PATH`, or `$HOME/workspace/pico-sdk` if that is not set.
The script stops with `BUILD FAILED: <board> / <variant>` and a non-zero exit
status at the first variant that fails, so the images in `kits/` are always
from the current run up to that point.

### Warnings are errors

Every picoOS `.c` file is compiled with a broad warning set and `-Werror`
(`PICOOS_WARNINGS` in `src/CMakeLists.txt`): `-Wall -Wextra -Wshadow -Wundef
-Wpointer-arith -Wwrite-strings -Wformat=2 -Wformat-truncation=2
-Wimplicit-fallthrough -Wnull-dereference -Wdouble-promotion -Wredundant-decls
-Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
-Wnested-externs`. The Pico SDK, lwIP, CYW43 and BTstack sources are not built
with these flags. The host tests use the same set (`TEST_FLAGS` in
`tests/CMakeLists.txt`).

Two things trip people up most:

- On ARM `uint32_t` is `unsigned long`, so `printf("%u", x)` with a `uint32_t`
  is a warning. Cast it: `printf("%u", (unsigned)x)`. `shell_print()` is
  checked the same way as `printf`.
- A non-`static` function needs a prototype in a header the `.c` file
  includes (`-Wmissing-prototypes`).

To keep the warnings but stop them failing the build while you experiment,
configure with `-DPICOOS_WERROR=OFF`.

### Configure and build one board by hand

Use any directory name except `build`, which is the script above:

```bash
# Configure (replace pico with your target board)
cmake -B builds/pico -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico

# Build
make -j$(nproc) -C builds/pico
```

A successful build produces these files in `builds/pico/src/`, named after the board, display variant and version (`<name>` below is e.g. `picoos_D-v0.3.9`):

| File | Purpose |
|------|---------|
| `<name>.uf2` | **Flash this** — UF2 image for drag-and-drop or `picotool` |
| `<name>.elf` | ELF with debug symbols (used by GDB) |
| `<name>.bin` | Raw binary |
| `<name>.hex` | Intel HEX image |
| `<name>.dis` | Disassembly listing |
| `<name>.elf.map` | Linker map (used by `mem_report.py`) |

The display suffix is `_D` for Display Pack, `_D2` for Display Pack 2 and nothing without a display; test images built with `PICOOS_SCAN_RACE_INJECT` add `_INJ`.  For example, a `pico` + Display Pack build at v0.3.9 produces `picoos_D-v0.3.9.uf2`, and a `picow` build without a display produces `picowos-v0.3.9.uf2`.  Use a separate build directory per board, as the `build` script does: the board is stored in the CMake cache when the directory is first configured.

### Build options (optional)

The Display Pack drivers are ON by default.  Override on the CMake command line:

```bash
# No Display Pack hardware attached
cmake -B builds/pico -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico \
      -DPICOOS_DISPLAY_ENABLE=OFF

# Display Pack present but suppress shell commands
cmake -B builds/pico -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico \
      -DPICOOS_DISPLAY_SHELL=OFF -DPICOOS_LED_SHELL=OFF
```

| Flag | Default | Effect |
|------|---------|--------|
| `PICOOS_DISPLAY_ENABLE` | ON | ST7789 driver + `/dev/display` |
| `PICOOS_DISPLAY_PACK2` | OFF | Use Display Pack 2 (320×240, ~75 KB framebuffer) instead of Display Pack (240×135, ~32 KB) |
| `PICOOS_DISPLAY_SHELL` | ON | `display` shell command |
| `PICOOS_LED_ENABLE` | ON | RGB LED driver + `/dev/led` |
| `PICOOS_LED_SHELL` | ON | `led` shell command |
| `PICOOS_BT_ENABLE` | ON | Bluetooth scanning on pico_w / pico2_w; ignored on boards without a CYW43 |
| `PICOOS_INCLUDE_DEMO_APPS` | ON | Built-in apps (`producer`, `consumer`, `sensor`, `pi`, `hwtest`, plus `scantest` on WiFi boards); set `OFF` to supply your own app table |
| `PICOOS_LOCK_DEBUG` | OFF | Deadlock detection for blocking locks (timeout `PICOOS_LOCK_TIMEOUT_MS`, default 5000) — see [testing.md](testing.md) |
| `PICOOS_SCAN_RACE_INJECT` | OFF | **Test only** — re-opens the scan-buffer race for `scantest`; never flash these images for normal use |

Setting `PICOOS_DISPLAY_ENABLE=OFF` cascades OFF to all sub-features.  Setting any sub-feature ON automatically enables `PICOOS_DISPLAY_ENABLE`.

### Incremental builds

After editing source files, just run `make` again:

```bash
make -j$(nproc) -C builds/pico
```

CMake tracks dependencies automatically — it will only recompile changed translation units.

### Cleaning

```bash
make -C builds/pico clean      # remove compiled objects, keep CMake cache
# or wipe everything:
rm -rf builds/pico && cmake -B builds/pico -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico && make -j$(nproc) -C builds/pico
# or remove every builds/<board>_<variant>/ directory made by ./build:
./clean
```

Never run `rm -rf build` in the repository root — `build` is the build script.

---

## 6. Run the host-native test suites

The kernel subsystems ship with unit tests that run on your development machine — no Pico hardware, no cross-compiler, and no Pico SDK needed.  The tests compile `mem.c`, `sync.c`, `fs.c`, and `vfs.c` directly against the host `gcc` toolchain, using lightweight stubs for hardware and scheduler dependencies.

### Prerequisites

The host `gcc` toolchain and `cmake` are already installed from step 2.  No additional packages are required.

### Run

From the project root:

```bash
./build tests
```

This configures `builds/tests/` if necessary, compiles all four test binaries, and runs them via CTest.  A passing run looks like:

```
--- tests ---
Test project /home/user/picoOS/builds/tests
    Start 1: mem
1/4 Test #1: mem ..............................   Passed    0.03 sec
    Start 2: sync
2/4 Test #2: sync .............................   Passed    0.10 sec
    Start 3: fs
3/4 Test #3: fs ...............................   Passed    0.00 sec
    Start 4: vfs
4/4 Test #4: vfs ..............................   Passed    0.00 sec

100% tests passed, 0 tests failed out of 4
```

### What is tested

| Binary | Module | Cases | Coverage |
|--------|--------|-------|---------|
| `test_mem` | `src/kernel/mem.c` | 14 | Zero alloc, alignment, exhaustion, fragmentation, coalescing, random sizes |
| `test_sync` | `src/kernel/sync.c` | 37 | Spinlock, mutex, semaphore, event flags, message queue — non-blocking paths; `PICOOS_LOCK_DEBUG` deadlock detection |
| `test_fs` | `src/kernel/fs.c` | 17 | Format, CRUD, limits, name boundary, truncate, append, persistence, corruption |
| `test_vfs` | `src/kernel/vfs.c` | 10 | Device/file routing, FD exhaustion, double-close, mount table limits |

See **[docs/testing.md](testing.md)** for a full description of each test case and instructions for adding new ones.

### Clean the test build

```bash
./clean          # removes builds/tests along with all firmware build directories
```

---

## 7. Flash to the Pico

### Method A — drag and drop (no extra tools)

1. Hold the **BOOTSEL** button on the Pico.
2. While holding BOOTSEL, plug the USB cable into your machine.
3. Release BOOTSEL. The Pico mounts as a USB mass-storage device named **RPI-RP2**.
4. Copy the `.uf2` file to the drive (substitute the actual filename for your board):

```bash
cp builds/pico/src/picoos_D-v0.3.9.uf2 /media/$USER/RPI-RP2/
# macOS: cp builds/pico/src/picoos_D-v0.3.9.uf2 /Volumes/RPI-RP2/
```

5. The Pico unmounts and reboots into picoOS automatically.

### Method B — picotool (scriptable, no button required if already running picoOS)

Use the picotool built in [Install a matching picotool](#install-a-matching-picotool) (on your `PATH` through `~/.local/bin`).

With the Pico in BOOTSEL mode:

```bash
picotool load builds/pico/src/picoos_D-v0.3.9.uf2 --force
picotool reboot
```

### Method C — from the running shell (`update` command)

If picoOS is already running and you have the console open, the `update` command reboots the Pico directly into BOOTSEL mode without touching the button:

```
pico> update
```

The Pico will reappear as **RPI-RP2**.  Then copy the new `.uf2` as above.

---

## 8. Connect to the console

picoOS outputs a USB CDC serial port.  Once the Pico is running, a `/dev/ttyACM0` device (Linux) or `/dev/cu.usbmodem*` device (macOS) appears.

### Option A — host console tool (recommended)

The companion Python script in `tools/console.py` auto-detects the Pico by USB VID:PID and provides a polished interactive session.

Install the dependency:

```bash
pip install pyserial
```

Run:

```bash
python3 tools/console.py
```

Useful flags:

```bash
python3 tools/console.py --port /dev/ttyACM0   # force a specific port
python3 tools/console.py --log session.log      # save all output to a file
python3 tools/console.py --upload myfile.txt myfile.txt   # upload a file (names ≤ 15 chars, no directories)
python3 tools/console.py --list-ports           # show available serial ports
```

Press **Ctrl-C** to exit.

### Option B — any serial terminal

```bash
# Linux
screen /dev/ttyACM0 115200

# or
minicom -b 115200 -D /dev/ttyACM0

# macOS
screen /dev/cu.usbmodem* 115200
```

---

## 9. First boot

After flashing, the console should show something like:

```
=======================================================
picoOS  v0.3.9

  Platform : RP2040, dual ARM Cortex-M0+ (133 MHz max)
  Options  : DISPLAY_PACK
=======================================================

Threads created:
  TID 1  PID 1  pri 7  idle
  TID 2  PID 1  pri 7  idle1
  TID 3  PID 2  pri 2  shell

Starting scheduler...

pico>
```

The Options line lists the compiled-in features: `WiFi`, `Bluetooth` and `DISPLAY_PACK`, or `none`.  A `picow` build with a Display Pack shows `Options  : WiFi Bluetooth DISPLAY_PACK`, and its thread list starts with `wifi-poll` (TID 1, pri 6), followed by `idle`, `idle1` and `shell`.  On the very first boot the filesystem is formatted, which takes a moment.

Type `help` to list available shell commands.  To start an app automatically on every boot, add an `AUTORUN=<app>` line to `config.txt` — see [application.md](application.md#launching-an-app-automatically).

---

## 10. Editor / IDE setup

### clangd (VS Code, Neovim, etc.)

For full SDK-aware completions and diagnostics, point clangd at the compile database generated by CMake.  Configure with a WiFi board if you want the WiFi/BT code (and `scantest.c`) to resolve, since those files are only compiled for `picow` / `pico2w`:

```bash
cmake -B builds/picow -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=picow \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sf builds/picow/compile_commands.json compile_commands.json
```

Then open the project root in your editor.

### VS Code extensions

- **C/C++** or **clangd** — language server
- **CMake Tools** — configure and build from the IDE
- **Cortex-Debug** — GDB-based on-chip debugging (requires a Picoprobe or J-Link)

---

## 11. Debugging with GDB (optional)

You need a second Pico flashed as a [Picoprobe](https://github.com/raspberrypi/picoprobe) (or a J-Link / CMSIS-DAP adapter) connected to the target Pico's SWD pins (SWDIO, SWDCLK, GND).

Install OpenOCD with RP2040/RP2350 support:

```bash
sudo apt install openocd
```

In one terminal, start OpenOCD:

```bash
# RP2040 (pico / picow)
openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg

# RP2350 (pico2 / pico2w)
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg
```

In another terminal, launch GDB (substitute the actual ELF name for your board):

```bash
arm-none-eabi-gdb builds/pico/src/picoos_D-v0.3.9.elf
(gdb) target remote :3333
(gdb) monitor reset init
(gdb) continue
```

---

## 12. Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `PICO_SDK_PATH` not found during `cmake` | Environment variable not exported | `export PICO_SDK_PATH=...` and re-run cmake |
| `/dev/ttyACM0` permission denied | User not in `dialout` group | `sudo usermod -aG dialout $USER` then log out/in |
| Pico doesn't appear as RPI-RP2 | Cable is power-only | Try a different USB cable |
| Console output is garbled | Baud rate mismatch | picoOS uses 115200 — set your terminal to match |
| `make` fails on `arm-none-eabi-gcc` not found | Toolchain not installed | Follow step 2 |
| `picotool` can't find device | Pico not in BOOTSEL mode | Hold BOOTSEL while plugging in, or use `update` shell command |
| `picotool` finds the device but gets a permission error | No udev rule for the Pico | Install `60-picotool.rules` — see [Install a matching picotool](#install-a-matching-picotool) |
| `cmake` stops with *Incompatible picotool installation found* | A distro picotool of the wrong version is installed and `PICOTOOL_FETCH_FROM_GIT_PATH` is not set | Build the matching picotool and export `PICOTOOL_FETCH_FROM_GIT_PATH` — see [Install a matching picotool](#install-a-matching-picotool) |
| `bash: /usr/bin/picotool: No such file or directory` after removing the distro package | bash cached the old location | `hash -r`, or open a new terminal |
| No USB input on pico2_w | Stale build without M33 FPU fix | Ensure you are on the current branch and rebuild |
| `cmake -B build` fails, or `build` vanished | `build` is the build script, not a directory | Use another directory name (`builds/pico`) or `./build`; restore the script with `git checkout build` |
| AUTORUN app does not start | Name mismatch or line past byte 255 of `config.txt` | See [application.md](application.md#launching-an-app-automatically) |
