#!/usr/bin/env python3
"""
mem_report.py — picoOS flash and SRAM usage report from linker map file.

Usage:
    python3 tools/mem_report.py                             # default map path
    python3 tools/mem_report.py build/src/picoos.elf.map   # positional path
    python3 tools/mem_report.py --map path/to/foo.map      # named option
    python3 tools/mem_report.py --brief                     # one-line summary only
"""

import argparse
import re
import sys

DEFAULT_MAP = "build/src/picoos.elf.map"

# SRAM totals (main RAM + SCRATCH_X + SCRATCH_Y) by chip.
# Used both to identify the chip from a parsed total and as the fallback
# when the Memory Configuration section cannot be found in the map.
_CHIP_SRAM = {
    "RP2040": 264 * 1024,   # 256 KB + 4 KB + 4 KB
    "RP2350": 520 * 1024,   # 512 KB + 4 KB + 4 KB
}

FLASH_ORIGIN = 0x10000000   # XIP base address, same on both chips


def chip_from_sram(total_sram):
    """Return the chip name whose canonical SRAM total best matches total_sram.

    Accepts a ±10 % tolerance to absorb minor linker-script variations.
    Returns 'Unknown' if no chip matches.
    """
    for name, canonical in _CHIP_SRAM.items():
        if abs(total_sram - canonical) <= canonical // 10:
            return name
    return "Unknown"


def chip_from_path(path):
    """Guess the chip name from the map file path (last resort fallback).

    Relies on picoOS's output naming convention:
      pico2wos-*.elf.map  → RP2350
      picoos-*.elf.map    → RP2040
    """
    name = path.lower()
    if "pico2" in name:
        return "RP2350"
    return "RP2040"


# Subsystem attribution: (substring_to_match, display_label)
# Matched in order; first match wins.
SUBSYSTEMS = [
    ("stack_pool",  "Thread stacks"),
    ("heap_memory", "Kernel heap"),
    ("framebuffer", "Display framebuffer"),
    ("fs_buffer",    "FS RAM buffer"),
    ("fs_ram",       "FS RAM buffer"),
    ("superblock_ram", "FS RAM buffer"),
    ("tcb_pool",    "TCB pool"),
    ("pcb_pool",    "PCB pool"),
    ("fd_table",    "VFS fd table"),
    ("dev_mounts",  "VFS mount table"),
]

# Flash section breakdown: (section_name, display_label)
# Matched against the exact top-level output section name.
FLASH_SECTIONS = [
    ("boot2",       ".boot2 (2nd-stage bootloader)"),
    ("text",        ".text  (code)"),
    ("rodata",      ".rodata (constants)"),
    ("binary_info", ".binary_info"),
    ("data",        ".data  (init'd globals, load image)"),
]


def label_for(symbol_name):
    for fragment, label in SUBSYSTEMS:
        if fragment in symbol_name:
            return label
    return "SDK / other"


def parse_map(path):
    try:
        with open(path) as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"error: map file not found: {path}", file=sys.stderr)
        sys.exit(1)
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    # --- Memory Configuration: SRAM and flash totals ---
    total_sram  = 0
    flash_total = 0
    in_mem_config = False
    for line in lines:
        stripped = line.strip()
        if stripped == "Memory Configuration":
            in_mem_config = True
            continue
        if in_mem_config:
            if stripped == "Linker script and memory map":
                break
            m = re.match(r'^(FLASH|RAM|SCRATCH_\w+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)',
                         stripped)
            if m:
                size = int(m.group(3), 16)
                if m.group(1) == "FLASH":
                    flash_total = size
                else:
                    total_sram += size

    if total_sram == 0:
        chip = chip_from_path(path)
        total_sram = _CHIP_SRAM[chip]
        print(
            f"warning: Memory Configuration section not found in {path};\n"
            f"         guessing {chip} ({total_sram // 1024} KB) from filename.",
            file=sys.stderr,
        )

    # --- Flash used: __flash_binary_end symbol ---
    # Appears as either:
    #   "  0x1000f188   __flash_binary_end = ."
    #   "  0x10057320   PROVIDE (__flash_binary_end = .)"
    flash_binary_end = 0
    flash_end_re = re.compile(
        r'^\s+(0x1[0-9a-fA-F]+)\s+(?:PROVIDE\s*\(\s*)?__flash_binary_end'
    )
    for line in lines:
        m = flash_end_re.match(line)
        if m:
            flash_binary_end = int(m.group(1), 16)
            break

    flash_used = flash_binary_end - FLASH_ORIGIN if flash_binary_end else 0

    # --- Flash section breakdown ---
    # Top-level flash sections: ".boot2  0x10000000  0x100"
    # .data also has a load address in flash: ".data  0x20000xxx  0xSIZE  load address 0x10xxxxxx"
    flash_section_sizes = {}   # section_name -> bytes
    top_flash_re = re.compile(
        r'^\.(\w+)\s+(0x1[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)'
    )
    data_load_re = re.compile(
        r'^\.data\s+0x[0-9a-fA-F]+\s+(0x[0-9a-fA-F]+)\s+load address\s+(0x1[0-9a-fA-F]+)'
    )
    for line in lines:
        m = data_load_re.match(line)
        if m:
            flash_section_sizes["data"] = int(m.group(1), 16)
            continue
        m = top_flash_re.match(line)
        if m:
            name = m.group(1)
            size = int(m.group(3), 16)
            if name != "data" and size > 0:
                flash_section_sizes[name] = size

    # --- Section totals for .data and .bss (SRAM) ---
    data_total = 0
    bss_total  = 0
    section_re = re.compile(r'^(\.(data|bss))\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)')
    for line in lines:
        m = section_re.match(line)
        if m:
            size = int(m.group(4), 16)
            if m.group(2) == "data":
                data_total = size
            else:
                bss_total = size

    # --- Named BSS symbol sizes ---
    # Two formats in the map:
    #   one-line:  " .bss.NAME  0xADDR  0xSIZE  file.o"
    #   two-line:  " .bss.NAME\n             0xADDR  0xSIZE  file.o"
    symbol_sizes = {}  # label -> bytes

    one_line_re = re.compile(
        r'^\s+\.(\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+\S'
    )
    pending_symbol = None
    addr_only_re = re.compile(r'^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+\S')

    for line in lines:
        if pending_symbol is not None:
            m = addr_only_re.match(line)
            if m:
                size = int(m.group(2), 16)
                lbl = label_for(pending_symbol)
                if lbl != "SDK / other":
                    symbol_sizes[lbl] = symbol_sizes.get(lbl, 0) + size
            pending_symbol = None
            continue

        m = one_line_re.match(line)
        if m:
            sym_name = m.group(1)
            size = int(m.group(3), 16)
            if "." in sym_name:
                lbl = label_for(sym_name)
                if lbl != "SDK / other":
                    symbol_sizes[lbl] = symbol_sizes.get(lbl, 0) + size
            continue

        m2 = re.match(r'^\s+\.(\S+)\s*$', line)
        if m2 and "." in m2.group(1):
            pending_symbol = m2.group(1)

    return (total_sram, data_total, bss_total, symbol_sizes,
            flash_total, flash_used, flash_section_sizes)


def fmt_row(label, size, total):
    kb  = size / 1024
    pct = 100.0 * size / total if total else 0.0
    return f"  {label:<32} {size:>8}   {kb:>7.1f}   {pct:>6.1f} %"


def report(path, brief):
    (total_sram, data_total, bss_total, symbol_sizes,
     flash_total, flash_used, flash_section_sizes) = parse_map(path)

    chip = chip_from_sram(total_sram)

    sram_used = data_total + bss_total
    sram_free = total_sram - sram_used
    flash_free = flash_total - flash_used if flash_total else 0

    if brief:
        sram_pct  = 100.0 * sram_used  / total_sram  if total_sram  else 0.0
        flash_pct = 100.0 * flash_used / flash_total if flash_total else 0.0
        print(
            f"[{chip}]  "
            f"Flash: {flash_used/1024:.1f} / {flash_total/1024:.1f} KB used ({flash_pct:.1f} %)  |  "
            f"SRAM: {sram_used/1024:.1f} / {total_sram/1024:.1f} KB used ({sram_pct:.1f} %)"
        )
        return

    sep = "-" * 60
    col_hdr = f"  {'Region':<32} {'Bytes':>8}   {'KB':>7}   {'% of total':>10}"

    print(f"\npicoOS memory report — {path}")
    print(f"Chip              : {chip}")

    # ---- Flash ----
    print(f"\nFlash total       : {flash_total} bytes ({flash_total // 1024} KB)")
    print(sep)
    print(col_hdr)
    print(sep)

    # Named sections in FLASH_SECTIONS order; skip any with zero size
    accounted = 0
    for sec_name, label in FLASH_SECTIONS:
        size = flash_section_sizes.get(sec_name, 0)
        if size:
            print(fmt_row(label, size, flash_total))
            accounted += size

    # Anything in flash not covered by the named sections above
    sdk_flash = max(0, flash_used - accounted)
    if sdk_flash:
        print(fmt_row("SDK / other (firmware, WiFi, ...)", sdk_flash, flash_total))

    print(sep)
    print(fmt_row("TOTAL USED", flash_used, flash_total))
    print(fmt_row("FREE", flash_free, flash_total))

    # ---- SRAM ----
    print(f"\nSRAM total        : {total_sram} bytes ({total_sram // 1024} KB)")
    print(sep)
    print(col_hdr)
    print(sep)

    seen = set()
    ordered_labels = []
    for _, label in SUBSYSTEMS:
        if label not in seen:
            seen.add(label)
            ordered_labels.append(label)

    for label in ordered_labels:
        size = symbol_sizes.get(label, 0)
        if size:
            print(fmt_row(label, size, total_sram))

    print(fmt_row(".data (init'd globals)", data_total, total_sram))

    attributed_bss = sum(symbol_sizes.values())
    sdk_other = max(0, bss_total - attributed_bss)
    if sdk_other:
        print(fmt_row("SDK / other", sdk_other, total_sram))

    print(sep)
    print(fmt_row("TOTAL USED", sram_used, total_sram))
    print(fmt_row("FREE", sram_free, total_sram))
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Report picoOS flash and SRAM usage from a linker map file."
    )
    parser.add_argument(
        "mapfile", nargs="?", metavar="MAP",
        help="path to the .elf.map file (positional)"
    )
    parser.add_argument(
        "--map", default=None, metavar="PATH",
        help=f"path to the .elf.map file (default: {DEFAULT_MAP})"
    )
    parser.add_argument(
        "--brief", action="store_true",
        help="print a single summary line instead of the full table"
    )
    args = parser.parse_args()
    map_path = args.mapfile or args.map or DEFAULT_MAP
    report(map_path, args.brief)


if __name__ == "__main__":
    main()
