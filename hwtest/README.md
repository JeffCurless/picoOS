# picoOS hardware tests

These tests run on a real board. The runner flashes a kit with
`picotool load <kit> --force`, then drives the picoOS shell over USB serial
and checks what it prints. The host unit tests (`./build tests`) cover the
kernel's logic. These tests cover what only shows up on hardware: two cores,
real flash, USB, and the radio.

For now the target is a **Pico W with a Display Pack** (`picowos_D` kits).
Other boards work with `--board`/`--variant`, and tests that need a feature
the kit lacks are skipped.

## Running

```bash
./build pico                         # or ./build — makes kits/picowos_D-v*.uf2
python3 hwtest/run.py                # flash the newest picowos_D kit, run everything
python3 hwtest/run.py --no-flash     # test what the board already runs
python3 hwtest/run.py -k kill        # only tests whose name contains "kill"
python3 hwtest/run.py --kit kits/picowos_D-v0.4.8.uf2
python3 hwtest/run.py -v             # echo the device output live
python3 hwtest/run.py --slow         # also run slow tests (scantest, ~75 s)
python3 hwtest/run.py --list
```

Requirements:
- `pyserial`
- picotool 2.3.0 on `PATH` (see `docs/setup.md`)
- the board running picoOS, or in BOOTSEL mode

Close any `tools/console.py` session first. The runner opens the port
exclusively, and `picotool --force` needs the board's USB reset interface.

Each run writes the full device output, with `### TEST …` markers, to
`hwtest/logs/<time>.log` (git-ignored). The exit status is 0 only if every
test passed or was skipped.

The tests create files named `hwt_*.txt` and delete them afterwards. A
`config.txt` on the board (for example `AUTORUN=`) is left alone.

## Files

| File | What it is |
|------|------------|
| `run.py` | Runner: picks and flashes the kit, connects, runs `test_*.py`, prints a summary |
| `pico.py` | `Pico`: flash, connect/reconnect, `cmd()`, `expect()`, parsed `threads()`/`ps()` |
| `harness.py` | `check()`, `@requires(...)`, the `Context` each test gets |
| `test_00_smoke.py` | Version matches the kit, commands present, core threads running |
| `test_10_fs.py` | `fs write`/`append`/`cat`/`rm`, multi-line write, a file surviving `reboot`, over-long names refused (#7), truncation surviving a reboot (#18), a full-length line keeping its newline (#39), other fds during a write and after a delete (#31, #8) |
| `test_20_kill.py` | Issue #26: a kill does not leak a held mutex or open files |
| `test_30_stacks.py` | Runs every app that can run unattended, then reports each thread's peak stack use and a suggested `stack_size` |
| `../src/apps/hwtest.c` | Device side: `run hwtest <mode>` |

## Commands go in step with the shell, never on a timer

The shell reads a line, runs it, prints `pico> `, and only then reads the
next line. Input that arrives early is not lost. It sits in the USB buffer
until something reads the console, and that may not be the shell prompt:
`fs write <file>` reads lines into the file, and an app could read them too.
Sleeping between commands only makes this less likely, and the right sleep
changes whenever the board is busy.

So `Pico` never sends ahead of the shell:

- **`cmd(line)`** sends one line and returns once that command's prompt comes
  back. The next `cmd()` therefore always starts at an idle shell.
- **`sync()`** (run before every test) sends a unique token, `hwsyncN`, and
  waits for `Unknown command: 'hwsyncN'` followed by the prompt. An empty line
  is not enough. A line sent while the board was still booting can be
  answered *after* ours, and its stray prompt would end the next `cmd()`
  early. That really happened during development: `cat` came back empty after
  a reboot. The shell answers lines in order, so once the token's reply has
  arrived, nothing older is still pending. The `Unknown command` lines in the
  log are these tokens.
- **`start(line)` + `expect()`** handle commands that don't go straight back to
  `pico> `, such as `fs write <f>` (which prompts `> `). **`type_lines()`**
  then sends each line only after its prompt.
- **`expect(regex, since=mark)`** searches everything received since a mark.
  An app's line that arrived before you started waiting still matches.
  Output from other threads is mixed into command output, so tests search
  for what they need rather than comparing whole outputs.

## When the host is too slow: push the timing onto the device

Each `cmd()` round trip costs a few milliseconds. A test that needs two
events closer together than that, such as "kill it *while* it holds the
lock, then check the next instant", can't be timed from the host. The
`hwtest` app (`src/apps/hwtest.c`, built into every kit that has the demo
apps) handles those in two ways:

- **Self-checking modes** run the whole sequence on the board and print
  `[hwtest] <mode>: RESULT: PASS|FAIL`, plus one `ok`/`FAIL` line per check.
  The host only starts them and reads the verdict. Example: `kill-mutex`
  kills a thread while it holds a `kmutex` and checks that, right after the
  kill, the thread still exists, is `kill_pending`, and has `mutex_depth == 1`.
- **Holder modes** set up a state and hold it for seconds, or forever, and
  print the PID/TID the host needs. The window is wide, so host timing
  doesn't matter. Example: `holdfile` keeps `hwt_hold.txt` open for writing
  until it is killed.

| Mode | Kind | Does |
|------|------|------|
| `kill-mutex` | self-checking | a kill waits for `kmutex_unlock`; the mutex is free afterwards |
| `kill-file` | self-checking | a killed process's open file is committed and the write buffer freed |
| `fs-longname` | self-checking | `vfs_open` refuses a 16-char name and makes no entry under its 15-char prefix (#7) |
| `fs-fds` | self-checking | another fd reads the committed file during a pending write; an fd left open on a deleted file fails instead of reading the next file (#31, #8) |
| `holdmutex` | holder | holds a mutex for 3 s, then releases it |
| `holdfile` | holder | keeps `hwt_hold.txt` open for writing, forever |
| `leakfile` | holder | writes `hwt_leak.txt` and exits without closing it |

`run hwtest` with no mode lists them.

## Stack measurement (`test_30_stacks.py`)

This file sizes `app_table[]` stacks from measurements. It resets the
records (`stacks reset`), then runs:
- the demo apps
- `pi` in both modes
- the `hwtest` modes
- a spread of shell commands, plus `wifi`/`bt scan` on W boards
- `scantest`, with `--slow`

`test_stack_report` then prints a table like this, and saves it to
`hwtest/logs/stacks-<time>.txt`:

```
thread            peak  size  use  seen  suggest  note
scantest          1264  2048  61%     1     1792  app_table[]
shell             1096  2048  53%     1     1536
pi                 608  2048  29%     2     1024  app_table[]
pi-worker           96  2048   4%     8      512
```

Short-lived threads, such as the `pi` workers, are gone before `threads`
could catch them. The measurement is therefore taken on the device:
`task_free_thread()` records each thread's high-water mark as it frees it,
keyed by thread name. The shell `stacks` command merges those records with
the live threads.

The suggested size is the peak plus a quarter (at least 256 B), rounded up
to 256 B. A reading only covers the paths that ran, so it is a lower bound,
and the margin is for the paths that didn't. The test fails if any thread
used more than 80% of its stack.

Run `-k test_30` (add `--slow` for `scantest`) to get a complete report.
`ctx.info()` is how a test adds lines like these under its result.

## Adding a test

Add a function `test_<name>(ctx)` to a `test_NN_<area>.py` file. Files run in
name order, and tests within a file run in source order.

```python
from harness import check, requires

@requires("wifi")                      # skipped on kits without WiFi
def test_wifi_status(ctx):
    out = ctx.pico.cmd("wifi status")
    check("WiFi state:" in out, "wifi status answers")
```

`ctx` carries `pico`, `kit`, `version`, `board`, `variant` and `features`
(`wifi`, `bt`, `display`, `led`, and `slow` with `--slow`), and
`ctx.info(text)` prints a report under the test's result. Use `check()` rather than `assert`. Delete
any files you create in a `finally:` block. If a test needs timing tighter
than a shell round trip, add a mode to `hwtest.c` rather than adding sleeps
on the host.
