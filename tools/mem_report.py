#!/usr/bin/env python3
"""
mem_report.py — picoOS SRAM usage report from linker map file.

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

    # --- Total SRAM from Memory Configuration ---
    total_sram = 0
    in_mem_config = False
    for line in lines:
        stripped = line.strip()
        if stripped == "Memory Configuration":
            in_mem_config = True
            continue
        if in_mem_config:
            if stripped == "":
                break
            # RAM  0x20000000  0x00040000  xrw
            # SCRATCH_X / SCRATCH_Y also count
            m = re.match(r'^(RAM|SCRATCH_\w+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)', stripped)
            if m:
                total_sram += int(m.group(3), 16)

    if total_sram == 0:
        # Fallback: RP2040 canonical value
        total_sram = 264 * 1024

    # --- Section totals for .data and .bss ---
    data_total = 0
    bss_total = 0
    # Lines like: ".data           0x200000c0     0x1448 load address ..."
    # or:         ".bss            0x20001508    0x23da8 load address ..."
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

    # Matches ".bss.symbol_name  0xADDR  0xSIZE  file.o" on one line.
    # Captures the full section name (e.g. "bss.stack_pool") after the leading dot.
    one_line_re = re.compile(
        r'^\s+\.(\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+\S'
    )
    pending_symbol = None  # name of a symbol whose address/size is on the next line
    addr_only_re = re.compile(r'^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+\S')

    for line in lines:
        # Check if previous line left a pending symbol name
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
            # Only attribute named subsections (e.g. "bss.fd_table", not bare "bss").
            # Skip "SDK / other" here — the residual is computed at report time.
            if "." in sym_name:
                lbl = label_for(sym_name)
                if lbl != "SDK / other":
                    symbol_sizes[lbl] = symbol_sizes.get(lbl, 0) + size
            continue

        # Detect two-line symbol: line is just "  .bss.NAME" with no address
        m2 = re.match(r'^\s+\.(\S+)\s*$', line)
        if m2 and "." in m2.group(1):
            pending_symbol = m2.group(1)

    return total_sram, data_total, bss_total, symbol_sizes


def fmt_row(label, size, total):
    kb = size / 1024
    pct = 100.0 * size / total
    return f"  {label:<28} {size:>8}   {kb:>6.1f}   {pct:>6.1f} %"


def report(path, brief):
    total_sram, data_total, bss_total, symbol_sizes = parse_map(path)

    total_used = data_total + bss_total
    total_free = total_sram - total_used

    if brief:
        pct = 100.0 * total_used / total_sram
        print(
            f"SRAM used: {total_used} B / {total_sram} B "
            f"({total_used/1024:.1f} KB / {total_sram/1024:.1f} KB, {pct:.1f} %)"
        )
        return

    sep = "-" * 56
    print(f"\npicoOS SRAM report — {path}")
    print(f"Total RP2040 SRAM : {total_sram} bytes ({total_sram // 1024} KB)")
    print(sep)
    print(f"  {'Region':<28} {'Bytes':>8}   {'KB':>6}   {'% of total':>10}")
    print(sep)

    # Named subsystems in SUBSYSTEMS order, deduplicating labels
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

    # .data
    print(fmt_row(".data (init'd globals)", data_total, total_sram))

    # SDK / other BSS = everything in .bss not claimed by a named subsystem
    attributed_bss = sum(symbol_sizes.values())
    sdk_other = max(0, bss_total - attributed_bss)
    if sdk_other:
        print(fmt_row("SDK / other", sdk_other, total_sram))

    print(sep)
    print(fmt_row("TOTAL USED", total_used, total_sram))
    print(fmt_row("FREE", total_free, total_sram))
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Report picoOS SRAM usage from a linker map file."
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
