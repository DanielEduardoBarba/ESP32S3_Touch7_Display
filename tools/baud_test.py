#!/usr/bin/env python3
"""Automated peer-link baud-rate ladder test.

For each candidate rate (exact 80MHz divisors, see firmware ports.cpp), this:
  1. resets both devices' transfer state ("update-reset"),
  2. switches BOTH ends to the rate individually ("baudlocal N" -- no
     broadcast, so a failing rate can't strand the peer),
  3. triggers a full device-to-device firmware PULL (puller <- sender),
  4. watches both consoles: PASS on "Update verified (CRC OK)", FAIL on
     abort/timeout; link-level warnings (CRC INVALID / resyncing / retries)
     mark the rate MARGINAL even if the transfer completed.

The sweep stops at the first hard failure (higher rates can't do better),
restores both boards to a safe rate, and prints the verdict: the fastest
rate with a clean PASS and zero warnings.

Usage:
  python3 tools/baud_test.py --puller /dev/ttyACM0 --sender /dev/ttyACM1
  (quit any dev_monitor first -- this needs exclusive port access)
"""

import argparse
import re
import sys
import time

import serial

SAFE_RATE = 115200
DEFAULT_RATES = [500000, 640000, 800000, 1000000, 1250000, 1600000, 2000000, 2500000, 4000000, 5000000]

PASS_RE = re.compile(r"Update verified \(CRC OK\)")
FAIL_RE = re.compile(r"aborted|FAILURE|Failed|not acknowledged|No verification verdict|stalled")
WARN_RE = re.compile(r"CRC INVALID|resyncing|missing ETX|retry \d+/|RX overflow|stream break")


def open_port(port: str) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200  # USB CDC: value is cosmetic
    ser.timeout = 0.05
    ser.dtr = False  # never toggle the reset lines
    ser.rts = False
    ser.open()
    return ser


def send(ser: serial.Serial, cmd: str) -> None:
    ser.write((cmd + "\n").encode())
    ser.flush()


def drain(ser: serial.Serial) -> None:
    ser.reset_input_buffer()


def run_rate(puller: serial.Serial, sender: serial.Serial, rate: int, timeout_s: float):
    """Returns (passed: bool, warnings: int, elapsed: float, detail: str)."""
    for ser in (puller, sender):
        send(ser, "update-reset")
    time.sleep(1.0)
    # Switch each end LOCALLY (no broadcast) so a bad rate can't strand anyone.
    for ser in (puller, sender):
        send(ser, f"baudlocal {rate}")
    time.sleep(1.5)
    for ser in (puller, sender):
        drain(ser)

    send(puller, "pull")
    deadline = time.time() + timeout_s
    warnings = 0
    buf = {id(puller): b"", id(sender): b""}
    start = time.time()

    while time.time() < deadline:
        for ser in (puller, sender):
            data = ser.read(4096)
            if not data:
                continue
            buf[id(ser)] += data
            while b"\n" in buf[id(ser)]:
                line_b, buf[id(ser)] = buf[id(ser)].split(b"\n", 1)
                line = line_b.decode(errors="replace")
                if WARN_RE.search(line):
                    warnings += 1
                if ser is puller and PASS_RE.search(line):
                    return True, warnings, time.time() - start, "verified"
                if ser is puller and FAIL_RE.search(line):
                    return False, warnings, time.time() - start, line.strip()[:100]
        time.sleep(0.01)
    return False, warnings, time.time() - start, "timeout"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--puller", required=True, help="Serial port of the device that PULLS (receives+flashes)")
    ap.add_argument("--sender", required=True, help="Serial port of the device that SENDS its image")
    ap.add_argument("--rates", default=",".join(str(r) for r in DEFAULT_RATES),
                    help="Comma-separated ladder, ascending (default: %(default)s)")
    ap.add_argument("--timeout", type=float, default=300.0, help="Per-rate transfer timeout in seconds")
    args = ap.parse_args()

    rates = [int(r) for r in args.rates.split(",")]
    puller = open_port(args.puller)
    sender = open_port(args.sender)

    results = []
    best_clean = None
    try:
        for rate in rates:
            print(f"\n=== Testing {rate} baud ===", flush=True)
            passed, warnings, elapsed, detail = run_rate(puller, sender, rate, args.timeout)
            speed = ""
            if passed:
                speed = f", {elapsed:.0f}s"
            verdict = "PASS" if passed and warnings == 0 else ("MARGINAL" if passed else "FAIL")
            print(f"    -> {verdict} ({detail}{speed}, link warnings: {warnings})", flush=True)
            results.append((rate, verdict, warnings, elapsed, detail))
            if passed and warnings == 0:
                best_clean = rate
            if not passed:
                print("    Hard failure -- higher rates won't do better; stopping the sweep.")
                break
    finally:
        # Always land both boards on the safe rate + clean state.
        print(f"\nRestoring both devices to {SAFE_RATE} baud...")
        for ser in (puller, sender):
            send(ser, f"baudlocal {SAFE_RATE}")
            time.sleep(0.3)
            send(ser, "update-reset")
        puller.close()
        sender.close()

    print("\n=== Baud ladder results ===")
    for rate, verdict, warnings, elapsed, detail in results:
        print(f"  {rate:>8}: {verdict:<8} warnings={warnings:<4} {detail}")
    if best_clean:
        print(f"\nMax SAFE baud rate (clean pass, zero warnings): {best_clean}")
        print(f"Set it on both boards via the Ports page, or: 'baud {best_clean}' from one device.")
    else:
        print("\nNo clean pass found -- stay at 460800/115200.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
