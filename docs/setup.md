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
    git
```

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

Set the environment variable so CMake and the SDK import script can find it.  Add this line to your `~/.bashrc` (or `~/.zshrc`):

```bash
export PICO_SDK_PATH="$HOME/pico-sdk"
```

Reload your shell:

```bash
source ~/.bashrc
```

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

| `-DPICO_BOARD=` | Board | Chip | WiFi |
|----------------|-------|------|------|
| `pico` | Raspberry Pi Pico | RP2040 | No |
| `pico2` | Raspberry Pi Pico 2 | RP2350 | No |
| `picow` | Raspberry Pi Pico W | RP2040 | Yes |
| `pico2w` | Raspberry Pi Pico 2 W | RP2350 | Yes |

If `-DPICO_BOARD` is omitted, CMake defaults to `pico`.

### Configure and build

```bash
# Configure (replace pico with your target board)
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico

# Build
make -j$(nproc) -C build
```

A successful build produces these files in `build/src/`, named after the board and version:

| File | Purpose |
|------|---------|
| `<board>os-v<ver>.uf2` | **Flash this** — UF2 image for drag-and-drop or `picotool` |
| `<board>os-v<ver>.elf` | ELF with debug symbols (used by GDB) |
| `<board>os-v<ver>.bin` | Raw binary |
| `<board>os-v<ver>.dis` | Disassembly listing |
| `<board>os-v<ver>.elf.map` | Linker map (used by `mem_report.py`) |

For example, a `pico` build at version 0.1.4 produces `picoos-v0.1.4.uf2`.  A `pico2w` build produces `pico2wos-v0.1.4.uf2`.  All board variants can safely share the same `build/src/` output directory.

### Build options (optional)

The Display Pack drivers are ON by default.  Override on the CMake command line:

```bash
# No Display Pack hardware attached
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico \
      -DPICOOS_DISPLAY_ENABLE=OFF

# Display Pack present but suppress shell commands
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico \
      -DPICOOS_DISPLAY_SHELL=OFF -DPICOOS_LED_SHELL=OFF
```

| Flag | Default | Effect |
|------|---------|--------|
| `PICOOS_DISPLAY_ENABLE` | ON | ST7789 driver + `/dev/display` (~32 KB framebuffer) |
| `PICOOS_DISPLAY_SHELL` | ON | `display` shell command |
| `PICOOS_LED_ENABLE` | ON | RGB LED driver + `/dev/led` |
| `PICOOS_LED_SHELL` | ON | `led` shell command |

Setting `PICOOS_DISPLAY_ENABLE=OFF` cascades OFF to all sub-features.  Setting any sub-feature ON automatically enables `PICOOS_DISPLAY_ENABLE`.

### Incremental builds

After editing source files, just run `make` again:

```bash
make -j$(nproc) -C build
```

CMake tracks dependencies automatically — it will only recompile changed translation units.

### Cleaning

```bash
make -C build clean      # remove compiled objects, keep CMake cache
# or wipe everything:
rm -rf build && cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico && make -j$(nproc) -C build
```

---

## 6. Flash to the Pico

### Method A — drag and drop (no extra tools)

1. Hold the **BOOTSEL** button on the Pico.
2. While holding BOOTSEL, plug the USB cable into your machine.
3. Release BOOTSEL. The Pico mounts as a USB mass-storage device named **RPI-RP2**.
4. Copy the `.uf2` file to the drive (substitute the actual filename for your board):

```bash
cp build/src/picoos-v0.1.4.uf2 /media/$USER/RPI-RP2/
# macOS: cp build/src/picoos-v0.1.4.uf2 /Volumes/RPI-RP2/
```

5. The Pico unmounts and reboots into picoOS automatically.

### Method B — picotool (scriptable, no button required if already running picoOS)

Install `picotool`:

```bash
sudo apt install picotool
# or build from source: https://github.com/raspberrypi/picotool
```

With the Pico in BOOTSEL mode:

```bash
picotool load build/src/picoos-v0.1.4.uf2 --force
picotool reboot
```

### Method C — from the running shell (`update` command)

If picoOS is already running and you have the console open, the `update` command reboots the Pico directly into BOOTSEL mode without touching the button:

```
pico> update
```

The Pico will reappear as **RPI-RP2**.  Then copy the new `.uf2` as above.

---

## 7. Connect to the console

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
python3 tools/console.py --upload myfile.txt /myfile.txt  # upload a file
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

## 8. First boot

After flashing, the console should show something like:

```
=======================================================
picoOS  v0.1.4

  Platform : RP2040, dual ARM Cortex-M0+ (133 MHz max)
  Options  : none
=======================================================

Threads created:
  TID 1  PID 1  pri 7  idle
  TID 2  PID 2  pri 2  shell

Starting scheduler...

pico>
```

On a `picow` or `pico2w` build the Options line will include `WiFi`:

```
  Options  : WiFi
```

Type `help` to list available shell commands.

---

## 9. Editor / IDE setup

### clangd (VS Code, Neovim, etc.)

For full SDK-aware completions and diagnostics, point clangd at the compile database generated by CMake:

```bash
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -s build/compile_commands.json compile_commands.json
```

Then open the project root in your editor.

### VS Code extensions

- **C/C++** or **clangd** — language server
- **CMake Tools** — configure and build from the IDE
- **Cortex-Debug** — GDB-based on-chip debugging (requires a Picoprobe or J-Link)

---

## 10. Debugging with GDB (optional)

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
arm-none-eabi-gdb build/src/picoos-v0.1.4.elf
(gdb) target remote :3333
(gdb) monitor reset init
(gdb) continue
```

---

## 11. Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `PICO_SDK_PATH` not found during `cmake` | Environment variable not exported | `export PICO_SDK_PATH=...` and re-run cmake |
| `/dev/ttyACM0` permission denied | User not in `dialout` group | `sudo usermod -aG dialout $USER` then log out/in |
| Pico doesn't appear as RPI-RP2 | Cable is power-only | Try a different USB cable |
| Console output is garbled | Baud rate mismatch | picoOS uses 115200 — set your terminal to match |
| `make` fails on `arm-none-eabi-gcc` not found | Toolchain not installed | Follow step 2 |
| `picotool` can't find device | Pico not in BOOTSEL mode | Hold BOOTSEL while plugging in, or use `update` shell command |
| No USB input on pico2_w | Stale build without M33 FPU fix | Ensure you are on the current branch and rebuild |
