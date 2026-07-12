#!/usr/bin/env python3
"""
Simple serial console for touch-esp32, Expo-Go style: watches the board's
serial output live, and pressing 'r' rebuilds + reflashes + resets the board
then goes right back to watching logs. No file-watching/hot-reload -- it's
a manual trigger only, exactly like Expo Go's "press r to reload".

Why not just `idf.py monitor`? On this board, the USB port enumerates as
the ESP32-S3's *native* USB-Serial/JTAG peripheral (not a classic external
UART bridge chip). `idf.py monitor` (esp-idf-monitor) issues its own
reset-on-connect using the RTS/DTR lines to guarantee it captures the full
boot log -- but for this native-USB peripheral, that same signal sequence
puts the chip into bootloader/download mode instead of running the app, so
you'd immediately see "waiting for download" instead of your program. This
script deliberately avoids touching RTS/DTR at all: it just opens the port
and reads, so whatever state the chip was already left in after flashing
(running the app) is undisturbed.

Usage:
    python3 tools/dev_monitor.py --port /dev/ttyACM0 --stage app --repo-root /path/to/repo

Keys (while this terminal is focused):
    r         rebuild + reflash `--stage`, then resume watching logs
    Ctrl+C    quit
"""
import argparse
import os
import select
import subprocess
import sys
import termios
import time
import tty

import serial

RED = "\033[31m"
YELLOW = "\033[33m"
GREEN = "\033[32m"
RESET = "\033[0m"


def open_port_passively(port: str, baud: int) -> serial.Serial:
    """Open the serial port without asserting/toggling DTR or RTS, so we
    never accidentally trigger a reset (see module docstring)."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.1
    # Leaving dtr/rts unset (None) before open() avoids pyserial's default
    # behavior of asserting them, which some drivers turn into a reset pulse.
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def wait_for_port(port: str, timeout_s: float = 8.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if os.path.exists(port):
            return True
        time.sleep(0.05)
    return False


def rebuild_and_flash(repo_root: str, stage: str, port: str) -> bool:
    """Runs `./build.sh --build <stage>` then `./build.sh --flash <stage>`.
    Returns True if both succeeded (i.e. the board was actually reset)."""
    build_sh = os.path.join(repo_root, "build.sh")

    print(f"\n{YELLOW}==> Rebuilding '{stage}'...{RESET}")
    build = subprocess.run([build_sh, "--build", stage], cwd=repo_root)
    if build.returncode != 0:
        print(f"{RED}==> Build failed (exit {build.returncode}). Not flashing; resuming logs.{RESET}\n")
        return False

    print(f"{YELLOW}==> Flashing '{stage}' to {port}...{RESET}")
    flash = subprocess.run([build_sh, "--flash", stage, "--port", port], cwd=repo_root)
    if flash.returncode != 0:
        print(f"{RED}==> Flash failed (exit {flash.returncode}). Resuming logs.{RESET}\n")
        return False

    print(f"{GREEN}==> Reflash complete, resuming logs...{RESET}")
    print(f"{YELLOW}    (If you don't see your app's logs below, this board's auto-reset isn't 100% reliable --{RESET}")
    print(f"{YELLOW}     press the physical RESET button once.){RESET}\n")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True, help="Serial device, e.g. /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--stage", default="app", choices=["factory", "app"],
                     help="Which firmware project 'r' rebuilds/reflashes")
    ap.add_argument("--repo-root", required=True, help="Path to the touch-esp32 repo root")
    args = ap.parse_args()

    print(f"Watching {args.port} @ {args.baud} baud.")
    print(f"Press 'r' to rebuild + reflash '{args.stage}' and resume watching. Ctrl+C to quit.\n")

    ser = open_port_passively(args.port, args.baud)

    # cbreak mode: read one keystroke at a time with no need to press Enter
    # (like Expo Go's "press r to reload"), while still letting Ctrl+C raise
    # KeyboardInterrupt normally (unlike raw mode, cbreak keeps ISIG enabled).
    stdin_fd = sys.stdin.fileno()
    old_term_settings = termios.tcgetattr(stdin_fd)
    tty.setcbreak(stdin_fd)

    try:
        while True:
            try:
                # Block briefly waiting for either a keystroke or serial
                # data, so we're not busy-polling the CPU while idle.
                readable, _, _ = select.select([sys.stdin, ser], [], [], 0.05)
            except (OSError, ValueError):
                # The port's file descriptor died out from under us -- most
                # likely the board was unplugged, power-cycled, or its
                # RESET button was pressed (this board's auto-reset circuit
                # isn't fully reliable, so a manual RESET press after
                # flashing is expected -- see Waveshare's own docs). Wait
                # for it to come back instead of crashing.
                print(f"\n{YELLOW}==> Lost connection to {args.port}, waiting for it to reappear...{RESET}")
                try:
                    ser.close()
                except Exception:
                    pass
                wait_for_port(args.port)
                time.sleep(0.3)
                ser = open_port_passively(args.port, args.baud)
                continue

            if sys.stdin in readable:
                ch = sys.stdin.read(1)
                if ch.lower() == "r":
                    ser.close()
                    reset_happened = rebuild_and_flash(args.repo_root, args.stage, args.port)
                    if reset_happened:
                        # The board's native USB peripheral drops off the bus
                        # for a moment during the actual reset; wait for it.
                        wait_for_port(args.port)
                        time.sleep(0.3)
                    ser = open_port_passively(args.port, args.baud)

            if ser in readable:
                try:
                    data = ser.read(ser.in_waiting or 1)
                except (OSError, serial.SerialException):
                    continue  # handled by the reconnect logic above on the next loop
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_term_settings)
        try:
            ser.close()
        except Exception:
            pass
        print("\nExiting monitor.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
