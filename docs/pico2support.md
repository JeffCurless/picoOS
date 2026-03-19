# Pico 2 (RP2350) Support

This document describes the code changes required to make picoOS build and run
on the Raspberry Pi Pico 2 (RP2350 chip) while retaining full support for the
original Pico (RP2040).

**Scope:** ARM Cortex-M33 variant of RP2350 only. The RISC-V variant would
require a complete replacement of `sched_asm.S` and is not covered here.

**SDK requirement:** Pico SDK 2.0.0 or later.

---

## Build usage after these changes

```bash
# Pico 1 (RP2040) — unchanged default
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk"
make -j$(nproc) -C build

# Pico 2 (RP2350)
cmake -B build -DPICO_SDK_PATH="$HOME/pico-sdk" -DPICO_BOARD=pico2
make -j$(nproc) -C build
```

---

## What already works on RP2350 without changes

- Flash layout — XIP base `0x10000000`, 4 KB sector size, and the
  `flash_range_erase` / `flash_range_program` SDK calls are identical on both
  chips.
- Multicore lockout — RP2350 SDK provides a compatible
  `multicore_lockout_start/end_blocking` API.
- All GPIO/SPI/PWM drivers — the Pimoroni Display Pack pin assignments are
  board-hardware, not chip-specific.
- Context-switch frame format — Cortex-M33 supports M0+ Thumb instructions as a
  strict subset; the staging-register workaround in `sched_asm.S` is
  inefficient on M33 but functionally correct.
- EXC_RETURN value `0xFFFFFFFD` — valid on both M0+ and M33 (Thread mode, PSP,
  no FP context).

---

## Required changes

### 1. `src/kernel/arch.h` — Remove the hardcoded `RP2040.h` include

**Problem:** Line 34 reads:
```c
#include "RP2040.h"
```
When the Pico SDK targets RP2350 it does not put `RP2040.h` on the include
path, so the build fails. The include is also redundant: `pico/stdlib.h`
(line 33) already transitively pulls in the correct CMSIS device header for
whichever chip is targeted.

**Fix:** Delete line 34. All CMSIS definitions (`SysTick_Config`, `SCB`,
`NVIC`, `__set_PSP`, etc.) continue to arrive via `pico/stdlib.h`.

---

### 2. `src/kernel/sched.c` — Dynamic SysTick frequency

**Problem:** Line 375:
```c
SysTick_Config(125000u);   /* uses CPU clock, enables SysTick IRQ */
```
This hardcodes 125 MHz (RP2040 default). The RP2350 defaults to 150 MHz, so
the 1 ms tick period would be wrong (it would tick every 0.83 ms instead).

**Fix:** Read the actual system clock at runtime using the Pico SDK's
`clock_get_hz(clk_sys)` and compute the correct reload value. Add
`#include "hardware/clocks.h"` to the ARM block of `arch.h` (or directly at
the top of `sched.c`).

```c
// Replace:
SysTick_Config(125000u);

// With:
SysTick_Config(clock_get_hz(clk_sys) / 1000u);   /* 1 ms tick, any clock speed */
```

---

### 3. `src/shell/shell.c` — Dynamic platform info strings

**Problem:** Lines 382–384 in `cmd_info()` hardcode chip-specific strings:
```c
shell_print("Platform  : RP2040, dual ARM Cortex-M0+ (133 MHz max)\r\n");
shell_print("SRAM      : 264 KB\r\n");
shell_print("Flash     : 2 MB QSPI (XIP)\r\n");
```

**Fix:** Replace with macro references injected by CMake (see change 4):
```c
shell_print("Platform  : " PICOOS_PLATFORM_STR "\r\n");
shell_print("SRAM      : " PICOOS_SRAM_STR "\r\n");
shell_print("Flash     : " PICOOS_FLASH_STR "\r\n");
```

---

### 4. `src/CMakeLists.txt` — Detect target chip, set string defines, link `hardware_clocks`

After `pico_sdk_init()` runs the SDK exposes `PICO_RP2040` and `PICO_RP2350`
as CMake booleans. Use them to set per-chip string defines and to add the
`hardware_clocks` library (needed by `clock_get_hz`).

**Add after the feature-flag option block (before `add_executable`):**
```cmake
if(PICO_RP2350)
    set(PICOOS_PLATFORM_STR "RP2350, dual ARM Cortex-M33 (150 MHz max)")
    set(PICOOS_SRAM_STR     "520 KB")
    set(PICOOS_FLASH_STR    "4 MB QSPI (XIP)")
else()
    set(PICOOS_PLATFORM_STR "RP2040, dual ARM Cortex-M0+ (133 MHz max)")
    set(PICOOS_SRAM_STR     "264 KB")
    set(PICOOS_FLASH_STR    "2 MB QSPI (XIP)")
endif()
```

**In `target_compile_definitions(picoos PRIVATE ...)`, add:**
```cmake
PICOOS_PLATFORM_STR="${PICOOS_PLATFORM_STR}"
PICOOS_SRAM_STR="${PICOOS_SRAM_STR}"
PICOOS_FLASH_STR="${PICOOS_FLASH_STR}"
```

**In `PICOOS_LIBS`, add:**
```cmake
hardware_clocks
```

---

## Comment-only updates (no logic change)

These files have text that refers to RP2040 specifics and should be updated for
accuracy, but none of the changes affect behavior.

| File | Lines | What to update |
|------|-------|----------------|
| `src/kernel/sched_asm.S` | 2–4, 14, 25–26, 104, 174 | Note M33 compatibility alongside M0+ references |
| `src/kernel/task.h` | 14–20 | Update SRAM budget comment (264 KB → note RP2350 has 520 KB) |
| `src/kernel/mem.h` | 10, 16–19 | Update "264 KB" reference |
| `CLAUDE.md` | Target line, SRAM budget table | Update for dual-chip support |
| `README.md` | Hardware table, memory budget section | Add RP2350/Pico 2 row |

---

## Optional enhancement: simplified M33 context switch

The Cortex-M33's Thumb-2 instruction set supports `push {r4-r11}` and
`pop {r4-r11}` directly, eliminating the six staging-register `MOV`
instructions currently used to work around the M0+ limitation in
`sched_asm.S`. This is not required for correctness but is a good student
exercise. A conditional implementation would look like:

```asm
#if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_7M__)
    /* Cortex-M33 / M4 / M7 — direct push/pop of high registers */
    push    {r4-r11}
    mov     r0, sp
    ...
    pop     {r4-r11}
#else
    /* Cortex-M0+ — staging-register workaround (existing code) */
    ...
#endif
```

---

## Verification checklist

1. **RP2040 build unchanged:** `cmake -B build && make -j$(nproc) -C build`
   succeeds; `info` command prints `RP2040 / 264 KB / 2 MB`.
2. **RP2350 build:** `cmake -B build -DPICO_BOARD=pico2 && make -j$(nproc) -C build`
   succeeds with no errors or warnings.
3. **Platform string on RP2350:** flash firmware to Pico 2; `info` prints
   `RP2350, dual ARM Cortex-M33 / 520 KB / 4 MB`.
4. **1 ms tick on RP2350:** `threads` CPU-time counters advance at the same
   real-time rate as on RP2040.
5. **`mem_report.py` on RP2350 build:** `python3 tools/mem_report.py` shows
   total SRAM of 532 480 bytes (520 KB = RAM 512 KB + SCRATCH_X 4 KB +
   SCRATCH_Y 4 KB, as reported by the RP2350 linker map).
