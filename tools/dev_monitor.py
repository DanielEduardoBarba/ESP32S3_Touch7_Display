#!/usr/bin/env python3
"""
Multi-device serial console for touch-esp32, Expo-Go style.

Watches ALL connected boards' serial output at once (each line tagged with
which device it came from), and lets you rebuild + reflash one, several, or
all of them with a single keypress -- no file-watching/hot-reload, it's a
manual trigger only, exactly like Expo Go's "press r to reload".

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
    python3 tools/dev_monitor.py --port /dev/ttyACM0 --port /dev/ttyACM1 \\
        --stage app --repo-root /path/to/repo
    (build.sh passes one --port per auto-detected device automatically;
    you only need to pass --port yourself to override which devices are used)

Keys (while this terminal is focused):
    0         target ALL devices (this is the default on startup)
    1-9       target only that device number (see the numbering with 'h')
    r         rebuild + reflash the default stage (--stage, normally 'app')
              to the TARGETED device(s), then resume logs
    f         rebuild + reflash the 'factory' (recovery) stage
    a         rebuild + reflash 'all' (factory + app) -- the whole firmware
    b         build the default stage only (compile check, no flash --
              device-independent, runs regardless of the current target)
    w         rebuild the web UI only (npm build; flashed on next 'r'/'a')
    h         show the device list (which /dev/ttyACM... is which number)
              and this key reference
    Ctrl+C    quit
"""
import argparse
import glob
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
CYAN = "\033[36m"
BOLD = "\033[1m"
RESET = "\033[0m"

# One color per device index (cycled if there are more devices than colors),
# used to prefix that device's log lines so multiple boards' output is easy
# to tell apart at a glance.
DEVICE_COLORS = ["\033[36m", "\033[35m", "\033[33m", "\033[32m", "\033[34m", "\033[91m"]


def default_ports(include_usb: bool = False):
    """Auto-detects connected boards when no --port is given at all.
    Only /dev/ttyACM* by default (this project's boards enumerate via their
    CH343 bridge as ttyACM); /dev/ttyUSB* devices are usually unrelated
    boards/dongles and are only included when --usbdevs is passed."""
    ports = sorted(glob.glob("/dev/ttyACM*"))
    if include_usb:
        ports += sorted(glob.glob("/dev/ttyUSB*"))
    return ports


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


class Device:
    """One connected board: its serial connection, a per-device read
    buffer (so we print whole lines, not arbitrary chunks), and enough
    identity (index/color/tag) to make multi-device logs readable."""

    def __init__(self, index: int, port: str, baud: int):
        self.index = index
        self.port = port
        self.baud = baud
        self.color = DEVICE_COLORS[(index - 1) % len(DEVICE_COLORS)]
        self.buffer = b""
        self.ser = None
        self.connected = False
        self._open()

    def _open(self):
        try:
            self.ser = open_port_passively(self.port, self.baud)
            self.connected = True
        except (OSError, serial.SerialException):
            self.ser = None
            self.connected = False

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.connected = False

    def tag(self) -> str:
        return f"{self.color}[{self.index}:{os.path.basename(self.port)}]{RESET}"

    def try_reconnect(self):
        """Cheap, non-blocking check: if the device node exists again after
        having disappeared (unplug, reset, mid-flash), reopen it. Doesn't
        block the main loop or other devices' logs while waiting."""
        if self.connected:
            return
        if os.path.exists(self.port):
            self._open()
            if self.connected:
                print(f"{self.tag()} {GREEN}reconnected{RESET}")

    def read_and_print(self):
        """Reads whatever is available and prints complete lines, tagged
        with this device's identity. Returns False if the read failed
        (device went away), so the caller can mark it disconnected."""
        try:
            data = self.ser.read(self.ser.in_waiting or 1)
        except (OSError, serial.SerialException):
            return False
        if data:
            self.buffer += data
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                sys.stdout.write(f"{self.tag()} {line.decode(errors='replace')}\n")
            sys.stdout.flush()
        return True

    def mark_disconnected(self, reason: str):
        print(f"\n{self.tag()} {YELLOW}{reason}{RESET}")
        self.close()


def build_only(repo_root: str, stage: str, variant: str) -> None:
    """Runs `./build.sh --build <stage> --variant <variant>` only -- no
    flashing, no board reset, so every device's serial connection is left
    completely undisturbed. Useful for a fast "does it compile" check."""
    print(f"\n{YELLOW}==> Building '{stage}' for variant '{variant}' (compile check only, not flashing)...{RESET}")
    build_sh = os.path.join(repo_root, "build.sh")
    build = subprocess.run([build_sh, "--build", stage, "--variant", variant], cwd=repo_root)
    if build.returncode != 0:
        print(f"{RED}==> Build failed (exit {build.returncode}).{RESET}\n")
    else:
        print(f"{GREEN}==> Build succeeded. (Press 'r' to flash it.){RESET}\n")


def build_web(repo_root: str) -> None:
    """Rebuilds only the React web UI (web/dist). The result is packed into
    the webapp partition image on the next firmware build, so follow up
    with 'r' (or 'a') to actually get it onto a board."""
    print(f"\n{YELLOW}==> Rebuilding the web UI (npm build)...{RESET}")
    build_sh = os.path.join(repo_root, "build.sh")
    build = subprocess.run([build_sh, "--web"], cwd=repo_root)
    if build.returncode != 0:
        print(f"{RED}==> Web build failed (exit {build.returncode}).{RESET}\n")
    else:
        print(f"{GREEN}==> Web UI built. (Press 'r' to flash it with the app.){RESET}\n")


def rebuild_and_flash(repo_root: str, stage: str, variant: str, devices) -> None:
    """Builds once (the same binary goes to every targeted device), then
    flashes each targeted device in turn. Each device's serial connection is
    closed right before its flash and reopened right after, exactly like
    the single-device version -- other devices not being flashed keep their
    connections open throughout, but note that ALL log output is paused for
    the duration of this whole operation since it's one single-threaded
    script (logs resume as soon as the last flash finishes)."""
    build_sh = os.path.join(repo_root, "build.sh")

    print(f"\n{YELLOW}==> Rebuilding '{stage}' for variant '{variant}' (shared by all targeted devices)...{RESET}")
    build = subprocess.run([build_sh, "--build", stage, "--variant", variant], cwd=repo_root)
    if build.returncode != 0:
        print(f"{RED}==> Build failed (exit {build.returncode}). Not flashing; resuming logs.{RESET}\n")
        return

    for device in devices:
        print(f"{YELLOW}==> Flashing '{stage}' to {device.tag()} ({device.port})...{RESET}")
        device.close()
        flash = subprocess.run([build_sh, "--flash", stage, "--variant", variant, "--port", device.port], cwd=repo_root)
        if flash.returncode != 0:
            print(f"{RED}==> Flash failed for {device.tag()} (exit {flash.returncode}).{RESET}")
        else:
            print(f"{GREEN}==> {device.tag()} reflashed.{RESET}")
        # The board's native/USB peripheral drops off the bus for a moment
        # during the actual reset; wait for it before moving on.
        wait_for_port(device.port)
        time.sleep(0.3)
        device._open()

    print(f"{GREEN}==> All targeted devices done, resuming logs...{RESET}")
    print(f"{YELLOW}    (If a device's logs don't show up below, this board's auto-reset isn't 100% reliable --{RESET}")
    print(f"{YELLOW}     press its physical RESET button once.){RESET}\n")


def print_help(devices, stage: str, variant: str, target: int) -> None:
    print(f"\n{BOLD}=== Devices ==={RESET}")
    for d in devices:
        status = f"{GREEN}connected{RESET}" if d.connected else f"{RED}disconnected{RESET}"
        print(f"  {d.tag()} {d.port} [{status}]")

    print(f"\n{BOLD}=== Keys ==={RESET}")
    print("  0        Target ALL devices")
    print("  1-9      Target only that device number")
    print(f"  r        Rebuild + reflash '{stage}' to the TARGETED device(s)")
    print("  f        Rebuild + reflash 'factory' (recovery stage)")
    print("  a        Rebuild + reflash 'all' (factory + app)")
    print(f"  b        Build '{stage}' only (compile check, no flash)")
    print("  w        Rebuild web UI only (npm build; flash it with 'r'/'a')")
    print("  h        Show this help")
    print("  Ctrl+C   Quit")

    target_desc = "ALL" if target == 0 else f"device {target}"
    print(f"\n{BOLD}Current target:{RESET} {target_desc}")
    print(f"{BOLD}Board variant:{RESET} {variant}  (change with --variant 7|7b when starting the monitor)\n")


def devices_for_target(devices, target: int):
    if target == 0:
        return [d for d in devices]
    for d in devices:
        if d.index == target:
            return [d]
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", action="append", dest="ports", default=None,
                     help="Serial device to watch, e.g. /dev/ttyACM0. Repeatable. "
                          "If omitted entirely, auto-detects all /dev/ttyUSB*|/dev/ttyACM* devices.")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--stage", default="app", choices=["factory", "app"],
                     help="Which firmware project 'r'/'b' build/reflash")
    ap.add_argument("--variant", default="7", choices=["7", "7b"],
                     help="Board hardware variant 'r'/'b' build/reflash with (default: 7). Must match "
                          "whatever the connected device(s) actually are, or 'r' will flash the wrong config.")
    ap.add_argument("--repo-root", required=True, help="Path to the touch-esp32 repo root")
    ap.add_argument("--usbdevs", action="store_true",
                     help="Also auto-detect /dev/ttyUSB* devices (default: /dev/ttyACM* only, "
                          "so unrelated boards/dongles are never picked up by accident)")
    args = ap.parse_args()

    port_list = args.ports if args.ports else default_ports(args.usbdevs)
    if not port_list:
        print(f"{RED}No serial devices found (looked for /dev/ttyACM*"
              f"{' and /dev/ttyUSB*' if args.usbdevs else ''}).{RESET}")
        print("Plug in a board and make sure your user is in the 'dialout' group (./build.sh --setup).")
        return 1

    devices = [Device(i + 1, port, args.baud) for i, port in enumerate(port_list)]
    target = 0  # 0 = all devices (the default)

    print(f"Watching {len(devices)} device(s) @ {args.baud} baud. Press 'h' for help.\n")
    print_help(devices, args.stage, args.variant, target)

    # cbreak mode: read one keystroke at a time with no need to press Enter
    # (like Expo Go's "press r to reload"), while still letting Ctrl+C raise
    # KeyboardInterrupt normally (unlike raw mode, cbreak keeps ISIG enabled).
    stdin_fd = sys.stdin.fileno()
    old_term_settings = termios.tcgetattr(stdin_fd)
    tty.setcbreak(stdin_fd)

    try:
        while True:
            # Only select() on devices that are currently connected; a
            # disconnected device is retried cheaply below instead, so one
            # board being unplugged/mid-reset never blocks the others.
            ser_to_device = {d.ser: d for d in devices if d.connected}
            read_list = [sys.stdin] + list(ser_to_device.keys())

            try:
                readable, _, _ = select.select(read_list, [], [], 0.05)
            except (OSError, ValueError):
                # One of the fds died between building the list and
                # selecting on it (race with a device disappearing) --
                # just loop again, the dead one will fail its own read/be
                # dropped by try_reconnect() on the next pass.
                readable = []

            for r in readable:
                if r is sys.stdin:
                    ch = sys.stdin.read(1)

                    if ch == "h":
                        print_help(devices, args.stage, args.variant, target)

                    elif ch.isdigit():
                        n = int(ch)
                        if n == 0:
                            target = 0
                            print(f"\n{CYAN}==> Target changed to ALL devices.{RESET}\n")
                        elif any(d.index == n for d in devices):
                            target = n
                            d = next(d for d in devices if d.index == n)
                            print(f"\n{CYAN}==> Target changed to device {n} ({d.port}).{RESET}\n")
                        else:
                            print(f"\n{RED}==> No device {n} (only {len(devices)} connected). "
                                  f"Target unchanged.{RESET}\n")

                    elif ch.lower() == "b":
                        build_only(args.repo_root, args.stage, args.variant)

                    elif ch.lower() == "w":
                        build_web(args.repo_root)

                    # r/f/a all share the same rebuild+reflash flow and only
                    # differ in WHICH build.sh stage they pass through:
                    #   r -> the default stage (--stage, normally 'app')
                    #   f -> 'factory' (the recovery stage)
                    #   a -> 'all' (factory + app together)
                    elif ch.lower() in ("r", "f", "a"):
                        stage_for_key = {"r": args.stage, "f": "factory", "a": "all"}[ch.lower()]
                        targeted = devices_for_target(devices, target)
                        if not targeted:
                            print(f"\n{RED}==> No devices match the current target; nothing to flash.{RESET}\n")
                        else:
                            rebuild_and_flash(args.repo_root, stage_for_key, args.variant, targeted)

                else:
                    device = ser_to_device[r]
                    if not device.read_and_print():
                        device.mark_disconnected("lost connection, waiting for it to reappear...")

            # Cheap, non-blocking retry for any device that's currently down.
            for d in devices:
                if not d.connected:
                    d.try_reconnect()
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_term_settings)
        for d in devices:
            d.close()
        print("\nExiting monitor.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
