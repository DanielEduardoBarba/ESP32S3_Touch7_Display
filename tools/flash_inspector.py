#!/usr/bin/env python3
"""
Interactive flash/memory inspector for touch-esp32, hexdump-style.

Lets you browse the ESP32-S3's entire 16MB external flash like a file, in
the same `hexdump -C` format Linux uses:

    00110000  e9 06 02 30 68 5b 06 42  ee 00 00 00 03 03 00 00  |...0h[.B........|

The viewer knows the partition layout (parsed from firmware/partitions.csv
plus the fixed bootloader/partition-table regions), so the header always
tells you WHICH region you're looking at and what lies directly above and
below it, and you can jump straight to any partition by name.

Reading happens over the serial bootloader (esptool read_flash), so:
  - each cache-miss fetch resets the board into download mode, reads a
    64KB window, then hard-resets it back into the app (takes a few
    seconds; scrolling within an already-fetched window is instant), and
  - nothing else may hold the port (quit dev_monitor.py first).

Usage:
    python3 tools/flash_inspector.py --port /dev/ttyACM0 \
        --partitions-csv firmware/partitions.csv [--start ota_0|0x110000]

Keys:
    j / k / down / up        scroll one line (16 bytes)
    d / u / PgDn / PgUp      scroll one page
    g                        go to an address (hex like 0x110000) or a
                             partition name (like ota_0, nvs, webapp)
    n / p                    jump to the next / previous region boundary
    m                        show the full memory map
    r                        re-read the current window (drop cache)
    q / Ctrl+C               quit
"""
import argparse
import os
import subprocess
import sys
import tempfile
import termios
import tty

BOLD = "\033[1m"
DIM = "\033[2m"
CYAN = "\033[36m"
YELLOW = "\033[33m"
RED = "\033[31m"
RESET = "\033[0m"

CHIP_SIZE = 16 * 1024 * 1024   # 16MB flash on both board variants
FETCH_WINDOW = 0x10000         # 64KB read per esptool invocation
BYTES_PER_LINE = 16
LINES_PER_PAGE = 24


# ---------------------------------------------------------------------------
# Memory map
# ---------------------------------------------------------------------------

def load_regions(partitions_csv: str):
    """Full flash map: the two fixed ESP-IDF regions (2nd-stage bootloader
    and the partition table itself) plus every row of partitions.csv,
    sorted by offset. Unallocated space between/after them shows up as
    '(unmapped)' when browsing."""
    regions = [
        {"name": "bootloader", "offset": 0x0, "size": 0x8000},
        {"name": "partition_table", "offset": 0x8000, "size": 0x1000},
    ]
    with open(partitions_csv) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = [p.strip() for p in line.split(",")]
            if len(fields) < 5:
                continue
            regions.append({
                "name": fields[0],
                "offset": int(fields[3], 0),
                "size": int(fields[4], 0),
            })
    return sorted(regions, key=lambda r: r["offset"])


def region_at(regions, addr: int):
    """The region containing `addr` (or None if it falls in a gap), plus
    its neighbors above and below, for the context header."""
    current, below, above = None, None, None
    for r in regions:
        if r["offset"] + r["size"] <= addr:
            below = r
        elif r["offset"] <= addr < r["offset"] + r["size"]:
            current = r
        elif above is None and r["offset"] > addr:
            above = r
    return current, below, above


def find_region(regions, name: str):
    for r in regions:
        if r["name"] == name:
            return r
    return None


# ---------------------------------------------------------------------------
# Flash access (chunked + cached esptool reads)
# ---------------------------------------------------------------------------

class FlashReadError(RuntimeError):
    """A read over the serial bootloader failed (wrong chip on the port,
    port busy, board unplugged mid-read, ...). Caught by the main loop so
    the user gets a clean message instead of a traceback."""


class FlashReader:
    """Reads flash through esptool in aligned 64KB windows and caches them,
    so interactive scrolling only pays the (slow) serial round-trip on the
    first visit to each window."""

    def __init__(self, port: str, baud: int):
        self.port = port
        self.baud = baud
        self.cache = {}  # window base address -> bytes

    def _fetch_window(self, base: int) -> bytes:
        size = min(FETCH_WINDOW, CHIP_SIZE - base)
        print(f"\n{YELLOW}Reading flash 0x{base:06x}..0x{base + size:06x} over serial "
              f"(board resets briefly)...{RESET}")
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
            tmp_path = tmp.name
        try:
            result = subprocess.run(
                ["esptool.py", "--chip", "esp32s3", "-p", self.port, "-b", str(self.baud),
                 "read_flash", hex(base), hex(size), tmp_path],
                capture_output=True, text=True)
            if result.returncode != 0:
                # Surface only esptool's actual complaint (its last few
                # output lines), not the whole banner.
                output = (result.stderr or result.stdout or "").strip()
                tail = "\n".join(output.splitlines()[-3:])
                raise FlashReadError(tail)
            with open(tmp_path, "rb") as f:
                return f.read()
        finally:
            os.unlink(tmp_path)

    def read(self, addr: int, size: int) -> bytes:
        """Returns `size` bytes starting at `addr`, fetching (and caching)
        as many 64KB windows as needed to cover the range."""
        out = b""
        while size > 0:
            base = addr - (addr % FETCH_WINDOW)
            if base not in self.cache:
                self.cache[base] = self._fetch_window(base)
            window = self.cache[base]
            skip = addr - base
            take = min(size, len(window) - skip)
            if take <= 0:
                break  # past end of chip
            out += window[skip:skip + take]
            addr += take
            size -= take
        return out

    def drop_cache_around(self, addr: int):
        base = addr - (addr % FETCH_WINDOW)
        self.cache.pop(base, None)


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def hexdump_lines(data: bytes, start_addr: int):
    """Formats data exactly like `hexdump -C`: offset, 16 hex bytes split
    into two groups of 8, then the ASCII column."""
    lines = []
    for i in range(0, len(data), BYTES_PER_LINE):
        chunk = data[i:i + BYTES_PER_LINE]
        hex_left = " ".join(f"{b:02x}" for b in chunk[:8])
        hex_right = " ".join(f"{b:02x}" for b in chunk[8:])
        ascii_col = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{start_addr + i:08x}  {hex_left:<23}  {hex_right:<23}  |{ascii_col}|")
    return lines


def render_page(reader: FlashReader, regions, addr: int):
    data = reader.read(addr, BYTES_PER_LINE * LINES_PER_PAGE)
    current, below, above = region_at(regions, addr)

    os.system("clear")
    cur_desc = (f"{current['name']} (0x{current['offset']:06x}..0x{current['offset'] + current['size']:06x})"
                if current else "(unmapped)")
    below_desc = f"{below['name']}" if below else "-- start of flash --"
    above_desc = f"{above['name']}" if above else "-- end of flash --"
    print(f"{BOLD}Flash @ 0x{addr:06x}{RESET}  in {CYAN}{cur_desc}{RESET}")
    print(f"{DIM}below: {below_desc}    above: {above_desc}{RESET}")
    print(f"{DIM}{'-' * 78}{RESET}")
    for line in hexdump_lines(data, addr):
        print(line)
    print(f"{DIM}{'-' * 78}{RESET}")
    print(f"{DIM}j/k scroll  d/u page  g goto  n/p region  m map  r re-read  q quit{RESET}")


def print_map(regions):
    os.system("clear")
    print(f"{BOLD}Flash memory map (16MB){RESET}\n")
    print(f"  {'name':<18} {'start':>10} {'end':>10} {'size':>10}")
    prev_end = 0
    for r in regions:
        if r["offset"] > prev_end:
            print(f"  {DIM}{'(unmapped)':<18} {prev_end:>#10x} {r['offset']:>#10x} "
                  f"{r['offset'] - prev_end:>#10x}{RESET}")
        end = r["offset"] + r["size"]
        print(f"  {r['name']:<18} {r['offset']:>#10x} {end:>#10x} {r['size']:>#10x}")
        prev_end = end
    if prev_end < CHIP_SIZE:
        print(f"  {DIM}{'(unmapped)':<18} {prev_end:>#10x} {CHIP_SIZE:>#10x} "
              f"{CHIP_SIZE - prev_end:>#10x}{RESET}")
    print(f"\n{DIM}press any key to return to the hex view{RESET}")


# ---------------------------------------------------------------------------
# Input helpers
# ---------------------------------------------------------------------------

def read_key(stdin_fd) -> str:
    """One keypress; decodes arrow/page keys (ESC [ x sequences) into the
    same letters as their vim-style equivalents."""
    ch = sys.stdin.read(1)
    if ch != "\x1b":
        return ch
    seq = sys.stdin.read(2)
    return {"[A": "k", "[B": "j", "[5": "u", "[6": "d"}.get(seq, "")


def prompt_line(stdin_fd, old_settings, prompt: str) -> str:
    """Line input (for 'g'): temporarily restores normal terminal mode so
    the user can type/edit/Enter, then goes back to single-key mode."""
    termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_settings)
    try:
        return input(prompt).strip()
    except EOFError:
        return ""
    finally:
        tty.setcbreak(stdin_fd)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def clamp(addr: int) -> int:
    top = CHIP_SIZE - BYTES_PER_LINE * LINES_PER_PAGE
    return max(0, min(addr, top))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="Serial device, e.g. /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--partitions-csv", required=True, help="Path to firmware/partitions.csv")
    ap.add_argument("--start", default="0x0",
                     help="Start address (hex like 0x110000) or partition name (like ota_0)")
    args = ap.parse_args()

    regions = load_regions(args.partitions_csv)

    region = find_region(regions, args.start)
    if region is not None:
        addr = region["offset"]
    else:
        try:
            addr = int(args.start, 0)
        except ValueError:
            print(f"{RED}--start must be a hex address or one of: "
                  f"{', '.join(r['name'] for r in regions)}{RESET}")
            return 1
    addr = clamp(addr - (addr % BYTES_PER_LINE))

    reader = FlashReader(args.port, args.baud)
    page_bytes = BYTES_PER_LINE * LINES_PER_PAGE

    stdin_fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(stdin_fd)
    tty.setcbreak(stdin_fd)
    try:
        render_page(reader, regions, addr)
        while True:
            key = read_key(stdin_fd)
            if key == "q":
                break
            elif key == "j":
                addr = clamp(addr + BYTES_PER_LINE)
            elif key == "k":
                addr = clamp(addr - BYTES_PER_LINE)
            elif key == "d":
                addr = clamp(addr + page_bytes)
            elif key == "u":
                addr = clamp(addr - page_bytes)
            elif key == "n":  # start of the next region above the cursor
                nxt = next((r for r in regions if r["offset"] > addr), None)
                addr = clamp(nxt["offset"]) if nxt else addr
            elif key == "p":  # start of the previous region below the cursor
                prevs = [r for r in regions if r["offset"] < addr]
                addr = clamp(prevs[-1]["offset"]) if prevs else 0
            elif key == "g":
                dest = prompt_line(stdin_fd, old_settings, "go to (hex addr or partition name): ")
                region = find_region(regions, dest)
                if region is not None:
                    addr = clamp(region["offset"])
                else:
                    try:
                        addr = clamp(int(dest, 0) - (int(dest, 0) % BYTES_PER_LINE))
                    except ValueError:
                        pass  # unrecognized input -- just redraw where we were
            elif key == "m":
                print_map(regions)
                read_key(stdin_fd)
            elif key == "r":
                reader.drop_cache_around(addr)
            else:
                continue  # unknown key -- don't redraw

            render_page(reader, regions, addr)
    except KeyboardInterrupt:
        pass
    except FlashReadError as e:
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_settings)
        print(f"\n{RED}Flash read failed:{RESET}\n{e}\n")
        print(f"{YELLOW}Hints:{RESET}")
        print("  - Wrong device? Pass --port explicitly (e.g. ./build.sh --mem ota_0 --port /dev/ttyACM0)")
        print("  - Port busy? Quit any running monitor (./build.sh --run) first.")
        print("  - Board mid-reset? Just re-run the command.")
        return 1
    finally:
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_settings)
        print("\nExiting inspector. (The board was hard-reset back into the app after the last read.)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
