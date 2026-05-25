# picoOS: Building an Operating System from Scratch
## Complete Book Outline

---

### README.md — About This Book

Introduces the book, its target audience (advanced high school students, early college students, and teachers), and the recommended reading paths. Explains that the book is structured as a reverse-engineering investigation: each chapter begins with a question an engineer would ask, then answers it by reading actual code. Provides two reading paths — sequential (start at Chapter 1) and reference (jump to the chapter covering the topic you need).

---

### investigation-notes.md — Codebase Inspection Notes

A technical record of the codebase inspection that informed this book. Lists every file examined, key functions and structs discovered, architecture notes, build process observations, documentation gaps, and questions for future maintainers. Intended as a companion document for readers who want to trace every claim in the book back to a specific file and line number.

---

### 00 — Title Page

Title, subtitle, project version, and framing statement. Sets the tone: this is a book about reading, understanding, and eventually improving a real operating system on real hardware.

---

### 01 — Preface

Explains why picoOS exists and why a book about it exists. The OS is intentionally imperfect — not because its authors were careless, but because imperfection is where learning happens. Introduces the teaching philosophy of deliberate suboptimality: a first-fit heap allocator fragments visibly; O(n) queue scans are slow enough to measure; synchronous I/O blocks in predictable ways. Describes what picoOS builds on (Raspberry Pi Pico SDK, BTstack, lwIP, TinyUSB) and what it intentionally omits (memory protection, privilege separation, network applications). Acknowledges the hardware environment and what makes bare-metal OS development different from application programming.

---

### 02 — How to Read This Book

A practical guide to getting the most from this text. Describes the two reading strategies: the sequential learner who builds understanding from the ground up, and the reference reader who dips into specific chapters as needed. Explains the recurring section formats: "Why This Matters," "Common Student Misunderstandings," and "Try This." Explains how to connect the book to the actual hardware — every chapter can be accompanied by running the code and observing the behavior with a USB serial terminal. Recommends keeping a terminal connected to the Pico while reading.

---

### 03 — Project Overview

Answers "What is picoOS and what does it do?" Describes the OS as a dual-core preemptive multitasking system for the Raspberry Pi Pico family. Lists the four board variants (pico, pico2, picow, pico2w) and explains what changes between them (processor architecture, SRAM, flash, optional WiFi and Bluetooth). Lists the major capabilities: USB CDC shell, concurrent threads with priority scheduling, a persistent flash filesystem, synchronization primitives, a device abstraction layer, and optional WiFi scanning and Bluetooth scanning. Explicitly lists what is NOT implemented: memory protection, privilege separation, SVC-based syscalls, subdirectories, multi-sector files, networked applications. Describes the version numbering and the development phases (phases 1–4 complete, 5–6 planned).

---

### 04 — Development Environment

Practical guide to setting up a working development environment. Covers prerequisites (Pico SDK, CMake, arm-none-eabi-gcc toolchain, Python 3, pyserial). Walks through the build steps for each board variant with exact CMake commands. Explains the LSP setup using a `compile_commands.json` symlink for editor code navigation. Covers flashing: BOOTSEL drag-and-drop for initial setup, and the `update` shell command for in-field firmware updates. Explains how to use `tools/console.py` for interactive testing — auto-detection of connected Picos, file upload with `--upload`, and log capture with `--log`. Introduces `tools/mem_report.py` for analyzing SRAM and flash usage from the ELF map file. Includes common setup errors and their fixes.

---

### 05 — First Principles: What Is an Operating System?

Answers "What problem does an operating system solve?" Starting from a bare microcontroller: with no OS, your program runs alone and never yields. Introduces the core OS problems — time sharing (multiple tasks on one CPU), resource management (who owns the UART?), and isolation (keeping one buggy program from crashing the whole system). Explains the ARM Cortex-M0+ and M33 execution model: registers, the stack pointer (MSP vs. PSP), thread mode vs. handler mode, and the exception model. Explains SysTick and PendSV as the two hardware mechanisms that make preemptive multitasking possible. Explains what picoOS implements (preemption, scheduling, IPC, filesystem, device abstraction) and what it deliberately skips (MMU, privilege levels, memory protection). This chapter gives readers the conceptual vocabulary for everything that follows.

---

### 06 — System Startup and the Boot Process

Answers "How does this system start?" Traces the full boot sequence from ARM ROM through the Pico SDK bootloader to `main()` in `src/main.c`. Walks through each phase of initialization in order: USB CDC setup and the 3-second enumeration wait, Core 1 launch and lockout victim registration, the five kernel subsystem inits (kmem → task → dev → vfs → fs), kernel process creation, optional WiFi/BT initialization, idle thread creation, shell process creation, and finally `sched_start()` which never returns. Explains why each step must happen before the next: you cannot allocate stacks before the heap is initialized; you cannot create threads before the task pool is initialized; you cannot write to flash before Core 1 is a lockout victim. Addresses the common student question: "where does main() go when sched_start() is called?"

---

### 07 — Project Structure

A guided tour of the repository layout. Explains the purpose of each directory and file. Covers `src/kernel/` (the OS core), `src/shell/` (the interactive interface), `src/drivers/` (hardware-specific display and LED code), `src/apps/` (user application framework), `tools/` (host companion scripts), `docs/` (design documentation), and `kits/` (pre-built firmware). Explains why `btstack_config.h` must live in `src/` rather than `src/kernel/`. Explains the dual-role of `src/kernel/arch.h`: on real hardware it includes the Pico SDK and CMSIS headers; on a development host without the SDK, it provides stub definitions so code editors and LSP tools can analyze the code without errors. Maps CMake feature flags to compiled source files.

---

### 08 — Core Architecture

Answers "How is the system organized at the highest level?" Explains the dual-core asymmetric design: Core 0 runs the USB console, SysTick, scheduler coordination, filesystem writes, and the shell; Core 1 is available for user threads and compute-heavy work. Explains the exception-driven scheduling loop: SysTick fires every 1 ms, decrements the time slice counter, and triggers PendSV when the slice expires; PendSV (at lowest priority) fires after all other ISRs complete and performs the context switch. Describes the process model: a Process Control Block (PCB) is a container for threads, open file descriptors, and a heap region — but provides no hardware memory isolation. Explains the VFS layer as the unifying I/O interface that routes `/dev/*` paths to device drivers and all other paths to the flash filesystem. Identifies the key architectural constraints: fixed TCB field layout, no MPU, single global kernel lock via interrupt disable.

---

### 09 — The Memory Model

Answers "How does this system remember things?" Covers three distinct memory regions: SRAM (where the running OS and threads live), flash (where the firmware and filesystem live), and the XIP window (flash mapped into the address space for zero-copy reads). Explains the 64 KB kernel heap and its first-fit boundary-tag allocator: how blocks are structured with size/free/next headers, how allocation works (first-fit search with optional splitting), and how deallocation coalesces adjacent free blocks. Explains the stack canary (0xDEADBEEF written at the bottom of every thread's stack, checked on every context switch). Covers static vs. dynamic allocation: TCB and PCB pools are statically allocated; thread stacks are dynamically allocated from the kernel heap. Explains the flash filesystem layout and why writing to flash requires halting both cores.

---

### 10 — Input, Output, and Device Interaction

Answers "How does the system talk to the outside world?" Introduces the device abstraction layer: every device is represented by a `device_t` struct containing function pointers for open, read, write, ioctl, and close. Walks through each device: the console (USB CDC non-blocking reads), the timer (64-bit microsecond counter via XIP), GPIO (ioctl-based direction and value control), the display (ST7789 SPI driver with ioctl commands for drawing), and the RGB LED (PWM-based active-low control). Explains the VFS routing layer that sits above the device layer: `vfs_open()` checks device mounts first, then routes to the filesystem. Traces a complete I/O operation — typing a character in the terminal, through the USB stack, through the console device, through VFS, into the shell.

---

### 11 — Processes, Tasks, and Control Flow

Answers "How does the system run multiple things at once?" Introduces the Thread Control Block (TCB) and Process Control Block (PCB) as the data structures that represent concurrent execution. Walks through every field of `tcb_t` — especially the fixed-offset fields that the context switch assembly depends on. Explains the six thread states (NEW, READY, RUNNING, BLOCKED, SLEEPING, ZOMBIE) and the transitions between them. Explains how a new thread is created: stack allocation from the kernel heap, construction of the initial ARM exception frame on the stack, and linking the thread into both the TCB pool and the scheduler's ready queue. Explains the zombie-and-reap lifecycle: how a thread marks itself ZOMBIE when it exits, and how the scheduler reclaims its stack and TCB slot on the next context switch. Explains the idle thread's role: always READY at the lowest priority, ensuring the scheduler never runs out of runnable threads.

---

### 12 — System Services

Answers "What does the kernel offer to threads?" Covers the two main categories of kernel services: synchronization primitives and the syscall interface. For synchronization, explains each primitive in turn — spinlock (busy-wait with interrupt disable), mutex (FIFO waiter queue with blocking), semaphore (counting, supports negative count = blocked threads), event flags (32-bit bitmask, wait-for-any or wait-for-all), and message queues (ring buffer, blocking send and receive). Explains why picoOS uses interrupt disable for atomicity instead of hardware LDREX/STREX instructions — a deliberate choice for teaching simplicity. Covers the 17 syscalls in `syscall.c`, dispatched via direct C function calls rather than the SVC exception instruction. Explains what this means: no privilege level boundary, no memory protection, but maximum clarity and debuggability for students.

---

### 13 — The User Interface: Shell and Command Layer

Answers "How does a user interact with this system?" Explains the USB CDC serial shell as the primary human interface. Walks through the command lifecycle: the readline loop reads one character at a time with backspace support, the tokenizer splits the input line in-place, and the dispatcher looks up the command name in the command table. Covers all 15 built-in commands with what each does and how it works. Explains the dynamic command registration system (`shell_register_cmd`) that allows kernel modules like WiFi and Bluetooth to add their own commands at initialization time. Covers the `config.txt` feature: AUTORUN for boot-time app launch and button bindings for Display Pack hardware. Explains the `run` command: how it looks up an app by name in `app_table[]`, spawns it in a new process, and returns immediately. Covers the `update` command: how it calls `reset_usb_boot()` to reboot into BOOTSEL mode for firmware flashing.

---

### 14 — Build, Run, and Debug

Answers "How do I get this running, and how do I know it's working?" Walks through the complete build pipeline from CMake configuration through compilation, linking, and UF2 generation. Explains every significant CMake variable and feature flag. Covers the four board targets and what changes between them. Explains flashing: drag-and-drop UF2 for initial setup, and the `update` shell command for iterative development. Explains `console.py` in depth: auto-detection logic (USB VID/PID), interactive mode with raw terminal emulation, file upload mode (`--upload`), and log capture. Explains `mem_report.py`: how it parses the ELF map file to report SRAM usage by subsystem. Covers debugging strategy: without a hardware debugger, the shell's `threads`, `mem`, `ps`, and `trace on/off` commands are the primary tools. Explains what each of these commands shows and how to interpret the output.

---

### 15 — Walking Through the Code

A guided execution trace from power-on to steady-state operation. Follows the exact call chain: ROM → stage2 bootloader → `main()` → kernel init sequence → `sched_start()` → idle thread (first to run) → SysTick fires → PendSV → shell thread runs → user types a command → tokenizer → dispatcher → command handler → back to shell loop. Includes detailed walkthroughs of: a thread voluntarily yielding via `sched_yield()`, a thread going to sleep via `sched_sleep()` and being woken by SysTick ISR, a message queue send/receive between two threads, a file write from VFS through the filesystem to flash commit. Ends with what happens when a stack canary is corrupted: the stack_overflow_panic path and why it re-enables interrupts before printing.

---

### 16 — Building the OS Step by Step

A staged engineering narrative reconstructing the likely development path of picoOS. Each of twelve stages presents a problem, the simplest first solution, the limitation of that solution, and the improvement that led to the current design. Stages: (1) Getting something to run — boot, USB, printf; (2) Getting reliable output — USB enumeration timing; (3) Managing memory — static arrays vs. the heap allocator; (4) Creating tasks — TCBs and exception frame setup; (5) Scheduling — cooperative to preemptive; (6) The context switch — ARM exception model, M0+ register constraints, the PendSV handler; (7) Synchronization — from spinlocks to queues; (8) Device abstraction — from direct register access to the vtable pattern; (9) Filesystem — flash constraints, the superblock model, XIP reads; (10) The shell — fixed menus to dynamic command tables; (11) The application model — app_table ABI; (12) Wireless — CYW43 async context, polling thread.

---

### 17 — Engineering Problems and Solutions

Five deep-dives into the hardest engineering problems in picoOS, explained at the level of "why did the engineer write it this way?" Topics: (1) The atomic sleep problem — the race condition in sched_sleep() and why the critical section is essential; (2) The M0+ context switch — why Cortex-M0+ cannot use STMDB for high registers and the staging-register workaround; (3) Flash write synchronization — what XIP means, why Core 1 must be halted, and how `multicore_lockout_start_blocking()` works; (4) Stack canary design — why 0xDEADBEEF is checked on every context switch rather than periodically; (5) The first thread problem — why calling the entry function directly is simpler than constructing a fake EXC_RETURN for the very first thread launch.

---

### 18 — Teaching with This Operating System

A chapter for teachers and curriculum designers. Explains how picoOS maps to standard OS textbook concepts (processes, scheduling, IPC, virtual memory, filesystems). Proposes a course flow that moves from hardware to boot to memory to scheduling to IPC to filesystem to shell. Explains how each intentional imperfection can be used as a teaching exercise: O(n) scan becomes visible with a thread benchmark; fragmentation accumulates with repeated allocation; synchronous console I/O can be measured with timing. Explains how to use the shell's built-in commands (`threads`, `mem`, `ps`, `trace on`) as classroom demonstration tools. Suggests pairing each OS concept with a student lab activity that makes the concept visible on real hardware.

---

### 19 — Student Activities and Labs

Ten structured lab activities progressing from beginner to advanced. Lab 1: Build and flash picoOS; connect with console.py; record the boot sequence output. Lab 2: Write and register a new shell command. Lab 3: Create a thread that blinks an LED; observe it in the `threads` output. Lab 4: Build a producer/consumer pair using message queues. Lab 5: Write a deeply recursive function and trigger a stack canary panic. Lab 6: Write and read back a file; verify superblock contents with `ls`. Lab 7 (pico_w): Run a WiFi scan and analyze RSSI values. Lab 8: Use mem_report.py before and after adding threads. Lab 9: Change TIME_SLICE_MS and measure scheduling behavior change. Lab 10: Implement and register a new app in app_table[]. Each lab includes setup, procedure, expected output, and reflection questions.

---

### 20 — Extension Projects

Ten substantial extension projects for advanced students or course final projects. Projects: (1) Replace first-fit with best-fit — measure fragmentation difference; (2) Implement a priority bitmap for O(1) ready-queue lookup; (3) Add fine-grained per-module locking; (4) Implement adaptive stack sizing using the canary to predict overflow; (5) Add a directory hierarchy to the filesystem; (6) Implement buffered async console I/O with a ring buffer; (7) Build a bytecode VM for interpreted programs (Phase 6 of picoOS design); (8) Implement SVC-based syscall dispatch with privilege levels; (9) Add a TCP client using lwIP (already linked for CYW43 boards); (10) Implement a deadlock detector using mutex waiter chain analysis. Each project includes background, suggested approach, and evaluation criteria.

---

### 21 — Known Limitations and Future Work

Documents what picoOS does not yet do, organized by development phase. Phase 5 items (not yet implemented): raw flash stream I/O, bulk GPIO operations. Phase 6 items (planned): bytecode VM. Structural limitations: single write buffer in the filesystem (one file open for writing at a time), maximum file size of 4 KB (one sector), no subdirectory support, synchronous console I/O that can stall on USB drain, Bluetooth scan-only (no connections). Trust-model limitations: no SVC boundary, no MPU enforcement, no per-process memory isolation. Explains why each limitation exists, whether it is intentional (teaching), structural (hardware constraint), or simply unimplemented (future work).

---

### 22 — Glossary

Alphabetical definitions of all technical terms used in the book. Covers: hardware terms (RP2040, RP2350, CYW43, XIP, QSPI, BOOTSEL, UF2), ARM architecture terms (Cortex-M0+, Cortex-M33, PSP, MSP, EXC_RETURN, PendSV, SysTick, NVIC), OS concepts (TCB, PCB, context switch, preemption, round-robin, time slice, zombie, spinlock, mutex, semaphore, event flags, message queue, VFS, ioctl, app_table), and memory terms (boundary-tag, first-fit, fragmentation, coalescing, stack canary). Each definition is one to three sentences, precise, and grounded in how the term is used in picoOS specifically.

---

### 23 — Index of Important Files

A reference index of every significant source file in the repository, organized by subsystem. For each file: path, line count, one-line purpose description, and key functions/structs defined within. Organized into sections: Boot and Initialization, Scheduler and Context Switch, Memory Management, Task and Process Management, Synchronization, System Calls, Device Abstraction and VFS, Filesystem, Shell, Drivers, Applications, WiFi and Bluetooth, Build and Tools, and Documentation.

---

### 24 — Appendix: Build Commands

Complete reference for all CMake invocations needed to build every board/feature combination. Includes: plain RP2040 pico (no features), pico with Display Pack, pico with Display Pack 2, pico2 (RP2350 plain), pico_w with WiFi and Bluetooth, pico_w with display and WiFi, pico2_w full features, and submodule build with custom app_table. Explains each CMake variable. Shows how to use the `build` and `clean` scripts. Includes common CMake error messages (missing SDK, wrong board name, linker overflow) and their fixes. Shows how to produce and read the disassembly file (`.dis`) and the ELF map file (`.elf.map`).

---

### 25 — Appendix: Code Reading Guide

A practical guide to navigating the picoOS source code. Explains how to set up clangd with the `compile_commands.json` symlink. Describes the key patterns found throughout the codebase: the vtable pattern (used in `device_t`), the pool pattern (used in TCB/PCB arrays), and the intrusive linked-list pattern (used in `tcb_t->next`). Explains how to trace a function's call chain using grep and editor navigation. Explains how to find where a shell command is handled (grep for the command name string in `shell.c`). Explains how to find where an ioctl command is defined (grep for the ioctl constant in `dev.h`). Includes a table of "if you want to understand X, read file Y" pointers covering the major subsystems.
