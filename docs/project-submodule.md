# Building a Project on picoOS

This tutorial walks through creating a new project that uses picoOS as its operating system via a git submodule.  Your application code lives in your own repository; the kernel, shell, and drivers live in picoOS and are pulled in at build time.

---

## How it works

picoOS is added to your project as a **git submodule** — a pinned reference to a specific picoOS commit.  This means:

- Your project always builds against a known-good kernel.
- You can pull upstream picoOS fixes with a single command.
- If you ever need a kernel change, you do it on a named feature branch in picoOS and update the pointer.  That workflow is covered at the end of this document.

Your project supplies two things picoOS does not: an **app table** (the list of applications the shell can launch) and the **application source files** themselves.  The kernel, shell, and drivers are compiled from the picoOS submodule unchanged.

---

## Prerequisites

- Raspberry Pi Pico SDK installed (tested with SDK 1.5+).  Set `PICO_SDK_PATH` to its location, or adjust the path in the build script.
- `cmake` 3.13+, `make`, `gcc-arm-none-eabi`, `python3`.
- A GitHub account (or any git remote) to host your project repo.

---

## Step 1 — Create the project repository

```bash
mkdir myproject
cd myproject
git init
```

Create a `.gitignore` so build directories and generated images are not tracked:

```
build_*/
kits/
```

---

## Step 2 — Add picoOS as a submodule

```bash
git submodule add https://github.com/JeffCurless/picoOS.git picoOS
git submodule update --init
```

Copy the Pico SDK import helper to your project root (it must be present before CMake's `project()` call):

```bash
cp picoOS/pico_sdk_import.cmake .
```

Your project now looks like this:

```
myproject/
├── .gitmodules
├── .gitignore
├── pico_sdk_import.cmake
└── picoOS/               ← submodule
```

---

## Step 3 — Add your application files

Create an `apps/` directory for your application code:

```bash
mkdir apps
```

### `apps/myapp.h`

Declare your application entry function.  Every picoOS application has the same signature: a single `void *arg` parameter that is currently unused but reserved for future use.

```c
#ifndef APPS_MYAPP_H
#define APPS_MYAPP_H

void myapp_entry(void *arg);

#endif
```

### `apps/myapp.c`

Implement the entry function.  The application runs as its own process; it can loop forever, call `sys_exit()`, or be killed from the shell with `kill` / `killproc`.

```c
#include "kernel/syscall.h"
#include "shell/shell.h"

void myapp_entry(void *arg)
{
    (void)arg;
    shell_println("myapp: started");

    for (;;) {
        shell_println("myapp: tick");
        sys_sleep(1000);     /* sleep 1 second */
    }
}
```

The headers are resolved via the `picoOS/src` include path, which picoOS adds automatically.  Available kernel APIs:

| Header | What it provides |
|--------|-----------------|
| `kernel/syscall.h` | `sys_sleep(ms)`, `sys_yield()`, `sys_exit(code)`, `sys_getpid()`, `sys_gettid()` |
| `kernel/sync.h` | Semaphores (`ksemaphore_t`), mutexes (`kmutex_t`), message queues (`mqueue_t`) |
| `kernel/vfs.h` | File I/O — `vfs_open/read/write/close` |
| `kernel/dev.h` | Device access — `dev_ioctl()`, device IDs (`DEV_DISPLAY`, etc.) |
| `drivers/display.h` | Display drawing API (only when `PICOOS_DISPLAY_ENABLE=ON`) |
| `drivers/led.h` | RGB LED API (only when `PICOOS_LED_ENABLE=ON`) |
| `shell/shell.h` | `shell_print(fmt, ...)`, `shell_println(str)` |

### `apps/app_table.c`

This file tells picoOS which applications exist and what priority to run them at.  The shell `run <name>` command looks up entries in this table.

```c
#include "apps/app_table.h"   /* app_entry_t — resolved via picoOS/src */
#include "myapp.h"

const app_entry_t app_table[] = {
    { "myapp", myapp_entry, 4u },
};

const int app_table_size = (int)(sizeof(app_table) / sizeof(app_table[0]));
```

Priority runs from 0 (highest) to 7 (lowest).  The kernel idle thread runs at 7; shell threads run at 2–3; user apps typically use 4–5.

To add more applications, add their entry functions to the `app_table[]` array and list their source files in `CMakeLists.txt` (see Step 4).

---

## Step 4 — Create the project `CMakeLists.txt`

Your root `CMakeLists.txt` replaces picoOS's own root for the project build.  It calls `add_subdirectory(picoOS/src)` directly — **not** `add_subdirectory(picoOS)` — because picoOS's root calls `project()` and `pico_sdk_init()`, which must not run twice.

The board alias block (the five `if/elseif` lines) is a short duplication from picoOS's root; it normalises friendly board names like `picow` into the SDK-canonical `pico_w`.

```cmake
cmake_minimum_required(VERSION 3.13)

# ---------------------------------------------------------------------------
# Board name aliases — mirrors picoOS/CMakeLists.txt.
# Required here because we call add_subdirectory(picoOS/src) directly,
# bypassing picoOS's root.
# ---------------------------------------------------------------------------
if(PICO_BOARD STREQUAL "picow")
    set(PICO_BOARD "pico_w" CACHE STRING "" FORCE)
elseif(PICO_BOARD STREQUAL "pico2w")
    set(PICO_BOARD "pico2_w" CACHE STRING "" FORCE)
endif()

include(pico_sdk_import.cmake)

project(myproject C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ---------------------------------------------------------------------------
# Application injection
#
# PICOOS_INCLUDE_DEMO_APPS defaults OFF here so the project owns the app
# table.  Pass -DPICOOS_INCLUDE_DEMO_APPS=ON on the cmake command line to
# substitute the picoOS built-in demo apps instead (see "Building with demo
# apps" below).
# ---------------------------------------------------------------------------
if(NOT PICOOS_INCLUDE_DEMO_APPS)
    set(PICOOS_INCLUDE_DEMO_APPS OFF)

    # Absolute paths are required because picoOS/src resolves relative paths
    # against its own source directory, not the project root.
    set(PICOOS_APP_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/app_table.c
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/myapp.c
    )

    set(PICOOS_APP_INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/apps
    )
endif()

add_subdirectory(picoOS/src)
```

When you add a new application file (e.g. `apps/otherapp.c`), append it to `PICOOS_APP_SOURCES` and add its entry to `apps/app_table.c`.

---

## Step 5 — Create the `build` script

```bash
#!/usr/bin/bash
# build — compile firmware variants and collect .uf2 images into kits/
#
# Usage:
#   ./build           build all 12 variants (4 boards × 3 display configs)
#   ./build pico      build pico + picow only
#   ./build pico2     build pico2 + pico2w only
#
# Set PICO_SDK_PATH if the SDK is not at ~/workspace/pico-sdk.

SDK="${PICO_SDK_PATH:-$HOME/workspace/pico-sdk}"
JOBS="$(nproc)"

case "${1:-all}" in
    all)   BOARDS="pico picow pico2 pico2w" ;;
    pico)  BOARDS="pico picow" ;;
    pico2) BOARDS="pico2 pico2w" ;;
    *)     echo "Usage: $0 [all|pico|pico2]" >&2; exit 1 ;;
esac

mkdir -p kits

build_variant() {
    local board="$1" disp="$2" flags="$3"
    local dir="build_${board}_${disp}"
    echo "--- ${board} / ${disp} ---"
    cmake -B "$dir" -DPICO_SDK_PATH="$SDK" -DPICO_BOARD="$board" $flags || return 1
    make -j"$JOBS" -C "$dir"                                              || return 1
    cp -f "$dir"/picoOS/src/*.uf2 kits/
}

for board in $BOARDS; do
    build_variant "$board" ND  "-DPICOOS_DISPLAY_ENABLE=OFF"
    build_variant "$board" D   "-DPICOOS_DISPLAY_ENABLE=ON  -DPICOOS_DISPLAY_PACK2=OFF"
    build_variant "$board" D2  "-DPICOOS_DISPLAY_ENABLE=ON  -DPICOOS_DISPLAY_PACK2=ON"
done

echo ""
echo "Done. kits/ contents:"
ls -1 kits/*.uf2
```

Make it executable:

```bash
chmod +x build
```

The output `.uf2` files land in `build_${board}_${disp}/picoOS/src/` (CMake nests the subdirectory under the picoOS path) and are then copied to `kits/`.  The naming follows picoOS convention — board name + display suffix + version — for example `picoos_D2-v0.1.9.uf2`.

---

## Step 6 — Create the `clean` script

```bash
#!/usr/bin/bash
# clean — remove all variant build directories

mkdir -p kits
for board in pico picow pico2 pico2w; do
    for disp in ND D D2; do
        dir="build_${board}_${disp}"
        [ -d "$dir" ] && echo "Cleaning $dir..." && rm -rf "$dir"
    done
done
```

```bash
chmod +x clean
```

---

## Step 7 — First build and flash

Build for a single board to verify everything links:

```bash
./build pico
```

cmake and make run for three variants (no display, Display Pack, Display Pack 2).  The resulting images appear in `kits/`.

Flash the appropriate `.uf2` to your Pico:

1. Hold **BOOTSEL** while plugging in USB.  The Pico mounts as a mass-storage drive.
2. Copy the `.uf2` to the drive:
   ```bash
   cp kits/picoos_D-v0.1.9.uf2 /media/$USER/RPI-RP2/
   ```
3. The Pico reboots automatically.

Connect to the shell via USB serial (115200 baud, or use `tools/console.py` from the picoOS repo).  At the `>` prompt:

```
> run myapp
myapp: started
myapp: tick
myapp: tick
...
```

Kill the app with `killproc <pid>` (find the PID with `ps`).

---

## Building with the picoOS built-in demo apps

The picoOS repo ships three demonstration apps — **producer**, **consumer**, and **sensor** — that exercise inter-process communication using message queues and semaphores.  These are useful for smoke-testing the kernel on new hardware or as reference code.

To build your project firmware with the demo apps instead of your own app table, pass `-DPICOOS_INCLUDE_DEMO_APPS=ON`:

```bash
cmake -B build_pico_demo \
      -DPICO_SDK_PATH="$HOME/workspace/pico-sdk" \
      -DPICO_BOARD=pico \
      -DPICOOS_DISPLAY_ENABLE=ON \
      -DPICOOS_INCLUDE_DEMO_APPS=ON
make -j$(nproc) -C build_pico_demo
```

When `PICOOS_INCLUDE_DEMO_APPS=ON`:
- `picoOS/src/apps/demo.c` is compiled (it defines `app_table[]` with the three demo apps).
- Your project's `apps/app_table.c` is **not** compiled (the `if(NOT PICOOS_INCLUDE_DEMO_APPS)` block in your `CMakeLists.txt` is skipped).
- The shell `run` command will offer `producer`, `consumer`, and `sensor`.

This also works on a Pico W with a display attached — the `cray-one` app (a WiFi + display demo) is automatically included:

```bash
cmake -B build_picow_demo \
      -DPICO_SDK_PATH="$HOME/workspace/pico-sdk" \
      -DPICO_BOARD=picow \
      -DPICOOS_DISPLAY_ENABLE=ON \
      -DPICOOS_INCLUDE_DEMO_APPS=ON
```

To add a `demo` target to your `build` script so you can run `./build demo`, append a case to the script's `case` statement:

```bash
case "${1:-all}" in
    all)   BOARDS="pico picow pico2 pico2w" ;;
    pico)  BOARDS="pico picow" ;;
    pico2) BOARDS="pico2 pico2w" ;;
    demo)
        # Build one demo variant per board family for hardware bring-up
        cmake -B build_pico_demo  -DPICO_SDK_PATH="$SDK" -DPICO_BOARD=pico  \
              -DPICOOS_DISPLAY_ENABLE=ON -DPICOOS_INCLUDE_DEMO_APPS=ON
        make -j"$JOBS" -C build_pico_demo
        cmake -B build_pico2_demo -DPICO_SDK_PATH="$SDK" -DPICO_BOARD=pico2 \
              -DPICOOS_DISPLAY_ENABLE=ON -DPICOOS_INCLUDE_DEMO_APPS=ON
        make -j"$JOBS" -C build_pico2_demo
        cp -f build_*_demo/picoOS/src/*.uf2 kits/
        exit 0
        ;;
    *)     echo "Usage: $0 [all|pico|pico2|demo]" >&2; exit 1 ;;
esac
```

---

## Adding more applications

1. **Create the source file** — `apps/newapp.c` with a `void newapp_entry(void *arg)` function.
2. **Create the header** — `apps/newapp.h` declaring `void newapp_entry(void *arg)`.
3. **Register it** — add a row to `app_table[]` in `apps/app_table.c`:
   ```c
   { "newapp", newapp_entry, 4u },
   ```
4. **Add it to the build** — append the source file to `PICOOS_APP_SOURCES` in `CMakeLists.txt`:
   ```cmake
   set(PICOOS_APP_SOURCES
       ${CMAKE_CURRENT_SOURCE_DIR}/apps/app_table.c
       ${CMAKE_CURRENT_SOURCE_DIR}/apps/myapp.c
       ${CMAKE_CURRENT_SOURCE_DIR}/apps/newapp.c   # ← new
   )
   ```
5. **Rebuild** — run `./build pico` (or your target board).  The new app appears in the shell `help` listing and can be launched with `run newapp`.

---

## Keeping picoOS up to date

To pull the latest picoOS fixes into your project:

```bash
cd picoOS
git fetch origin
git checkout main
git pull
cd ..
git add picoOS
git commit -m "Update picoOS submodule to latest main"
```

To bring a collaborator's machine in sync after a submodule pointer update:

```bash
git pull
git submodule update --init
```

---

## The 10% case: making a kernel change

When a project needs a kernel change, always work on a named branch in the picoOS repo — never leave the submodule with uncommitted changes.

```bash
# 1. Create a feature branch inside the submodule
cd picoOS
git checkout -b feature/myproject-xyz
# edit kernel files, then:
git commit -am "Add interrupt priority override for myproject"
git push origin feature/myproject-xyz

# 2. Record the new submodule pointer in your project
cd ..
git add picoOS
git commit -m "Pin picoOS to kernel feature branch"
```

Update `.gitmodules` so collaborators know which branch to pull from:

```
[submodule "picoOS"]
    path   = picoOS
    branch = feature/myproject-xyz
    url    = https://github.com/JeffCurless/picoOS.git
```

**Keeping in sync as picoOS main advances:**

```bash
cd picoOS
git fetch origin
git rebase origin/main
git push --force-with-lease origin feature/myproject-xyz
cd ..
git add picoOS
git commit -m "Rebase picoOS feature branch onto updated main"
```

**When the feature is merged upstream:**

```bash
cd picoOS
git checkout main && git pull
cd ..
# restore branch field in .gitmodules to "main"
git add .gitmodules picoOS
git commit -m "Return picoOS submodule to main (feature upstreamed)"
```

---

## Final project structure

After completing all steps your project directory looks like this:

```
myproject/
├── .gitignore
├── .gitmodules
├── CMakeLists.txt
├── pico_sdk_import.cmake
├── build                   ← build script
├── clean                   ← clean script
├── picoOS/                 ← git submodule (do not edit directly)
│   └── src/ ...
├── apps/
│   ├── app_table.c         ← app registry
│   ├── myapp.h
│   └── myapp.c
└── kits/                   ← built .uf2 images (gitignored)
```
