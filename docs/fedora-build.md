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

`libusb1-devel` and `pkg-config` are needed to build `picotool` with USB support from source in step 4.

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

## 4. Build picotool (required — the SDK uses it to make the UF2)

The pico-sdk converts every firmware ELF into a `.uf2` with `picotool uf2 convert`, so picotool is part of the build, not just a flashing tool.  The SDK insists on an **exact** picotool version: SDK 2.3.1 requires picotool 2.3.0.  For another SDK release, read the required version from the SDK:

```bash
grep -m1 picotool_VERSION_REQUIRED ~/pico-sdk/tools/CMakeLists.txt
#     set(picotool_VERSION_REQUIRED 2.3.0)
```

The SDK finds picotool through its CMake package config (`picotoolConfig.cmake`), **not** through `PATH`.  It looks in `$PICOTOOL_FETCH_FROM_GIT_PATH/picotool/` first, then the system CMake package paths (where a `dnf install picotool` puts it).  If neither has picotool at all, it downloads and builds one (without USB support) inside every build directory.  If it finds one of the wrong version, configuration stops with *"Incompatible picotool installation found"*.  A distro package only works if its version matches exactly (`dnf info picotool`); otherwise do not install it, and build from source below.

### Prerequisites

The build dependencies from step 1 (`libusb1-devel`, `pkg-config`), and the pico-sdk's mbedtls submodule (already present if you cloned the SDK with `--recursive`) — picotool uses it for signing and hashing:

```bash
cd ~/pico-sdk
git submodule update --init lib/mbedtls
```

### Clone and build

```bash
mkdir -p ~/workspace && cd ~/workspace
git clone --branch 2.3.0 https://github.com/raspberrypi/picotool.git picotool-src
cmake -S picotool-src -B picotool-src/build \
    -DPICO_SDK_PATH="$HOME/pico-sdk" \
    -DPICOTOOL_FLAT_INSTALL=1 \
    -DCMAKE_INSTALL_PREFIX="$HOME/workspace/picotool-install"
cmake --build picotool-src/build -j$(nproc)
cmake --install picotool-src/build
```

`PICOTOOL_FLAT_INSTALL=1` puts the binary and its CMake config side by side in `~/workspace/picotool-install/picotool/`.

### Make picotool visible to the pico-sdk and your shell

Add to `~/.bash_profile` (environment variables belong in the login file):

```bash
export PICOTOOL_FETCH_FROM_GIT_PATH="$HOME/workspace/picotool-install"
```

`PICOTOOL_FETCH_FROM_GIT_PATH` names the directory that *contains* `picotool/`.  (Exporting `PICOTOOL_DIR` does nothing: the SDK does not read it.)  The `./build` script sets this variable itself when `~/workspace/picotool-install/picotool` exists; a manual `cmake -B …` needs the export.

Put the binary on your `PATH` for flashing — Fedora already puts `~/.local/bin` on `PATH`:

```bash
mkdir -p ~/.local/bin
ln -sf ~/workspace/picotool-install/picotool/picotool ~/.local/bin/picotool
source ~/.bash_profile
hash -r            # forget any old picotool location bash cached
picotool version   # picotool v2.3.0 ...
```

When you update the SDK, rebuild picotool at the matching tag and remove the old `builds/` directories (`./clean`).

### Build without USB support (offline / CI)

If `libusb1-devel` is unavailable, add `-DPICOTOOL_NO_LIBUSB=1`. The resulting binary can still inspect and convert ELF/UF2 files (enough for the firmware build) but cannot communicate with a connected Pico over USB (`load`, `save`, `reboot` commands will be absent).

```bash
cmake -S picotool-src -B picotool-src/build \
    -DPICO_SDK_PATH="$HOME/pico-sdk" \
    -DPICOTOOL_FLAT_INSTALL=1 \
    -DPICOTOOL_NO_LIBUSB=1 \
    -DCMAKE_INSTALL_PREFIX="$HOME/workspace/picotool-install"
```

---

## 5. USB device permissions

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

So `picotool load`/`reboot` can reach the Pico without `sudo`, install picotool's own udev rules.  They cover the BOOTSEL product IDs of both chips (`2E8A:0003` RP2040, `2E8A:000F` RP2350) as well as the running-picoOS IDs:

```bash
sudo cp ~/workspace/picotool-src/udev/60-picotool.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

The rules grant access to the logged-in seat user (`uaccess`) and to the `plugdev` group.

---

## 6. Clone this repository

```bash
git clone <repo-url> picoOS
cd picoOS
```

---

## 7. Build

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

CMake automatically reads the `pico_sdk_import.cmake` at the project root, locates the ARM cross-compiler via `arm-none-eabi-gcc` in `PATH`, and sets `CMAKE_SYSTEM_PROCESSOR=arm` — no extra toolchain flags needed.

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
   cp build_pico/src/picoos_D-v0.3.9.uf2 /run/media/$USER/RPI-RP2/
   sync
   ```

   The Pico unmounts and reboots into picoOS automatically.

### Method B — picotool (built in step 4)

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

## 9. Connect to the console

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
python3 tools/console.py --log session.log      # save output
```

---

## 10. LSP / clangd (optional)

After a successful build, symlink the CMake compile database so clangd can resolve all Pico SDK headers:

```bash
cmake -B build_pico -DPICO_SDK_PATH="$HOME/pico-sdk" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sf build_pico/compile_commands.json compile_commands.json
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
| pico-sdk downloads and builds picotool during the firmware build | SDK found no picotool | Build picotool (step 4) and export `PICOTOOL_FETCH_FROM_GIT_PATH="$HOME/workspace/picotool-install"` in `~/.bash_profile` |
| `cmake` stops with *Incompatible picotool installation found* | A picotool of the wrong version (e.g. a distro package) was found first | Build the version the SDK requires (step 4) and export `PICOTOOL_FETCH_FROM_GIT_PATH`, or remove the distro package |
| `picotool: command not found` after install | Binary not linked into `~/.local/bin` | `ln -sf ~/workspace/picotool-install/picotool/picotool ~/.local/bin/picotool` |
| `bash: …/picotool: No such file or directory` after moving or removing picotool | bash cached the old location | `hash -r`, or open a new terminal |
