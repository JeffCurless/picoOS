# picoOS — Student Build Guide (Fedora)

This guide walks you through cloning, building, flashing, and connecting to picoOS on your Fedora lab machine. The toolchain (ARM cross-compiler, CMake, Git) is already installed.

---

## 1. Get the Pico SDK

Clone the SDK into your home directory:

```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk
git submodule update --init --recursive
```

Export the path so CMake can find it. Add this line to `~/.bashrc`:

```bash
export PICO_SDK_PATH="$HOME/pico-sdk"
```

Reload your shell:

```bash
source ~/.bashrc
```

---

## 2. Build picotool (optional — for scriptable flashing)

`picotool` allows you to flash the Pico without pressing the BOOTSEL button each time. This step is optional — drag-and-drop flashing works without it.

```bash
git clone https://github.com/raspberrypi/picotool.git ~/picotool
cd ~/picotool
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk"
cmake --build build -j$(nproc)
sudo cmake --install build          # installs to /usr/local/bin
```

---

## 3. USB device permissions

The Pico appears as two different USB devices depending on mode:

| Mode | VID:PID | Notes |
|---|---|---|
| BOOTSEL (flash mode) | `2E8A:0003` | Mass storage — accessible without extra rules |
| Running picoOS | `2E8A:000A` (RP2040) / `2E8A:0009` (RP2350) | USB CDC serial — needs `dialout` group |

Add your user to the `dialout` group:

```bash
sudo usermod -aG dialout $USER
```

Log out and back in for the group change to take effect.

If you built `picotool` in step 2, also add a udev rule so it can access the Pico without `sudo`:

```bash
sudo bash -c 'cat > /etc/udev/rules.d/99-pico.rules <<EOF
# Raspberry Pi Pico — BOOTSEL mode
SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="0003", MODE="0666"
# Raspberry Pi Pico — CDC serial while running picoOS (any Raspberry Pi product ID)
SUBSYSTEM=="tty", ATTRS{idVendor}=="2e8a", MODE="0666", GROUP="dialout"
EOF'
sudo udevadm control --reload-rules
sudo udevadm trigger
```

---

## 4. Clone the repository

```bash
cd workspace
git clone https://github.com/JeffCurless/picoOS.git
cd picoOS
```

---

## 5. Build

To build every board and display variant at once (images are copied to `kits/`):

```bash
./build            # or: ./build pico | ./build pico2 | ./build wifi
```

Or build one board by hand.  Add `-DPICO_BOARD=picow` (or `pico2`, `pico2w`) for another
board; the default is `pico`.  Don't use `build` as the directory name — it is the
build script:

```bash
cmake -B build_pico -DPICO_SDK_PATH="$HOME/pico-sdk"
make -j$(nproc) -C build_pico
```

CMake automatically reads `pico_sdk_import.cmake` at the project root, locates the ARM cross-compiler via `arm-none-eabi-gcc` in `PATH`, and sets `CMAKE_SYSTEM_PROCESSOR=arm` — no extra toolchain flags needed.

A successful build produces these files in `build_pico/src/`, named after the board,
display variant, and version (e.g. `picoos_D-v0.3.9.*` for a pico + Display Pack build):

| File | Purpose |
|---|---|
| `<board>os[_D|_D2]-v<ver>.uf2` | **Flash this** — UF2 image for drag-and-drop or picotool |
| `<board>os[_D|_D2]-v<ver>.elf` | ELF with debug symbols (for GDB) |
| `<board>os[_D|_D2]-v<ver>.bin` | Raw binary |
| `<board>os[_D|_D2]-v<ver>.hex` | Intel HEX |
| `<board>os[_D|_D2]-v<ver>.dis` | Disassembly listing |
| `<board>os[_D|_D2]-v<ver>.elf.map` | Linker map (for `tools/mem_report.py`) |

### Incremental builds

After editing source files, just run:

```bash
make -j$(nproc) -C build_pico
```

### Clean rebuild

```bash
rm -rf build_pico      # never "rm -rf build" — that is the build script
cmake -B build_pico -DPICO_SDK_PATH="$HOME/pico-sdk"
make -j$(nproc) -C build_pico
```

---

## 6. Flash the UF2

### Method A — drag and drop (no extra tools needed)

1. Hold **BOOTSEL** on the Pico and plug in USB. Release BOOTSEL.
2. The Pico mounts as `RPI-RP2` — find the mount point:

   ```bash
   lsblk -o NAME,LABEL,MOUNTPOINT | grep RPI-RP2
   ```

   Fedora auto-mounts removable drives under `/run/media/$USER/RPI-RP2`.

3. Copy the UF2:

   ```bash
   cp build_pico/src/picoos_D-v0.3.9.uf2 /run/media/$USER/RPI-RP2/
   sync
   ```

   The Pico unmounts and reboots into picoOS automatically.

### Method B — picotool (if built in step 2)

With the Pico in BOOTSEL mode:

```bash
picotool load build_pico/src/picoos_D-v0.3.9.uf2 --force
picotool reboot
```

### Method C — from the running shell

If picoOS is already running, the `update` shell command reboots directly into BOOTSEL mode:

```
pico> update
```

Then copy the UF2 as in Method A.

---

## 7. Connect to the console

Install pyserial for the companion console tool:

```bash
pip3 install --user pyserial
```

Run:

```bash
python3 tools/console.py
```

The script auto-detects the Pico by USB VID:PID (`2E8A:000A` on RP2040, `2E8A:0009` on RP2350). Press **Ctrl-C** to exit.

```bash
python3 tools/console.py --port /dev/ttyACM0   # specific port
python3 tools/console.py --log session.log      # save output to a file
```

---

## 8. LSP / clangd (optional)

After a successful build, symlink the CMake compile database so clangd can resolve all Pico SDK headers in your editor:

```bash
cmake -B build_pico -DPICO_SDK_PATH="$HOME/pico-sdk" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sf build_pico/compile_commands.json compile_commands.json
```

---

## 9. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `/dev/ttyACM0: Permission denied` | Not in `dialout` group | `sudo usermod -aG dialout $USER` then log out/in |
| Pico not found by `picotool` | udev rules not loaded | Re-run `udevadm` commands from step 3 |
| `RPI-RP2` not mounted | Fedora auto-mount disabled | `udisksctl mount -b /dev/sdX` or check `dmesg` |
| `PICO_SDK_PATH` not found during `cmake` | Environment variable not set | `export PICO_SDK_PATH="$HOME/pico-sdk"` and re-run |
| Pico doesn't appear as RPI-RP2 | Cable is power-only | Try a different USB cable |
| Console output is garbled | Baud rate mismatch | picoOS uses 115200 — set your terminal to match |
