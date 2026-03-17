# Environment Setup, Build, and Flash Guide

This guide covers everything needed to go from a fresh machine to a running picoOS image on a Raspberry Pi Pico.

---

## 1. Hardware required

| Item | Notes |
|------|-------|
| Raspberry Pi Pico or Pico W | Any RP2040-based board works |
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

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

CMake will automatically locate the Pico SDK via `PICO_SDK_PATH` and pull in `pico_sdk_import.cmake`.

A successful build produces these files in `build/src/`:

| File | Purpose |
|------|---------|
| `picoos.uf2` | **Flash this** — UF2 image for drag-and-drop or `picotool` |
| `picoos.elf` | ELF with debug symbols (used by GDB) |
| `picoos.bin` | Raw binary |
| `picoos.hex` | Intel HEX format |
| `picoos.dis` | Disassembly listing |

### Incremental builds

After editing source files, just run `make` again from the `build/` directory:

```bash
cd build && make -j$(nproc)
```

CMake tracks dependencies automatically — it will only recompile changed translation units.

### Cleaning

```bash
cd build && make clean      # remove compiled objects, keep CMake cache
# or wipe everything:
rm -rf build/ && mkdir build && cd build && cmake .. && make -j$(nproc)
```

---

## 6. Flash to the Pico

### Method A — drag and drop (no extra tools)

1. Hold the **BOOTSEL** button on the Pico.
2. While holding BOOTSEL, plug the USB cable into your machine.
3. Release BOOTSEL. The Pico mounts as a USB mass-storage device named **RPI-RP2**.
4. Copy `build/src/picoos.uf2` to the drive:

```bash
cp build/src/picoos.uf2 /media/$USER/RPI-RP2/
# macOS: cp build/src/picoos.uf2 /Volumes/RPI-RP2/
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
picotool load build/src/picoos.uf2 --force
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

After flashing, the console should show:

```
=== picoOS ===
RP2040 dual-core educational OS
Build: Mar  9 2026 14:23:01

pico>
```

Type `help` to list available shell commands.

---

## 9. Editor / IDE setup

### clangd (VS Code, Neovim, etc.)

A `.clangd` file at the project root adds the correct include paths so the language server resolves kernel headers without needing the Pico SDK installed on the host.  For full SDK-aware completions and diagnostics, point clangd at the compile database generated by CMake:

```bash
cd build && cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -s build/compile_commands.json ../compile_commands.json
```

Then open the project root in your editor.

### VS Code extensions

- **C/C++** or **clangd** — language server
- **CMake Tools** — configure and build from the IDE
- **Cortex-Debug** — GDB-based on-chip debugging (requires a Picoprobe or J-Link)

---

## 10. Debugging with GDB (optional)

You need a second Pico flashed as a [Picoprobe](https://github.com/raspberrypi/picoprobe) (or a J-Link / CMSIS-DAP adapter) connected to the target Pico's SWD pins (SWDIO, SWDCLK, GND).

Install OpenOCD with RP2040 support:

```bash
sudo apt install openocd
```

In one terminal, start OpenOCD:

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg
```

In another terminal, launch GDB:

```bash
arm-none-eabi-gdb build/src/picoos.elf
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
