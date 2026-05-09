# Filesystem Flash Region — Sizing Analysis

> **Status: Implemented.**  The per-board filesystem sizing described below is in place
> as of v0.2.0.  This document records the analysis that drove the design decisions.

## Background

The picoOS filesystem flash region started hardcoded in `src/kernel/fs.h` at a 1 MB
offset with a 512 KB size — values chosen conservatively for the RP2040.  Since picoOS
runs on both RP2040 (2 MB flash) and RP2350 (4 MB flash), a single fixed configuration
wasted flash and did not scale with the board.

## Flash Layout Analysis

Flash usage was measured from linker map files using `tools/mem_report.py --brief`:

| Board | Total flash | Binary end | Firmware gap to 1 MB | FS start | FS size (current) | Flash after FS | Flash wasted |
|-------|------------|-----------|---------------------|----------|-------------------|----------------|-------------|
| pico (RP2040) | 2 MB | ~62 KB | ~962 KB | 1 MB | 512 KB | 512 KB | 512 KB |
| picow (RP2040+WiFi) | 2 MB | ~150 KB | ~874 KB | 1 MB | 512 KB | 512 KB | 512 KB |
| pico2w (RP2350+WiFi) | 4 MB | ~357 KB | ~667 KB | 1 MB | 512 KB | 2.5 MB | 3 MB |

The 1 MB firmware reservation is safe for all current builds (largest binary is 357 KB
on pico2w).  The waste is after the FS region — flash that is fully accessible but
never claimed.

## Constraints

### Superblock size limit

The superblock is the first 4 KB sector of the FS region.  Its on-flash layout is:

```
magic      (4 B)
version    (4 B)
file_count (4 B)
files[]    (N × sizeof(fs_entry_t))
```

`sizeof(fs_entry_t)` is 32 bytes (1 bool padded to 4 B + 16 B name + 4 B size +
4 B start_block + 4 B block_count = 32 B).  The header is 12 bytes, so:

```
12 + N × 32 ≤ 4096   →   N ≤ 127
```

**Maximum: 127 files** before the superblock overflows its one-sector allocation.

### RAM cost

The superblock is RAM-cached as `fs_superblock_t`.  It scales with `FS_MAX_FILES`:

| FS_MAX_FILES | Superblock RAM |
|-------------|---------------|
| 32 (current) | 1,036 B (~1 KB) |
| 64 | 2,060 B (~2 KB) |
| 127 | 4,076 B (~4 KB) |

Both chips have ample headroom (RP2040 had ~147 KB free SRAM in the last `mem` report).

### FS_FLASH_OFFSET stays at 1 MB

Changing the offset would invalidate any existing filesystem on a flashed device.
All supported boards have at least 1 MB of firmware headroom, so 1 MB is kept as a
fixed constant — only `FS_FLASH_SIZE` and `FS_MAX_FILES` vary by chip.

## Implemented Limits

| Chip | FS_FLASH_SIZE | FS_MAX_FILES | Sectors used | Flash used | Superblock RAM |
|------|--------------|-------------|-------------|-----------|---------------|
| RP2040 | 1 MB | 64 | 65 × 4 KB = 260 KB | 260 KB / 1 MB | ~2 KB |
| RP2350 | 3 MB | 127 | 128 × 4 KB = 512 KB | 512 KB / 3 MB | ~4 KB |

These values are intentionally conservative — they do not pack the FS region to the
flash limit, leaving room for future growth while still doubling (RP2040) or sextupling
(RP2350) the file count relative to the current limit of 32.

## Implementation (completed)

### 1. `src/kernel/fs.h` — guarded board-variable constants

Wrap the two size constants in `#ifndef` guards so CMake can override them per board
without touching the header:

```c
/* Before */
#define FS_MAX_FILES      32u
#define FS_FLASH_SIZE     (512u  * 1024u)

/* After */
#ifndef FS_MAX_FILES
#define FS_MAX_FILES      32u        /* override via CMake for board-specific sizing */
#endif

#ifndef FS_FLASH_SIZE
#define FS_FLASH_SIZE     (512u * 1024u)   /* override via CMake for board-specific sizing */
#endif
```

`FS_FLASH_OFFSET` remains a plain `#define` — it is the same for all supported boards.

### 2. `src/CMakeLists.txt` — board-specific definitions (in place)

In the existing `if(PICO_RP2350)` / `else()` block, add the FS variables:

```cmake
if(PICO_RP2350)
    # existing platform strings...
    set(PICOOS_FS_FLASH_SIZE   "(3u * 1024u * 1024u)")   # 3 MB (4 MB total − 1 MB firmware)
    set(PICOOS_FS_MAX_FILES    127u)
else()
    # existing platform strings...
    set(PICOOS_FS_FLASH_SIZE   "(1u * 1024u * 1024u)")   # 1 MB (2 MB total − 1 MB firmware)
    set(PICOOS_FS_MAX_FILES    64u)
endif()
```

Then pass them to the compiler in the `target_compile_definitions` block:

```cmake
target_compile_definitions(picoos PRIVATE
    ...existing defines...
    FS_FLASH_SIZE=${PICOOS_FS_FLASH_SIZE}
    FS_MAX_FILES=${PICOOS_FS_MAX_FILES}
)
```

### 3. `CLAUDE.md` — updated fs module description (done)

```
**fs** — flash-native filesystem (currently RAM-backed in current phase);
FS_MAX_FILES: 64 (RP2040) / 127 (RP2350) — set per-board in CMakeLists.txt,
max 127 (superblock must fit in one 4 KB sector: 12 + N×32 ≤ 4096);
FS_BLOCK_SIZE=4096; flash region at 1 MB offset;
FS_FLASH_SIZE: 1 MB (RP2040) / 3 MB (RP2350)
```

No changes are needed in `fs.c` — all constants are consumed via the header macros.

## Verification

1. **Both boards compile without warnings:**
   ```bash
   cmake -B build_pico  -DPICO_BOARD=pico   -DPICO_SDK_PATH="$HOME/pico-sdk"
   cmake -B build_pico2w -DPICO_BOARD=pico2_w -DPICO_SDK_PATH="$HOME/pico-sdk"
   make -j$(nproc) -C build_pico
   make -j$(nproc) -C build_pico2w
   ```

2. **`mem` shell command** — run on hardware; the superblock canary row should reflect
   the new `FS_MAX_FILES` (64 or 127 entries instead of 32).

3. **`mem_report.py`** — flash report should show the new `FS_FLASH_SIZE` bytes
   allocated (1 MB or 3 MB) vs the binary footprint.

4. **Filesystem smoke test on hardware:**
   - `fs format` — erases and re-creates the superblock with the new file count limit
   - Write 33+ files (beyond the old 32-file limit) — should succeed up to `FS_MAX_FILES`
   - `ls` — all files visible; `cat` — content correct
   - `reboot` — filesystem persists across reboot (flash was erased/programmed)

5. **RP2040 SRAM check** — `mem` command should show superblock RAM increased from
   ~1 KB to ~2 KB (64 files × 32 B + 12 B header = 2,060 B); heap headroom should
   still be healthy.
