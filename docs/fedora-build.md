# Building picoOS on Intel Fedora (Cross-Compile)

This guide covers everything needed to cross-compile picoOS on an x86-64 Fedora Linux system and produce a flashable UF2 image for the Raspberry Pi Pico (RP2040).

---

## 1. Install host build dependencies

```bash
sudo dnf install -y \
    cmake \
    make \
    gcc \
    g++ \
    git \
    python3 \
    python3-pip \
    libusb1-devel \
    pkg-config \
    ninja-build
```

`libusb1-devel` and `pkg-config` are needed to build `picotool` (the flash utility) from source in step 4.

---

## 2. Install the ARM bare-metal cross-compiler

### Option A — Fedora DNF packages (recommended, simplest)

```bash
sudo dnf install -y \
    arm-none-eabi-gcc-cs \
    arm-none-eabi-gcc-cs-c++ \
    arm-none-eabi-binutils-cs \
    arm-none-eabi-newlib
```

Verify:

```bash
arm-none-eabi-gcc --version
# arm-none-eabi-gcc (Fedora 13.x ...) 13.x.x ...
```

The Pico SDK requires GCC 10 or later. Fedora ships GCC 12–14 in these packages depending on the Fedora release; all are compatible.

### Option B — Official ARM GNU Toolchain tar.gz (latest)

Use this if the DNF packages are too old or missing on your Fedora version.

1. Download the **AArch32 bare-metal** tarball from the ARM Developer site:

   ```
   https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
   ```

   Select: **arm-gnu-toolchain-XX.X-x86_64-arm-none-eabi.tar.xz**

2. Extract and add to `PATH`:

   ```bash
   sudo tar -xJf arm-gnu-toolchain-*.tar.xz -C /opt
   # Adjust the directory name to match what was extracted:
   echo 'export PATH="/opt/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin:$PATH"' >> ~/.bashrc
   source ~/.bashrc
   ```

3. Verify:

   ```bash
   arm-none-eabi-gcc --version
   ```

---

## 3. Get the Pico SDK

Clone the SDK into a permanent location:

```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk
git submodule update --init --recursive
```

Export the path so CMake can find it. Add this to `~/.bashrc`:

```bash
export PICO_SDK_PATH="$HOME/pico-sdk"
```

Reload:

```bash
source ~/.bashrc
```

---

## 4. Build picotool (optional — for scriptable flashing)

This step is optional — you can flash without it using BOOTSEL drag-and-drop (Method A in step 8).

### Check the Fedora package first

Recent Fedora releases may ship picotool directly:

```bash
dnf info picotool 2>/dev/null && sudo dnf install -y picotool
```

If that succeeds, skip to step 5.  If the package is not found, build from source below.

### Build from source

#### Prerequisites

Ensure the build dependencies from step 1 are installed (`libusb1-devel`, `pkg-config`), then initialize the pico-sdk's mbedtls submodule — picotool requires it for binary signing and hashing:

```bash
cd ~/pico-sdk
git submodule update --init lib/mbedtls
```

#### Clone and build

```bash
git clone https://github.com/raspberrypi/picotool.git ~/picotool
cd ~/picotool
cmake -B build \
    -DPICO_SDK_PATH="$HOME/pico-sdk" \
    -DPICOTOOL_FLAT_INSTALL=1 \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j$(nproc)
cmake --install build
```

`PICOTOOL_FLAT_INSTALL=1` places the binary and its CMake config file together under `$HOME/.local/picotool/`, which makes it straightforward to point the pico-sdk at it in the next step.

#### Make picotool visible to the pico-sdk

Add both of the following to `~/.bashrc`:

```bash
export PATH="$HOME/.local/picotool:$PATH"
export PICOTOOL_DIR="$HOME/.local/picotool"
```

`PICOTOOL_DIR` tells the pico-sdk's `Findpicotool.cmake` exactly where to find the installed CMake config file, preventing it from auto-fetching a second copy during your firmware builds.

Reload:

```bash
source ~/.bashrc
```

Verify:

```bash
picotool version
# picotool v2.x.x (x86_64-pc-linux-gnu, ...)
```

#### Build without USB support (offline / CI)

If `libusb1-devel` is unavailable, add `-DPICOTOOL_NO_LIBUSB=1`. The resulting binary can still inspect and convert ELF/UF2 files but cannot communicate with a connected Pico over USB (`load`, `save`, `reboot` commands will be absent).

```bash
cmake -B build \
    -DPICO_SDK_PATH="$HOME/pico-sdk" \
    -DPICOTOOL_FLAT_INSTALL=1 \
    -DPICOTOOL_NO_LIBUSB=1 \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local"
```

---

## 5. USB device permissions

The Pico appears as two different USB devices depending on mode:

| Mode | VID:PID | Notes |
|---|---|---|
| BOOTSEL (flash mode) | `2E8A:0003` | Mass storage — accessible without extra rules |
| Running picoOS | `2E8A:000A` | USB CDC serial — needs `dialout` group |

Add your user to the `dialout` group:

```bash
sudo usermod -aG dialout $USER
```

Log out and back in for the group change to take effect.

If you installed `picotool`, also add a udev rule so it can access the Pico without `sudo`:

```bash
sudo bash -c 'cat > /etc/udev/rules.d/99-pico.rules <<EOF
# Raspberry Pi Pico — BOOTSEL mode
SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="0003", MODE="0666"
# Raspberry Pi Pico — CDC serial (running)
SUBSYSTEM=="tty", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="000a", MODE="0666", GROUP="dialout"
EOF'
sudo udevadm control --reload-rules
sudo udevadm trigger
```

---

## 6. Clone this repository

```bash
git clone <repo-url> picoOS
cd picoOS
```

---

## 7. Build

```bash
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk"
make -j$(nproc) -C build
```

CMake automatically reads the `pico_sdk_import.cmake` at the project root, locates the ARM cross-compiler via `arm-none-eabi-gcc` in `PATH`, and sets `CMAKE_SYSTEM_PROCESSOR=arm` — no extra toolchain flags needed.

A successful build produces these files in `build/src/`:

| File | Purpose |
|---|---|
| `picoos.uf2` | **Flash this** — UF2 image for drag-and-drop or picotool |
| `picoos.elf` | ELF with debug symbols (for GDB) |
| `picoos.bin` | Raw binary |
| `picoos.hex` | Intel HEX |
| `picoos.dis` | Disassembly listing |

### Incremental builds

```bash
make -j$(nproc) -C build
```

### Clean rebuild

```bash
rm -rf build
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk"
make -j$(nproc) -C build
```

---

## 8. Flash the UF2

### Method A — drag and drop (no extra tools needed)

1. Hold **BOOTSEL** on the Pico and plug in USB. Release BOOTSEL.
2. The Pico mounts as `RPI-RP2` — find the mount point:

   ```bash
   lsblk -o NAME,LABEL,MOUNTPOINT | grep RPI-RP2
   ```

   Fedora auto-mounts removable drives under `/run/media/$USER/RPI-RP2`.

3. Copy the UF2:

   ```bash
   cp build/src/picoos.uf2 /run/media/$USER/RPI-RP2/
   sync
   ```

   The Pico unmounts and reboots into picoOS automatically.

### Method B — picotool (if built in step 4)

With the Pico in BOOTSEL mode:

```bash
picotool load build/src/picoos.uf2 --force
picotool reboot
```

### Method C — from the running shell

If picoOS is already running, the `update` shell command reboots directly into BOOTSEL mode:

```
pico> update
```

Then copy the UF2 as in Method A.

---

## 9. Connect to the console

Install pyserial for the companion console tool:

```bash
pip3 install --user pyserial
```

Run:

```bash
python3 tools/console.py
```

The script auto-detects the Pico by USB VID:PID `2E8A:000A`. Press **Ctrl-C** to exit.

```bash
python3 tools/console.py --port /dev/ttyACM0   # specific port
python3 tools/console.py --log session.log      # save output
```

---

## 10. LSP / clangd (optional)

After a successful build, symlink the CMake compile database so clangd can resolve all Pico SDK headers:

```bash
ln -sf build/compile_commands.json compile_commands.json
```

---

## 11. Troubleshooting (Fedora-specific)

| Symptom | Likely cause | Fix |
|---|---|---|
| `arm-none-eabi-gcc: command not found` | Package not installed or not in PATH | Re-run step 2; check `echo $PATH` |
| `cmake` fails: `C compiler cannot compile` | Wrong GCC or missing newlib | Verify `arm-none-eabi-newlib` is installed |
| `/dev/ttyACM0: Permission denied` | Not in `dialout` group | `sudo usermod -aG dialout $USER` then log out/in |
| Pico not found by `picotool` | udev rules not loaded | Re-run `udevadm` commands from step 5 |
| `RPI-RP2` not mounted | Fedora auto-mount disabled | `udisksctl mount -b /dev/sdX` or check `dmesg` |
| `cmake` version too old | Fedora version ships cmake < 3.13 | `sudo dnf install cmake` or use `dnf module enable cmake` |
| Build fails: missing `libusb` | Only affects picotool build | `sudo dnf install libusb1-devel` |
| `picotool` build: `mbedtls` errors | mbedtls submodule not fetched | `cd ~/pico-sdk && git submodule update --init lib/mbedtls` |
| pico-sdk fetches picotool during build | `PICOTOOL_DIR` not set | Add `export PICOTOOL_DIR="$HOME/.local/picotool"` to `~/.bashrc` |
| `picotool: command not found` after install | `~/.local/picotool` not in PATH | Add `export PATH="$HOME/.local/picotool:$PATH"` to `~/.bashrc` |
