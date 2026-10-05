#!/usr/bin/env python3
"""Number the sections of the picoOS book and (re)build its contents and index.

Usage:  python3 docs/book/build_toc_index.py [--report]

Edits learning-operating-systems-with-picoos.md in place and is safe to rerun
after the text changes: it strips its own section numbers, anchors, contents
and index first, then regenerates them.

Sections are numbered <chapter>.<n> (P.<n> for the preface, A.<n> for the
appendix). The contents and the index cite those numbers rather than page
numbers, so they stay correct in any print layout. --report prints where each
index term was found, to help tune INDEX_TERMS.
"""

import re
import sys
from pathlib import Path

BOOK = Path(__file__).with_name("learning-operating-systems-with-picoos.md")

TOC_START, TOC_END = "<!-- toc:start -->", "<!-- toc:end -->"
INDEX_START = "<!-- index:start -->"

# Index entries: (label, regex, fixed). The label is printed as written, so
# code names carry their own backticks. The regex (case-insensitive) is
# searched in each section and chapter opening; a fixed list of section ids
# replaces the search for words that appear almost everywhere, so the index
# cites only their main discussions. A bare chapter number ("15") cites the
# chapter's opening paragraphs, before its first numbered section.
INDEX_TERMS = [
    ("affinity", r"\baffinity\b", None),
    ("`app_table[]`", r"app_table", None),
    ("applications, writing", None, ["14", "14.1", "14.2", "14.3", "14.4", "14.5"]),
    ("`arch.h`", r"arch\.h", None),
    ("async context (SDK)", r"async.context", None),
    ("`AUTORUN=`", r"AUTORUN", None),
    ("back-pressure", r"back-pressure", None),
    ("banner, boot", r"\bbanner\b", None),
    ("BLOCKED state", r"\bBLOCKED\b", None),
    ("Bluetooth", r"\bBluetooth\b|\bBTstack\b", None),
    ("BOOTSEL", r"BOOTSEL", None),
    ("boot sequence", None, ["3", "3.1", "3.2", "3.4"]),
    ("boundary-tag allocator", r"boundary-tag", None),
    ("buddy allocator", r"\bbuddy\b", None),
    ("`build` script", r"\./build\b", None),
    ("`BUTTONA=` … `BUTTONY=`", r"BUTTON[ABXY]=", None),
    ("canary, stack", r"\bcanary\b|0xDEADBEEF", None),
    ("client–server design", r"client.server", None),
    ("coalescing", r"coalesc", None),
    ("`config.txt`", r"config\.txt", None),
    ("`console.py`", r"console\.py", None),
    ("context switch", None, ["3.3", "4.1", "5.5", "6.1"]),
    ("cooperative scheduling", r"\bcooperative\b", None),
    ("copy-on-write", r"copy-on-write", None),
    ("Cortex-M0+", r"Cortex-M0\+|\bM0\+", None),
    ("Cortex-M33", r"Cortex-M33|\bM33\b", None),
    ("critical section", r"critical section", None),
    ("cross-compiler", r"cross-compiler", None),
    ("`current_tcb[]`, `CURRENT_TCB`", r"current_tcb|CURRENT_TCB", None),
    ("CYW43 radio", r"CYW43", None),
    ("deadlock", r"deadlock", None),
    ("`dev_ioctl()`", r"dev_ioctl", None),
    ("`device_t`", r"device_t", None),
    ("Dijkstra, Edsger", r"Dijkstra", None),
    ("Display Pack", r"Display Pack", None),
    ("`dropped` counter", r"`dropped`", None),
    ("event flags", r"event.flags", None),
    ("EXC_RETURN", r"EXC.RETURN|exc_return", None),
    ("execute in place (XIP)", r"\bXIP\b|execute in place", None),
    ("fake interrupt frame", r"fake (interrupt )?frame|pretend saved state", None),
    ("file descriptor", r"file descriptor", None),
    ("first-fit", r"first-fit", None),
    ("flash memory", None, ["7.1", "7.2", "12", "12.1", "12.2", "12.4", "12.5"]),
    ("flashing the Pico", None, ["1.4"]),
    ("`flash_safe_execute()`", r"flash_safe_execute", None),
    ("fragmentation", r"fragment", None),
    ("`fs_close()`", r"fs_close", None),
    ("`fs_init()`", r"fs_init", None),
    ("`get_core_num()`", r"get_core_num", None),
    ("hardware spinlocks", r"spinlock registers?|hardware (spin)?locks?", None),
    ("heap", None, ["7.3", "7.5", "7.6", "7.7", "7.8"]),
    ("`heap_lock`", r"heap_lock", None),
    ("idle threads", None, ["3.2", "5.4", "6.1", "6.2"]),
    ("interrupts, disabling", r"disabl\w* interrupts|interrupts off|IRQs off", None),
    ("inter-processor interrupt", r"inter-processor", None),
    ("ioctl", r"\bioctl\b", None),
    ("journal (filesystem)", r"\bjournal\b", None),
    ("kernel process (PID 1)", r"kernel process", None),
    ("`kfree()`", r"\bkfree\b", None),
    ("`kill`, `killproc`", r"\bkill(proc)?\b", None),
    ("`kmalloc()`", r"\bkmalloc\b", None),
    ("`kmutex_t`, `kmutex_lock()`", r"kmutex", None),
    ("lock order", r"same order|lock order|writes its order", None),
    ("lockout victim", r"lockout victim", None),
    ("lost wake-up", r"lost wake-up", None),
    ("lwIP", r"\blwIP\b", None),
    ("Mars Pathfinder", r"Pathfinder", None),
    ("`mem` command", r"pico> mem|`mem`", None),
    ("memory map", None, ["7.1"]),
    ("memory management unit (MMU)", r"\bMMU\b", None),
    ("memory protection unit (MPU)", r"\bMPU\b", None),
    ("message queue (`mqueue_t`)", r"mqueue|message queue", None),
    ("multicore lockout", r"lockout", None),
    ("mutex", None, ["8.5", "8.8", "8.11", "11.3", "12.6", "15.4"]),
    ("mutual exclusion", r"mutual exclusion", None),
    ("PCB (process control block)", r"\bPCB\b|pcb_t|process control block", None),
    ("PendSV", r"PendSV", None),
    ("`pi` app", r"run pi\b|`pi`", None),
    ("Pico SDK", r"Pico SDK", None),
    ("picoOS API", r"picoOS API|picoOS_API", None),
    ("PID ranges", None, ["4.2", "13.4"]),
    ("power loss", r"Pull the power|power cut", None),
    ("preemption", r"preempt", None),
    ("`prim_pool` (stripe pool)", r"prim_pool|stripe", None),
    ("priorities", None, ["5", "5.2", "5.3", "8.10", "14.3"]),
    ("priority inheritance", r"priority inheritance", None),
    ("priority inversion", r"priority inversion", None),
    ("process", None, ["2.3", "2.5", "4", "4.2"]),
    ("producer and consumer demo", r"\bproducer\b", None),
    ("PSP and MSP", r"\bPSP\b|\bMSP\b", None),
    ("race condition", r"\brace\b", None),
    ("ready queues", r"ready.queues?", None),
    ("reaping", r"\breap", None),
    ("ring buffer", r"ring buffer", None),
    ("round-robin", r"round-robin", None),
    ("RP2040", None, ["1.1", "5.5", "7.1", "8.3", "8.8", "12.2"]),
    ("RP2350", None, ["1.1", "2.3", "5.5", "10.3", "12.2"]),
    ("`run` command", None, ["4.2", "6.3", "13.3", "14.4"]),
    ("`scanbuf.c`", r"scanbuf", None),
    ("scantest", r"scantest", None),
    ("`sched_lock`", r"sched_lock", None),
    ("`sched_next_thread()`", r"sched_next_thread", None),
    ("`sched_start()`", r"sched_start", None),
    ("`sched_yield()`", r"sched_yield", None),
    ("semaphore", r"semaphore", None),
    ("shell", None, ["13", "13.1", "13.2", "13.3", "13.4"]),
    ("`shell_print()`", r"shell_print", None),
    ("`shell_register_cmd()`", r"shell_register_cmd", None),
    ("SIO block", r"\bSIO\b|0xD000", None),
    ("slab allocator", r"\bslab\b", None),
    ("SLEEPING state", r"\bSLEEPING\b", None),
    ("SMP (symmetric multiprocessing)", r"\bSMP\b|symmetric", None),
    ("`spinlock_irq_acquire()`", r"spinlock_irq_acquire", None),
    ("`spinlock_t`", r"spinlock_t", None),
    ("SRAM", None, ["1.1", "7", "7.1", "7.3", "12.4"]),
    ("stack", None, ["3.3", "4.4", "7.4"]),
    ("stack overflow", r"STACK OVERFLOW|overflow", None),
    ("starvation", r"starv", None),
    ("superblock", r"superblock", None),
    ("`svc` instruction", r"\bsvc\b", None),
    ("`sys_sleep()`", r"sys_sleep", None),
    ("`syscall_dispatch()`", r"syscall_dispatch", None),
    ("system calls", None, ["10", "10.1", "10.2", "10.3", "10.4"]),
    ("SysTick", r"SysTick", None),
    ("TCB (thread control block)", None, ["4.1", "4.3", "5.5", "8.5"]),
    ("tests, host-native", r"\./build tests|host test|tests/", None),
    ("thread", None, ["2.5", "3.3", "4", "4.1"]),
    ("thread states", None, ["4.5", "5.3"]),
    ("`threads` command", r"pico> threads|`threads`", None),
    ("time slice", r"time slice|TIME_SLICE_MS|10 ms", None),
    ("torn read", r"torn read", None),
    ("`trace` command", r"`trace|trace on|trace show", None),
    ("triple buffer", r"triple buffer", None),
    ("`tud_task()`", r"tud_task", None),
    ("`dev_console_poll()`", r"dev_console_poll", None),
    ("UF2 file", r"\.uf2|\bUF2\b", None),
    ("`update` command", r"`update`", None),
    ("VFS (virtual file system)", None, ["2.2", "11", "11.3"]),
    ("`vfs_lock`", r"vfs_lock", None),
    ("wear levelling", r"wear", None),
    ("WiFi", None, ["6.2", "15", "15.1", "15.2", "15.3", "15.4"]),
    ("`wifi-poll` thread", r"wifi-poll", None),
    ("`wifi_scan_wait()`", r"wifi_scan_wait", None),
    ("ZOMBIE state", r"\bZOMBIE\b|zombie", None),
]

# The glossary repeats most terms in one line each; citing it everywhere
# would only add noise, so it is left out of the search.
NO_SEARCH = {"A.1"}


# Terms found in more sections than this are reported so they can be given a
# fixed list instead; the index itself still cites every hit.
NOISY = 9


def strip_generated(text):
    """Remove everything this script added, restoring the plain book."""
    text = re.sub(re.escape(TOC_START) + r".*?" + re.escape(TOC_END) + r"\n*",
                  "", text, flags=re.S)
    i = text.find(INDEX_START)
    if i != -1:
        text = text[:i].rstrip() + "\n"
    text = re.sub(r'^<a id="[^"]+"></a>\n', "", text, flags=re.M)
    text = re.sub(r"^(### )(?:[0-9]+|P|A)\.[0-9]+ ", r"\1", text, flags=re.M)
    return text


def chapter_key(title):
    if title.startswith("Preface"):
        return "P"
    if title.startswith("Appendix"):
        return "A"
    m = re.match(r"Chapter (\d+)", title)
    return m.group(1) if m else None


def number_sections(lines):
    """Number ### headings, add anchors, and return the section list.

    Each section is [id, title, chapter key, body lines]; a chapter's opening
    paragraphs are a section whose id is the bare chapter key. Chapters are
    (key, title). Fenced code is skipped so '#include' is never a heading.
    """
    out, sections, chapters = [], [], []
    in_code = False
    chap, chap_title, n = None, None, 0
    current = None
    for line in lines:
        if line.startswith("```"):
            in_code = not in_code
        if not in_code and line.startswith("## "):
            chap_title = line[3:].strip()
            chap, n = chapter_key(chap_title), 0
            chapters.append((chap, chap_title))
            out.append(f'<a id="ch-{chap.lower()}"></a>')
            out.append(line)
            current = [chap, chap_title, chap, []]   # the chapter's opening
            sections.append(current)
            continue
        if not in_code and line.startswith("### ") and chap:
            n += 1
            sid = f"{chap}.{n}"
            title = line[4:].strip()
            current = [sid, title, chap, []]
            sections.append(current)
            out.append(f'<a id="s-{sid.lower().replace(".", "-")}"></a>')
            out.append(f"### {sid} {title}")
            continue
        if current is not None:
            current[3].append(line)
        out.append(line)
    return out, sections, chapters


def anchor(sid):
    if "." not in sid:
        return "#ch-" + sid.lower()
    return "#s-" + sid.lower().replace(".", "-")


def build_toc(sections, chapters):
    rows = [TOC_START, "", "## Contents", ""]
    for key, title in chapters:
        rows.append(f"- [{title}](#ch-{key.lower()})")
        for sid, stitle, chap, _ in sections:
            if chap == key and "." in sid:
                rows.append(f"    - [{sid} {stitle}]({anchor(sid)})")
    rows += ["- [Index](#index)", "", TOC_END, ""]
    return rows


def sort_key(sid):
    chap, _, n = sid.partition(".")
    order = -1 if chap == "P" else (99 if chap == "A" else int(chap))
    return (order, int(n or 0))


def index_sort_key(term):
    return re.sub(r"[^a-z0-9 ]", "", term.lower())


def build_index(sections, report=False):
    known = {s[0] for s in sections}
    entries = []
    for term, pattern, fixed in INDEX_TERMS:
        if fixed:
            bad = [s for s in fixed if s not in known]
            if bad:
                sys.exit(f"index term {term!r}: unknown sections {bad}")
            hits = fixed
        else:
            rx = re.compile(pattern, re.I)
            hits = [sid for sid, title, _, body in sections
                    if sid not in NO_SEARCH
                    and (rx.search(title) or rx.search("\n".join(body)))]
            if report:
                flag = ("  <-- no hits" if not hits else
                        "  <-- noisy" if len(hits) > NOISY else "")
                print(f"{term:40s} {len(hits):2d} {' '.join(hits)}{flag}")
        if not hits:
            continue
        entries.append((term, sorted(set(hits), key=sort_key)))

    entries.sort(key=lambda e: index_sort_key(e[0]))
    rows = [INDEX_START, "", '<a id="index"></a>', "## Index", "",
            "Numbers are section numbers: 7.5 is Chapter 7, section 5. "
            "A bare number such as 15 is the opening of that chapter; "
            "P is the preface and A the appendix.", ""]
    letter = None
    for term, hits in entries:
        first = index_sort_key(term)[:1].upper()
        first = first if first.isalpha() else "#"
        if first != letter:
            letter = first
            rows += ["", f"**{letter}**", ""]
        refs = ", ".join(f"[{h}]({anchor(h)})" for h in hits)
        rows.append(f"- {term} — {refs}")
    return rows


def main():
    report = "--report" in sys.argv
    text = strip_generated(BOOK.read_text())
    lines = text.rstrip("\n").split("\n")
    body, sections, chapters = number_sections(lines)

    # Contents go after the title and byline, before the first chapter.
    head_end = next(i for i, l in enumerate(body) if l.startswith("<a id=\"ch-"))
    toc = build_toc(sections, chapters)
    index = build_index(sections, report)
    if report:
        return
    out = body[:head_end] + toc + body[head_end:] + ["", ""] + index
    BOOK.write_text("\n".join(out) + "\n")
    print(f"{len(chapters)} chapters, "
          f"{sum(1 for s in sections if '.' in s[0])} sections, "
          f"{sum(1 for l in index if l.startswith('- '))} index entries")


if __name__ == "__main__":
    main()
