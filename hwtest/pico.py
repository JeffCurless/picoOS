"""
pico.py — drive a picoOS board over its USB shell, for the hardware tests
=========================================================================

The Pico class flashes a kit with picotool, opens the USB serial port, and
runs shell commands one at a time.

Commands go in step with the shell, not on a timer
--------------------------------------------------
The shell reads one line, runs the command, and only then prints "pico> "
and reads the next line.  Anything sent before that prompt is still read,
but by whatever reads the console next, and that is not always the shell
prompt.  `fs write <file>` reads lines for the file, so a command sent early
ends up as text in the file.  Sleeping between commands only makes this
less likely, and it gets worse when the board is busy.  So cmd() always:

  1. waits for the prompt that ends the previous command (sync),
  2. sends the line,
  3. waits for the prompt that ends this command, and returns the lines
     printed in between.

Nothing is ever sent ahead of the shell.  That makes the host the slow side:
one command costs a few milliseconds at best.  A test that needs two events
closer together than that should not be timed from the host.  Put the
sequence in the device-side `hwtest` app (src/apps/hwtest.c) and read its
verdict, or use a holder mode that keeps a state for seconds so the host
can act on it at its own pace.

Output that apps print while a command runs (other threads, IRQ callbacks)
lands in the same stream.  cmd() returns it mixed with the command's own
output, so tests should search the result, not compare it line for line.
expect() searches everything received since a mark, so a line printed
before you started waiting for it is still found.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
from console import DEFAULT_BAUD, find_pico_port  # noqa: E402

import serial  # noqa: E402  (console.py has already checked it is installed)

PROMPT = "pico> "
LINE_MAX = 127          # SHELL_LINE_MAX - 1: longer lines are cut by the shell


class PicoError(Exception):
    """The board did not answer as expected (timeout, lost connection)."""


class Pico:
    def __init__(self, port: str | None = None, baud: int = DEFAULT_BAUD,
                 log_path: str | None = None, echo: bool = False):
        self.port_arg = port
        self.baud = baud
        self.echo = echo                 # copy device output to stdout
        self.log = open(log_path, "a", encoding="utf-8") if log_path else None
        self.ser: serial.Serial | None = None
        self._buf = ""                   # everything received this connection (no CRs)
        self._cv = threading.Condition()
        self._reader: threading.Thread | None = None
        self._stop = threading.Event()
        self._lost = False
        self._sync_n = 0

    # ------------------------------------------------------------------
    # Logging
    # ------------------------------------------------------------------
    def note(self, text: str) -> None:
        """Write a host-side note to the log (and stdout when echoing)."""
        line = f"\n### {text}\n"
        if self.log:
            self.log.write(line)
            self.log.flush()
        if self.echo:
            sys.stdout.write(line)
            sys.stdout.flush()

    # ------------------------------------------------------------------
    # Flashing
    # ------------------------------------------------------------------
    def flash(self, kit: str, timeout: float = 60.0) -> None:
        """Load kit with `picotool load <kit> --force`.

        --force makes a running picoOS reboot into BOOTSEL through its USB
        reset interface, and picotool reboots it back into the new image when
        the load is done.  The serial port is closed first and must be
        reopened with connect().
        """
        self.close()
        self.note(f"flash {kit}")
        cmd = ["picotool", "load", kit, "--force"]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        except FileNotFoundError:
            raise PicoError("picotool not found on PATH")
        except subprocess.TimeoutExpired:
            raise PicoError(f"picotool load timed out after {timeout:.0f} s")
        out = (r.stdout + r.stderr).strip()
        if self.log:
            self.log.write(out + "\n")
        if r.returncode != 0:
            raise PicoError(f"picotool load failed ({r.returncode}):\n{out}")

    # ------------------------------------------------------------------
    # Connection
    # ------------------------------------------------------------------
    def connect(self, timeout: float = 30.0) -> None:
        """Open the serial port (waiting for the board to enumerate after a
        flash or reboot) and sync with the shell prompt.  If the port drops
        while we wait (the board was still resetting), start over."""
        deadline = time.monotonic() + timeout
        last_err = "no Pico serial port found"
        while time.monotonic() < deadline:
            port = self.port_arg or find_pico_port()
            if not port:
                time.sleep(0.25)
                continue
            try:
                # exclusive: refuse to share the port with a console.py.
                self.ser = serial.Serial(port, self.baud, timeout=0.05,
                                         exclusive=True)
            except serial.SerialException as e:
                last_err = str(e)
                time.sleep(0.25)
                continue

            self._buf = ""
            self._lost = False
            self._stop.clear()
            self._reader = threading.Thread(target=self._read_loop, daemon=True)
            self._reader.start()
            self.note(f"connected {self.ser.port}")

            # The shell prints nothing until it sees DTR (set by the open)
            # and may still be booting: keep poking until a prompt comes back.
            while time.monotonic() < deadline and not self._lost:
                try:
                    self.sync(timeout=2.0)
                    return
                except PicoError as e:
                    last_err = str(e)
            self.close()
            last_err = "port dropped while waiting for the shell"
        raise PicoError(f"could not reach the shell: {last_err}")

    def reconnect(self, gone_timeout: float = 5.0, timeout: float = 40.0) -> None:
        """After something that resets the board (`reboot`, a crash): close,
        wait for the port to disappear so we do not reopen the old one, then
        connect() to the new one."""
        self.close()
        deadline = time.monotonic() + gone_timeout
        while time.monotonic() < deadline and self._port_present():
            time.sleep(0.1)
        self.connect(timeout=timeout)

    def reboot(self, timeout: float = 40.0) -> None:
        """Run the shell `reboot` and reconnect.  Does not wait for its
        "Rebooting..." line: the watchdog reset often beats it to the host."""
        self.start("reboot")
        self.reconnect(timeout=timeout)

    def _port_present(self) -> bool:
        if self.port_arg:
            return os.path.exists(self.port_arg)
        return find_pico_port() is not None

    def close(self) -> None:
        self._stop.set()
        if self._reader:
            self._reader.join(timeout=1.0)
            self._reader = None
        if self.ser:
            try:
                self.ser.close()
            except serial.SerialException:
                pass
            self.ser = None

    def shutdown(self) -> None:
        self.close()
        if self.log:
            self.log.close()
            self.log = None

    def _read_loop(self) -> None:
        while not self._stop.is_set():
            try:
                data = self.ser.read(self.ser.in_waiting or 1)
            except (serial.SerialException, OSError, TypeError):
                with self._cv:
                    self._lost = True
                    self._cv.notify_all()
                return
            if not data:
                continue
            text = data.decode("utf-8", errors="replace")
            if self.log:
                self.log.write(text)
                self.log.flush()
            if self.echo:
                sys.stdout.write(text)
                sys.stdout.flush()
            with self._cv:
                # Stored without CRs, so a mark is a position in exactly the
                # text that expect() searches.
                self._buf += text.replace("\r", "")
                self._cv.notify_all()

    # ------------------------------------------------------------------
    # Waiting on output
    # ------------------------------------------------------------------
    def mark(self) -> int:
        """Position in the received stream; pass it to expect(since=...)."""
        with self._cv:
            return len(self._buf)

    def text_since(self, since: int) -> str:
        with self._cv:
            return self._buf[since:]

    def expect(self, pattern: str, timeout: float = 5.0,
               since: int | None = None) -> re.Match:
        """Wait until pattern (a regex) appears in output received since the
        mark `since` (default: now).  Returns the match; raises PicoError on
        timeout or a lost connection."""
        if since is None:
            since = self.mark()
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        with self._cv:
            while True:
                m = rx.search(self._buf, since)
                if m:
                    return m
                if self._lost:
                    raise PicoError("serial connection lost")
                left = deadline - time.monotonic()
                if left <= 0:
                    tail = self._buf[since:][-400:]
                    raise PicoError(f"timed out after {timeout:.1f} s waiting "
                                    f"for /{pattern}/; last output:\n{tail}")
                self._cv.wait(left)

    def absent(self, pattern: str, duration: float, since: int | None = None) -> bool:
        """True if pattern does NOT appear within duration seconds."""
        try:
            self.expect(pattern, timeout=duration, since=since)
            return False
        except PicoError:
            if self._lost:
                raise
            return True

    # ------------------------------------------------------------------
    # Sending
    # ------------------------------------------------------------------
    def _write(self, text: str) -> None:
        if self.ser is None or self._lost:
            raise PicoError("not connected")
        try:
            self.ser.write(text.encode("utf-8"))
            self.ser.flush()
        except serial.SerialException as e:
            raise PicoError(f"write failed: {e}")

    def sync(self, timeout: float = 5.0) -> None:
        """Bring the shell to an idle prompt with no unread input.

        Sends a unique token as a command and waits for the shell's
        "Unknown command: '<token>'" reply and the prompt after it.  A plain
        empty line is not enough: if an earlier line is still unanswered
        (sent while the board was booting, or by a test that gave up), its
        prompt can arrive after ours, and the next cmd() would take it as
        its own and return early.  The shell answers lines in order, so
        everything older is printed before the token's reply."""
        self._sync_n += 1
        token = f"hwsync{self._sync_n}"
        since = self.mark()
        self._write(token + "\r")
        m = self.expect(re.escape(f"Unknown command: '{token}'"),
                        timeout=timeout, since=since)
        self.expect(re.escape(PROMPT), timeout=timeout, since=m.end())

    def cmd(self, line: str, timeout: float = 10.0) -> str:
        """Run one shell command and return what it printed (CRs removed,
        the echoed command line dropped).

        Waits for the command's own prompt, so the next cmd() never runs
        into this one.  The caller must start at a prompt: every cmd() ends
        at one, and connect()/sync() leave the shell at one."""
        if len(line) > LINE_MAX:
            raise ValueError(f"command longer than {LINE_MAX} characters")
        if "\r" in line or "\n" in line:
            raise ValueError("one line per cmd()")
        since = self.mark()
        self._write(line + "\r")
        # The shell echoes the line, prints "\r\n", runs the command, then
        # prints the prompt.  The echo is not matched exactly: an app that
        # prints while the line is echoed splits it.  No command prints
        # "pico> " itself, and the shell was idle at a prompt before we
        # sent, so the first prompt after `since` is this command's.
        m = self.expect(re.escape(PROMPT), timeout=timeout, since=since)
        with self._cv:
            body = self._buf[since:m.start()]
        first, _, rest = body.partition("\n")
        return rest if line in first else body

    def start(self, line: str) -> int:
        """Send a command that does not come straight back to "pico> " (one
        that prompts for input, reboots, ...) and return a mark from before
        it, for expect().  Like cmd(), call it only at an idle prompt."""
        if len(line) > LINE_MAX:
            raise ValueError(f"command longer than {LINE_MAX} characters")
        since = self.mark()
        self._write(line + "\r")
        return since

    def lines(self, text: str) -> list[str]:
        return [ln for ln in text.split("\n") if ln.strip()]

    def type_lines(self, lines: list[str], prompt: str = "> ",
                   end_prompt: str = PROMPT, timeout: float = 10.0) -> str:
        """Answer a command that reads its own input lines (`fs write <f>`):
        send each line only after the command prints `prompt` for it.  The
        last line should be the command's terminator (e.g. "."); waits for
        `end_prompt` after it and returns everything printed."""
        since = self.mark()
        for i, ln in enumerate(lines):
            want = end_prompt if i == len(lines) - 1 else prompt
            start = self.mark()
            self._write(ln + "\r")
            # Wait for the prompt after this line's echo ("\n" ends it).
            nl = self.expect(r"\n", timeout=timeout, since=start)
            self.expect(re.escape(want), timeout=timeout, since=nl.end())
        return self.text_since(since)

    # ------------------------------------------------------------------
    # Parsed views of common commands
    # ------------------------------------------------------------------
    def threads(self) -> dict[int, dict]:
        """`threads` as {tid: {pid, state, name}}."""
        out = {}
        for ln in self.lines(self.cmd("threads")):
            f = ln.split()
            if len(f) >= 8 and f[0].isdigit():
                out[int(f[0])] = {"pid": int(f[1]), "state": f[4], "name": f[7]}
        return out

    def processes(self) -> dict[int, dict]:
        """`ps` as {pid: {name, threads, alive}}."""
        out = {}
        for ln in self.lines(self.cmd("ps")):
            f = ln.split()
            if len(f) >= 4 and f[0].isdigit():
                out[int(f[0])] = {"name": f[1], "threads": int(f[2]),
                                  "alive": f[3] == "yes"}
        return out

    def wait_until(self, predicate, timeout: float = 5.0, every: float = 0.2,
                   what: str = "condition") -> None:
        """Poll predicate() (which may run commands) until it is true."""
        deadline = time.monotonic() + timeout
        while True:
            if predicate():
                return
            if time.monotonic() >= deadline:
                raise PicoError(f"timed out after {timeout:.1f} s waiting for {what}")
            time.sleep(every)
