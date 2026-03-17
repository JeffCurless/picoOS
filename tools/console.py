#!/usr/bin/env python3
"""
picoOS Console — Host-side terminal companion
===================================================
Connects to a Raspberry Pi Pico running picoOS over USB serial (CDC).

Usage
-----
  python3 console.py                        # auto-detect Pico, interactive
  python3 console.py --port /dev/ttyACM0   # use a specific port
  python3 console.py --baud 115200         # override baud rate (default 115200)
  python3 console.py --log output.log      # tee all console output to a file
  python3 console.py --upload FILE DEST    # upload a file using 'write' command
  python3 console.py --help

Key bindings (interactive mode)
--------------------------------
  Ctrl-C  — exit
  Any other key/line — sent directly to the Pico shell

Dependencies
------------
  pip install pyserial
"""

import argparse
import sys
import os
import select
import threading
import time
import tty
import termios

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial is not installed.")
    print("       Install it with:  pip install pyserial")
    sys.exit(1)


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

PICO_USB_VID = 0x2E8A  # Raspberry Pi
PICO_USB_PID = 0x000A  # Pico USB serial (CDC)

DEFAULT_BAUD = 115200


# ---------------------------------------------------------------------------
# Port detection
# ---------------------------------------------------------------------------

def find_pico_port():
    """Scan connected serial ports and return the first one that looks like
    a Raspberry Pi Pico USB CDC device.  Returns None if not found."""
    ports = list(serial.tools.list_ports.comports())
    for port in ports:
        vid = getattr(port, "vid", None)
        pid = getattr(port, "pid", None)
        if vid == PICO_USB_VID and pid == PICO_USB_PID:
            return port.device
        # Fall back to description matching for systems that do not expose
        # the VID/PID through the port info object.
        desc = (port.description or "").lower()
        if "pico" in desc or "rp2040" in desc:
            return port.device
    return None


def list_serial_ports():
    """Print all detected serial ports."""
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports detected.")
        return
    print(f"{'Device':<20} {'VID:PID':<12} Description")
    print("-" * 70)
    for p in ports:
        vid_pid = ""
        if p.vid is not None and p.pid is not None:
            vid_pid = f"{p.vid:04X}:{p.pid:04X}"
        print(f"{p.device:<20} {vid_pid:<12} {p.description or ''}")


# ---------------------------------------------------------------------------
# Log file helper
# ---------------------------------------------------------------------------

class Tee:
    """Writes data to both a file and an optional second stream."""

    def __init__(self, path):
        self._file = open(path, "ab")  # append, binary

    def write(self, data: bytes):
        self._file.write(data)
        self._file.flush()

    def close(self):
        self._file.close()


# ---------------------------------------------------------------------------
# Background reader thread
# ---------------------------------------------------------------------------

def reader_thread(ser: serial.Serial, tee: "Tee | None", stop_event: threading.Event,
                  disconnected: "threading.Event | None" = None):
    """Continuously read bytes from the serial port and print them to stdout.
    Also writes to the log file if a Tee is provided.
    Sets *disconnected* if the port disappears unexpectedly."""
    while not stop_event.is_set():
        try:
            waiting = ser.in_waiting
            if waiting > 0:
                data = ser.read(waiting)
                text = data.decode("utf-8", errors="replace")
                sys.stdout.write(text)
                sys.stdout.flush()
                if tee:
                    tee.write(data)
            else:
                time.sleep(0.01)
        except serial.SerialException:
            if disconnected:
                disconnected.set()
            break
        except Exception:
            if disconnected:
                disconnected.set()
            break


# ---------------------------------------------------------------------------
# Interactive mode
# ---------------------------------------------------------------------------

def _wait_for_connection(port: "str | None", baud: int,
                         poll_interval: float = 1.0,
                         quit_event: "threading.Event | None" = None) -> "serial.Serial | None":
    """Block until the Pico port is available, then return an open Serial.

    Returns None if the user presses Ctrl-C (\\x03) while waiting.
    Uses select() on stdin so Ctrl-C is detected even in raw terminal mode,
    where SIGINT is suppressed and the byte must be read explicitly.
    """
    while True:
        if quit_event and quit_event.is_set():
            return None
        p = port or find_pico_port()
        if p:
            try:
                ser = serial.Serial(p, baud, timeout=0.1)
                time.sleep(0.2)
                ser.reset_input_buffer()
                return ser
            except serial.SerialException:
                pass
        # Poll in short bursts so stdin (Ctrl-C) is checked every 0.1 s.
        deadline = time.monotonic() + poll_interval
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            r, _, _ = select.select([sys.stdin], [], [], min(remaining, 0.1))
            if r:
                ch = sys.stdin.buffer.read(1)
                if ch == b'\x03':  # Ctrl-C in raw mode
                    if quit_event:
                        quit_event.set()
                    return None


def interactive_mode(port: "str | None", baud: int, tee: "Tee | None"):
    """Forward keystrokes to the Pico in raw terminal mode.

    Automatically reconnects when the Pico reboots or is briefly unplugged.
    Ctrl-C exits entirely.
    """
    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)

    try:
        tty.setraw(fd)

        quit_event = threading.Event()

        while True:  # outer reconnect loop
            ser = _wait_for_connection(port, baud, quit_event=quit_event)
            if ser is None:  # Ctrl-C pressed during reconnect wait
                break
            sys.stdout.write(f"\r\n[console] Connected ({ser.port}).  "
                             "Press Ctrl-C to exit.\r\n")
            sys.stdout.flush()

            disconnected = threading.Event()
            stop_event = threading.Event()
            t = threading.Thread(target=reader_thread,
                                 args=(ser, tee, stop_event, disconnected),
                                 daemon=True)
            t.start()

            user_quit = False
            while not disconnected.is_set():
                r, _, _ = select.select([sys.stdin], [], [], 0.2)
                if not r:
                    continue
                ch = sys.stdin.buffer.read(1)
                if not ch or ch == b'\x03':  # Ctrl-C
                    user_quit = True
                    quit_event.set()
                    break
                # Map bare \n to \r so the Pico shell sees Enter correctly.
                if ch == b'\n':
                    ch = b'\r'
                try:
                    ser.write(ch)
                    if tee:
                        tee.write(ch)
                except serial.SerialException:
                    break

            stop_event.set()
            t.join(timeout=1.0)
            ser.close()

            if user_quit:
                break

            sys.stdout.write("\r\n[console] Pico disconnected — "
                             "waiting for reconnect...\r\n")
            sys.stdout.flush()

    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
        sys.stdout.write("\r\n[console] Disconnected.\r\n")


# ---------------------------------------------------------------------------
# Upload mode
# ---------------------------------------------------------------------------

def upload_file(ser: serial.Serial, local_path: str, remote_name: str):
    """Upload a local file to the Pico by sending a 'write' shell command.

    The 'write' command in the picoOS shell takes:
        write <filename> <data>
    so we chunk the file content into multiple write calls if it is large.

    For simplicity this implementation sends the entire file as a single
    write command (suitable for small text files).  Binary files or files
    larger than SHELL_LINE_MAX should be chunked — marked as a TODO.
    """
    if not os.path.isfile(local_path):
        print(f"[upload] ERROR: '{local_path}' is not a file.")
        return False

    with open(local_path, "rb") as f:
        content = f.read()

    # Decode as UTF-8; the picoOS filesystem stores text.
    try:
        text = content.decode("utf-8")
    except UnicodeDecodeError:
        print("[upload] ERROR: file contains non-UTF-8 data.  "
              "Only text files are supported in this version.")
        return False

    # Strip the remote name of path separators for safety.
    remote_name = os.path.basename(remote_name)

    print(f"[upload] Uploading '{local_path}' -> '{remote_name}' "
          f"({len(text)} bytes)...")

    # Build the write command.
    # NOTE: newlines inside the file content are not handled by the single-line
    # shell protocol.  For multi-line files, consider a chunked protocol or
    # a dedicated transfer command in a future phase.
    safe_text = text.replace("\r\n", " ").replace("\n", " ").replace("\r", " ")
    cmd = f"write {remote_name} {safe_text}\r\n"

    ser.write(cmd.encode("utf-8"))
    ser.flush()

    # Wait briefly for the Pico to process and echo a response.
    time.sleep(0.5)
    waiting = ser.in_waiting
    if waiting > 0:
        response = ser.read(waiting).decode("utf-8", errors="replace")
        sys.stdout.write(response)
        sys.stdout.flush()

    print(f"[upload] Done.")
    return True


# ---------------------------------------------------------------------------
# Connection helper
# ---------------------------------------------------------------------------

def open_connection(port: "str | None", baud: int) -> serial.Serial:
    """Open a serial connection to the Pico, auto-detecting the port if
    port is None.  Exits the program on failure."""
    if port is None:
        port = find_pico_port()
        if port is None:
            print("ERROR: No Raspberry Pi Pico found.")
            print("       Connect a Pico running picoOS and try again,")
            print("       or specify the port with --port /dev/ttyXXX")
            print()
            list_serial_ports()
            sys.exit(1)
        print(f"[console] Auto-detected Pico at {port}")
    else:
        print(f"[console] Using port {port}")

    try:
        ser = serial.Serial(port, baud, timeout=0.1)
    except serial.SerialException as e:
        print(f"ERROR: Cannot open {port}: {e}")
        sys.exit(1)

    print(f"[console] Port {port} opened at {baud} baud")

    # Give the CDC stack a moment to settle.
    time.sleep(0.2)

    # Read and discard any stale data in the buffer.
    ser.reset_input_buffer()

    return ser


# ---------------------------------------------------------------------------
# Device info
# ---------------------------------------------------------------------------

def print_device_info(port_device: str):
    """Print info about the device on the given port."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if p.device == port_device:
            print(f"[console] Device info:")
            print(f"           Port       : {p.device}")
            print(f"           Description: {p.description}")
            if p.vid is not None:
                print(f"           VID:PID    : {p.vid:04X}:{p.pid:04X}")
            if p.manufacturer:
                print(f"           Manufacturer: {p.manufacturer}")
            if p.serial_number:
                print(f"           Serial     : {p.serial_number}")
            return


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="picoOS Console — host-side terminal companion",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--port", "-p",
        metavar="PORT",
        default=None,
        help="Serial port device (e.g. /dev/ttyACM0, COM3).  "
             "Auto-detected if not specified.",
    )
    parser.add_argument(
        "--baud", "-b",
        metavar="BAUD",
        type=int,
        default=DEFAULT_BAUD,
        help=f"Baud rate (default: {DEFAULT_BAUD}).",
    )
    parser.add_argument(
        "--log", "-l",
        metavar="LOGFILE",
        default=None,
        help="Tee all console output to this file.",
    )
    parser.add_argument(
        "--upload", "-u",
        metavar=("FILE", "DEST"),
        nargs=2,
        help="Upload FILE to the Pico filesystem as DEST.",
    )
    parser.add_argument(
        "--list-ports",
        action="store_true",
        help="List all detected serial ports and exit.",
    )
    return parser


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = build_arg_parser()
    args = parser.parse_args()

    if args.list_ports:
        list_serial_ports()
        return

    # Set up log file tee.
    tee = None
    if args.log:
        tee = Tee(args.log)
        print(f"[console] Logging to '{args.log}'")

    try:
        if args.upload:
            # Upload is a one-shot operation — open a connection explicitly.
            ser = open_connection(args.port, args.baud)
            print_device_info(ser.port)
            try:
                local_file, remote_name = args.upload
                upload_file(ser, local_file, remote_name)
            finally:
                ser.close()
        else:
            interactive_mode(args.port, args.baud, tee)
    finally:
        if tee:
            tee.close()


if __name__ == "__main__":
    main()
