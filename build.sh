#!/usr/bin/env bash
#
# build.sh -- bootstrap, build, and flash helper for the touch-esp32 project.
#
# Usage:
#   ./build.sh --setup              One-time machine setup on a fresh Ubuntu
#                                    install: apt packages, serial port
#                                    permissions, ESP-IDF toolchain, Node.js.
#   ./build.sh --web                Build the React UI (web/) into web/dist.
#   ./build.sh --build [stage]      Build firmware. stage: factory|app|all
#                                    (default: all).
#   ./build.sh --flash [stage]      Flash already-built firmware to a
#                                    connected board. stage (default: all):
#                                      factory  bootloader + partition table
#                                               + otadata + recovery app
#                                               (0x0..0x10000 region + factory)
#                                      app      main app -> ota_0 (0x110000)
#                                               + web UI -> webapp (0x710000).
#                                               Bootloader/recovery untouched.
#                                      all      factory, then app (no erase;
#                                               NVS/settings survive).
#                                      full     ERASES THE ENTIRE CHIP first
#                                               (wipes NVS/WiFi credentials,
#                                               otadata, ota_1, webapp), then
#                                               flashes factory + app. Use for
#                                               a truly pristine board state.
#   ./build.sh --install [stage]    web + build + flash in one go (default:
#                                    all; also accepts full). This is what
#                                    you want for a brand new board.
#   ./build.sh --run [stage]        Connect and watch live serial output from
#                                    EVERY connected board at once (stage:
#                                    factory|app, default: app) -- does NOT
#                                    build or flash on startup. Each device's
#                                    log lines are tagged/colored so you can
#                                    tell multiple boards apart. While
#                                    watching: '0' targets ALL devices
#                                    (default), '1'-'9' targets one device,
#                                    'r' rebuilds+reflashes the default stage
#                                    to the targeted device(s), 'f' the
#                                    factory/recovery stage, 'a' all stages
#                                    (factory+app), 'b' compile-check only,
#                                    'w' rebuilds the web UI only, 'h' shows
#                                    a help menu (device numbering + keys),
#                                    Ctrl+C quits.
#   ./build.sh --monitor [stage]    Same as --run (identical behavior; kept
#                                    as a separate name for clarity).
#   ./build.sh --menuconfig <stage> Open idf.py menuconfig for one stage.
#   ./build.sh --clean [stage]      Remove build/ dirs.
#   ./build.sh --mem-map            Print the full flash memory map (fixed
#                                    bootloader/partition-table regions +
#                                    all partitions + unmapped gaps) --
#                                    offline, no board needed.
#   ./build.sh --mem [addr|name]    Interactively browse a connected board's
#                                    ACTUAL flash contents in `hexdump -C`
#                                    format, starting at a hex address or a
#                                    partition name (default 0x0). Shows
#                                    which region you're in plus what lies
#                                    above/below; scroll with j/k/u/d, jump
#                                    with 'g' (address or partition name),
#                                    hop region boundaries with n/p, 'm' for
#                                    the map, 'q' to quit. Reads go over the
#                                    serial bootloader in cached 64KB
#                                    windows (each uncached read briefly
#                                    resets the board). Examples:
#                                      ./build.sh --mem ota_0
#                                      ./build.sh --mem 0x110000
#   ./build.sh --port /dev/ttyXXX   Override auto-detected serial port for
#                                    any of the above (can appear anywhere).
#   ./build.sh --usbdevs            Also consider /dev/ttyUSB* devices during
#                                    port auto-detection. By default ONLY
#                                    /dev/ttyACM* is scanned (this project's
#                                    boards), so an unrelated ttyUSB board or
#                                    dongle can never be flashed by accident.
#                                    (--port /dev/ttyUSBx also works without
#                                    this flag -- explicit choice always wins.)
#   ./build.sh --variant 7|7b       Select board hardware revision (default:
#                                    7). Waveshare ESP32-S3-Touch-LCD-7 (the
#                                    original, ST7262 LCD driver) vs -7B (the
#                                    newer revision, ST7701 LCD driver -- not
#                                    yet officially supported upstream, see
#                                    firmware/README.md "Board variants").
#                                    Applies to build/flash/install/run/
#                                    monitor/menuconfig/clean; can appear
#                                    anywhere. Switching variants on a stage
#                                    that was already built for the other one
#                                    auto-cleans its stale build output.
#   ./build.sh --help
#
# Notes:
#   - If your shell session was opened before your user was added to the
#     'dialout' group (e.g. right after the first `--setup` run), any
#     command that needs the serial port transparently re-runs itself via
#     `sg dialout` -- you don't need to log out/in first.
#   - --build/--flash/--install act on exactly ONE board: if you have more
#     than one ESP32/serial device plugged in, pass `--port /dev/ttyXXX` to
#     pick which one; otherwise the first /dev/ttyUSB*|/dev/ttyACM* found is
#     used. --run/--monitor is the exception -- it watches ALL connected
#     boards by default (use its '1'-'9' keys to target just one for
#     reflashing), unless you also pass --port here to restrict it to one.
#
# Examples:
#   ./build.sh --setup
#   ./build.sh --install
#   ./build.sh --install --variant 7b   # for a Waveshare -7B board
#   ./build.sh --run app            # watch all boards; 0/1-9 target, b/r build/flash, h help
#   ./build.sh --build app && ./build.sh --flash app --port /dev/ttyUSB0
#
set -euo pipefail

# --------------------------------------------------------------------------
# Paths / constants
# --------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$SCRIPT_DIR/firmware"
WEB_DIR="$SCRIPT_DIR/web"
PARTITIONS_CSV="$FIRMWARE_DIR/partitions.csv"

IDF_INSTALL_DIR="${IDF_INSTALL_DIR:-$HOME/esp/esp-idf}"
IDF_BRANCH="${IDF_BRANCH:-v5.3.1}"
IDF_TARGET="esp32s3"
FLASH_BAUD="${FLASH_BAUD:-921600}"
NODE_MIN_MAJOR=18

PORT_OVERRIDE=""
# Include /dev/ttyUSB* devices in port auto-detection (0 = ttyACM only).
# Off by default so an unrelated ttyUSB board/dongle can never be flashed
# by accident; enable with --usbdevs, or target one directly with --port.
USB_DEVS=0
# Board variant: "7" (original, Kconfig-supported) or "7b" (newer revision,
# custom board config -- see firmware/README.md "Board variants"). Applies
# to --build/--flash/--install/--run/--monitor/--menuconfig/--clean.
VARIANT="7"
# Kept so ensure_dialout_group() can re-exec this exact invocation via `sg`.
ORIGINAL_ARGS=("$@")

# --------------------------------------------------------------------------
# Logging helpers
# --------------------------------------------------------------------------
c_reset="\033[0m"; c_bold="\033[1m"; c_green="\033[32m"; c_yellow="\033[33m"; c_red="\033[31m"
log()  { echo -e "${c_bold}${c_green}==>${c_reset} $*"; }
warn() { echo -e "${c_bold}${c_yellow}==>${c_reset} $*"; }
err()  { echo -e "${c_bold}${c_red}==>${c_reset} $*" >&2; }
die()  { err "$*"; exit 1; }

usage() { sed -n '2,107p' "$0" | sed 's/^# \{0,1\}//'; }

# --------------------------------------------------------------------------
# Setup (fresh machine bootstrap)
# --------------------------------------------------------------------------
cmd_setup() {
    if ! command -v apt-get >/dev/null 2>&1; then
        die "This --setup flow targets Debian/Ubuntu (apt-get not found). Install ESP-IDF prerequisites manually: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/linux-macos-setup.html"
    fi

    log "Updating apt and installing ESP-IDF + serial + build prerequisites..."
    sudo apt-get update -y
    sudo apt-get install -y \
        git wget curl flex bison gperf python3 python3-pip python3-venv \
        cmake ninja-build ccache libffi-dev libssl-dev dfu-util \
        libusb-1.0-0 build-essential unzip

    log "Adding '$USER' to the 'dialout' group (serial port access for /dev/ttyUSB*/ttyACM*)..."
    if ! groups "$USER" | grep -qw dialout; then
        sudo usermod -aG dialout "$USER"
        warn "Added to 'dialout'. This only takes effect in a NEW login session:"
        warn "  log out/in, reboot, or run 'newgrp dialout' in your current shell."
    else
        log "'$USER' is already in the 'dialout' group."
    fi

    log "Installing a udev rule so ModemManager (if present) ignores CH340/CH343 USB-serial adapters..."
    local udev_rule="/etc/udev/rules.d/99-touch-esp32-serial.rules"
    if [[ ! -f "$udev_rule" ]]; then
        echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ENV{ID_MM_DEVICE_PROCESS}="0"' \
            | sudo tee "$udev_rule" >/dev/null
        sudo udevadm control --reload-rules
        sudo udevadm trigger
    fi

    if [[ -x "$IDF_INSTALL_DIR/install.sh" ]]; then
        log "ESP-IDF already present at $IDF_INSTALL_DIR (skipping clone). Re-running its installer for target '$IDF_TARGET'..."
    else
        log "Cloning ESP-IDF ($IDF_BRANCH) into $IDF_INSTALL_DIR ..."
        mkdir -p "$(dirname "$IDF_INSTALL_DIR")"
        git clone -b "$IDF_BRANCH" --recursive --depth 1 https://github.com/espressif/esp-idf.git "$IDF_INSTALL_DIR"
    fi

    log "Installing ESP-IDF Python environment + toolchain for $IDF_TARGET (this takes a while the first time)..."
    ( cd "$IDF_INSTALL_DIR" && ./install.sh "$IDF_TARGET" )

    # Make `get_idf` available in future shells, matching upstream ESP-IDF convention.
    local rc_line=". \"$IDF_INSTALL_DIR/export.sh\" > /dev/null 2>&1"
    local alias_line="alias get_idf='$rc_line'"
    if ! grep -qF "get_idf=" "$HOME/.bashrc" 2>/dev/null; then
        {
            echo ""
            echo "# Added by touch-esp32/build.sh --setup"
            echo "$alias_line"
        } >> "$HOME/.bashrc"
        log "Added 'get_idf' alias to ~/.bashrc (run 'get_idf' in new shells to load the ESP-IDF environment)."
    fi

    setup_node

    log "Machine setup complete."
    warn "IMPORTANT: start a new shell (or run 'newgrp dialout') before flashing, so serial port permissions apply."
    log "Next: ./build.sh --install   (builds the web UI + both firmware stages, and flashes a connected board)"
}

setup_node() {
    local node_major=0
    if command -v node >/dev/null 2>&1; then
        node_major="$(node -v | sed -E 's/^v([0-9]+).*/\1/')"
    fi

    if [[ "$node_major" -ge "$NODE_MIN_MAJOR" ]] 2>/dev/null; then
        log "Node.js $(node -v) already installed and new enough for Vite."
        return
    fi

    log "Installing Node.js (>= $NODE_MIN_MAJOR) via NodeSource, needed to build the React UI..."
    curl -fsSL "https://deb.nodesource.com/setup_20.x" | sudo -E bash -
    sudo apt-get install -y nodejs
}

# --------------------------------------------------------------------------
# ESP-IDF environment
# --------------------------------------------------------------------------
ensure_idf_env() {
    if command -v idf.py >/dev/null 2>&1; then
        return
    fi
    if [[ -f "$IDF_INSTALL_DIR/export.sh" ]]; then
        log "Sourcing ESP-IDF environment from $IDF_INSTALL_DIR ..."
        # shellcheck disable=SC1091
        source "$IDF_INSTALL_DIR/export.sh" >/dev/null
    fi
    command -v idf.py >/dev/null 2>&1 || die "idf.py not found. Run './build.sh --setup' first (or 'source \$IDF_PATH/export.sh')."
}

# --------------------------------------------------------------------------
# Serial-port group membership ('dialout')
# --------------------------------------------------------------------------
# Actions that open a serial port (flash/monitor) need the 'dialout' group.
# `./build.sh --setup` adds the user to it, but group membership is fixed
# for the lifetime of a login session -- a freshly-added user won't see it
# without logging out/in. Rather than making that a hard requirement, we
# transparently re-exec this whole script via `sg dialout` (runs a command
# with an extra group active, no new login needed) the first time we notice
# it's missing, then continue normally.
ensure_dialout_group() {
    if id -nG | tr ' ' '\n' | grep -qx dialout; then
        return
    fi
    if [[ "${TOUCH_ESP32_SG_REEXEC:-0}" == "1" ]]; then
        die "Still not in the 'dialout' group after re-exec via 'sg dialout'. Log out/in (or reboot) and try again."
    fi

    warn "This shell session doesn't have the 'dialout' group active yet -- re-running via 'sg dialout' so the serial port is accessible..."
    local quoted="$0"
    local a
    for a in "${ORIGINAL_ARGS[@]}"; do
        quoted+=" $(printf '%q' "$a")"
    done
    export TOUCH_ESP32_SG_REEXEC=1
    exec sg dialout -c "$quoted"
}

# --------------------------------------------------------------------------
# Serial port auto-detection
# --------------------------------------------------------------------------
find_esp32_port() {
    if [[ -n "$PORT_OVERRIDE" ]]; then
        echo "$PORT_OVERRIDE"
        return
    fi

    local candidates=()
    shopt -s nullglob
    # Only /dev/ttyACM* by default: this project's boards enumerate via
    # their CH343 bridge as ttyACM. ttyUSB* devices are usually UNRELATED
    # boards/dongles (e.g. a classic ESP32), and flashing one by accident
    # is destructive -- so they are never auto-selected unless the user
    # explicitly opts in with --usbdevs. (--port always works for any path.)
    candidates+=(/dev/ttyACM*)
    if [[ $USB_DEVS -eq 1 ]]; then
        candidates+=(/dev/ttyUSB*)
    fi
    shopt -u nullglob

    if [[ ${#candidates[@]} -eq 0 ]]; then
        die "No /dev/ttyACM* device found. Plug in the board (USB port labeled UART), and make sure your user is in the 'dialout' group (./build.sh --setup). To target a /dev/ttyUSB* device instead, pass --usbdevs or --port /dev/ttyUSBx."
    elif [[ ${#candidates[@]} -eq 1 ]]; then
        echo "${candidates[0]}"
    else
        warn "Multiple serial ports found: ${candidates[*]}" >&2
        warn "Using the first one (${candidates[0]}). Override with --port /dev/ttyXXX." >&2
        echo "${candidates[0]}"
    fi
}

# Lists every candidate serial device, one per line (used by the multi-device
# monitor, which -- unlike single-target flash/build -- watches ALL of them
# by default instead of picking just one).
find_all_esp32_ports() {
    if [[ -n "$PORT_OVERRIDE" ]]; then
        echo "$PORT_OVERRIDE"
        return
    fi

    local candidates=()
    shopt -s nullglob
    candidates+=(/dev/ttyACM*)
    if [[ $USB_DEVS -eq 1 ]]; then
        candidates+=(/dev/ttyUSB*)
    fi
    shopt -u nullglob

    if [[ ${#candidates[@]} -eq 0 ]]; then
        die "No /dev/ttyUSB* or /dev/ttyACM* device found. Plug in at least one board, and make sure your user is in the 'dialout' group (./build.sh --setup)."
    fi
    printf '%s\n' "${candidates[@]}"
}

# --------------------------------------------------------------------------
# partitions.csv lookup (name -> hex offset)
# --------------------------------------------------------------------------
get_partition_offset() {
    local name="$1"
    awk -F',' -v name="$name" '
        {
            gsub(/^[ \t]+|[ \t]+$/, "", $1)
            if ($1 == name) {
                offset = $4
                gsub(/^[ \t]+|[ \t]+$/, "", offset)
                print offset
                found = 1
                exit
            }
        }
        END { if (!found) exit 1 }
    ' "$PARTITIONS_CSV"
}

# --------------------------------------------------------------------------
# Web UI build
# --------------------------------------------------------------------------
cmd_web() {
    command -v npm >/dev/null 2>&1 || die "npm not found. Run './build.sh --setup' first (installs Node.js), or install Node.js >= $NODE_MIN_MAJOR manually."
    log "Building the React UI (web/) ..."
    ( cd "$WEB_DIR" && npm ci && npm run build )
    log "Web UI built at web/dist. It will be packed into the 'webapp' SPIFFS image on the next firmware build."
}

# --------------------------------------------------------------------------
# Firmware build / flash
# --------------------------------------------------------------------------
stage_dir() { echo "$FIRMWARE_DIR/$1"; }

cmd_build() {
    local stage="${1:-all}"
    ensure_idf_env
    case "$stage" in
        factory) ensure_variant_consistency factory; idf.py -C "$(stage_dir factory)" build ;;
        app)     ensure_variant_consistency app; idf.py -C "$(stage_dir app)" build ;;
        all)     cmd_build factory; cmd_build app ;;
        *) die "Unknown stage '$stage' (expected factory|app|all)" ;;
    esac
}

flash_factory() {
    local port="$1"
    ensure_variant_consistency factory
    log "Flashing 'factory' stage (bootloader + partition table + factory app) to $port ..."
    idf.py -C "$(stage_dir factory)" -p "$port" -b "$FLASH_BAUD" flash
}

flash_app() {
    local port="$1"
    ensure_idf_env
    ensure_variant_consistency app
    log "Building 'app' stage ..."
    idf.py -C "$(stage_dir app)" build

    local app_dir; app_dir="$(stage_dir app)"
    local app_bin="$app_dir/build/app.bin"
    local webapp_bin="$app_dir/build/webapp.bin"
    [[ -f "$app_bin" ]] || die "Expected build output not found: $app_bin"

    local ota0_offset webapp_offset
    ota0_offset="$(get_partition_offset ota_0)" || die "Could not find 'ota_0' partition in $PARTITIONS_CSV"
    webapp_offset="$(get_partition_offset webapp)" || die "Could not find 'webapp' partition in $PARTITIONS_CSV"

    # NOTE: we intentionally do NOT use `idf.py -C firmware/app flash` here.
    # Because firmware/partitions.csv (shared with the factory stage) lists
    # a `factory` partition before `ota_0`, idf.py's default "first app
    # partition" heuristic would otherwise try to flash this app image over
    # the factory splash stage. Writing directly to the known `ota_0` /
    # `webapp` offsets (parsed from partitions.csv) avoids that entirely and
    # never touches the bootloader or partition table (already written by
    # the factory stage's flash step).
    log "Flashing app image to ota_0 (offset $ota0_offset) on $port ..."
    esptool.py --chip "$IDF_TARGET" -p "$port" -b "$FLASH_BAUD" write_flash "$ota0_offset" "$app_bin"

    if [[ -f "$webapp_bin" ]]; then
        log "Flashing web UI image to webapp partition (offset $webapp_offset) on $port ..."
        esptool.py --chip "$IDF_TARGET" -p "$port" -b "$FLASH_BAUD" write_flash "$webapp_offset" "$webapp_bin"
    else
        warn "No webapp.bin found (SPIFFS image); skipping. This should have been generated during the build."
    fi
}

cmd_flash() {
    local stage="${1:-all}"
    ensure_idf_env
    local port; port="$(find_esp32_port)"
    log "Using serial port: $port"

    case "$stage" in
        factory) flash_factory "$port" ;;
        app)     flash_app "$port" ;;
        all)     flash_factory "$port"; flash_app "$port" ;;
        full)
            # Full factory-fresh reflash: wipe every byte of flash first --
            # including NVS (WiFi credentials/settings), otadata (boot slot +
            # rollback state), the ota_1 backup slot, and the webapp
            # partition -- then write factory + app back. The result is
            # indistinguishable from a brand-new board running this firmware.
            warn "FULL flash: erasing the ENTIRE chip (all settings/WiFi credentials will be lost)..."
            esptool.py --chip "$IDF_TARGET" -p "$port" -b "$FLASH_BAUD" erase_flash
            flash_factory "$port"
            flash_app "$port"
            ;;
        *) die "Unknown stage '$stage' (expected factory|app|all|full)" ;;
    esac

    log "Flash complete. Reset the board (or power-cycle) to boot into the new firmware."
}

cmd_install() {
    local stage="${1:-all}"
    if [[ "$stage" == "all" || "$stage" == "app" || "$stage" == "full" ]]; then
        if [[ ! -f "$WEB_DIR/dist/index.html" ]]; then
            cmd_web
        else
            log "web/dist already built (skipping; run './build.sh --web' to rebuild it)."
        fi
    fi
    # 'full' only affects how much is ERASED at flash time -- the build
    # artifacts themselves are identical to 'all'.
    local build_stage="$stage"
    if [[ "$build_stage" == "full" ]]; then
        build_stage="all"
    fi
    cmd_build "$build_stage"
    cmd_flash "$stage"
}

cmd_monitor() {
    local stage="${1:?Usage: ./build.sh --monitor <factory|app>}"
    ensure_idf_env

    # Unlike --flash/--build (which act on exactly one board), the monitor
    # watches EVERY connected board by default (target '0' inside it), each
    # log line tagged with which device produced it. Pass one --port per
    # detected device; --port /dev/ttyXXX at the build.sh level still works
    # to restrict this to a single device if you only want to watch one.
    local ports=()
    while IFS= read -r p; do
        ports+=(--port "$p")
    done < <(find_all_esp32_ports)

    # NOTE: we deliberately don't use `idf.py monitor` here. It resets the
    # board on connect (via RTS/DTR) to guarantee it captures the full boot
    # log -- but on this board's native USB-Serial/JTAG port, that same
    # reset sequence leaves the chip sitting in bootloader/download mode
    # instead of running the app. Our own tools/dev_monitor.py never touches
    # RTS/DTR, so it just watches whatever is already running, and adds
    # Expo-Go-style "press r to rebuild+reflash+resume" / device-targeting
    # shortcuts (press 'h' inside it for the full key reference).
    python3 "$SCRIPT_DIR/tools/dev_monitor.py" "${ports[@]}" --stage "$stage" --variant "$VARIANT" --repo-root "$SCRIPT_DIR"
}

# Just connects and watches live serial output from every connected board --
# no build or flash on startup. Use the monitor's own keys to do that on
# demand: '0'/'1'-'9' pick which device(s) are targeted, 'r' rebuilds +
# reflashes the default stage, 'f' the factory/recovery stage, 'a' all
# stages, 'b' compile-checks, 'w' rebuilds the web UI, 'h' shows a full
# reference (including which /dev/ttyACM... is which device number). This is
# the typical "leave it running and iterate" loop: start it once, then
# press 'r' whenever you want to try new code on one or all boards, instead
# of waiting through a build+flash every time you just want to glance at logs.
# Defaults to the 'app' stage since that's what you iterate on day-to-day;
# 'factory' is rarely touched once it's on the board.
cmd_run() {
    local stage="${1:-app}"
    if [[ "$stage" == "all" ]]; then
        stage="app" # monitor the app stage by default (most relevant logs)
    fi
    cmd_monitor "$stage"
}

cmd_menuconfig() {
    local stage="${1:?Usage: ./build.sh --menuconfig <factory|app>}"
    ensure_idf_env
    idf.py -C "$(stage_dir "$stage")" menuconfig
}

cmd_clean() {
    local stage="${1:-all}"
    case "$stage" in
        factory) rm -rf "$(stage_dir factory)/build" "$(stage_dir factory)/sdkconfig" ;;
        app)     rm -rf "$(stage_dir app)/build" "$(stage_dir app)/sdkconfig" ;;
        all)     rm -rf "$(stage_dir factory)/build" "$(stage_dir factory)/sdkconfig" "$(stage_dir app)/build" "$(stage_dir app)/sdkconfig" ;;
        *) die "Unknown stage '$stage' (expected factory|app|all)" ;;
    esac
    log "Cleaned build output for: $stage"
}

# --------------------------------------------------------------------------
# Board variant tracking
# --------------------------------------------------------------------------
# sdkconfig is only regenerated from sdkconfig.defaults* when it doesn't
# already exist (or on `idf.py fullclean`/reconfigure) -- so switching
# --variant on a stage that was already built with a *different* variant
# needs its stale build/ and sdkconfig wiped first, or the old variant's
# settings would silently stick around. This tracks the last variant each
# stage was built with (in a marker file that -- unlike build/ -- survives
# `--clean`) and auto-cleans exactly when it changes.
variant_marker_file() { echo "$(stage_dir "$1")/.build_variant"; }

ensure_variant_consistency() {
    local stage="$1"
    local marker; marker="$(variant_marker_file "$stage")"
    if [[ -f "$marker" ]]; then
        local previous; previous="$(cat "$marker")"
        if [[ "$previous" != "$VARIANT" ]]; then
            warn "Stage '$stage' was last built for variant '$previous'; switching to '$VARIANT' -- cleaning stale build output and sdkconfig first."
            rm -rf "$(stage_dir "$stage")/build" "$(stage_dir "$stage")/sdkconfig"
        fi
    fi
    echo "$VARIANT" > "$marker"
}

# --------------------------------------------------------------------------
# Flash memory inspection
# --------------------------------------------------------------------------
# Prints the full flash layout (fixed bootloader/partition-table regions +
# every partitions.csv row + any unmapped gaps) without touching a board.
cmd_mem_map() {
    python3 - "$PARTITIONS_CSV" "$SCRIPT_DIR/tools" <<'PYEOF'
import sys
sys.path.insert(0, sys.argv[2])
from flash_inspector import load_regions, CHIP_SIZE

regions = load_regions(sys.argv[1])
print(f"{'name':<18} {'start':>10} {'end':>10} {'size':>10}")
prev_end = 0
for r in regions:
    if r["offset"] > prev_end:
        print(f"{'(unmapped)':<18} {prev_end:>#10x} {r['offset']:>#10x} {r['offset'] - prev_end:>#10x}")
    end = r["offset"] + r["size"]
    print(f"{r['name']:<18} {r['offset']:>#10x} {end:>#10x} {r['size']:>#10x}")
    prev_end = end
if prev_end < CHIP_SIZE:
    print(f"{'(unmapped)':<18} {prev_end:>#10x} {CHIP_SIZE:>#10x} {CHIP_SIZE - prev_end:>#10x}")
PYEOF
}

# Interactive hexdump-style browser over the chip's actual flash contents
# (reads over the serial bootloader; see tools/flash_inspector.py).
cmd_mem() {
    local start="${1:-0x0}"
    ensure_idf_env
    local port; port="$(find_esp32_port)"
    warn "Each uncached read resets the board briefly (download mode -> read -> hard reset)."
    warn "Quit any running monitor first -- the inspector needs exclusive port access."
    python3 "$SCRIPT_DIR/tools/flash_inspector.py" \
        --port "$port" --baud "$FLASH_BAUD" \
        --partitions-csv "$PARTITIONS_CSV" --start "$start"
}

# --------------------------------------------------------------------------
# Argument parsing
# --------------------------------------------------------------------------
ACTION=""
STAGE_ARG=""

# First pass: pull out --port/--variant anywhere in the args.
args=("$@")
filtered=()
i=0
while [[ $i -lt ${#args[@]} ]]; do
    if [[ "${args[$i]}" == "--port" ]]; then
        i=$((i+1))
        PORT_OVERRIDE="${args[$i]:-}"
    elif [[ "${args[$i]}" == "--usbdevs" ]]; then
        USB_DEVS=1
    elif [[ "${args[$i]}" == "--variant" ]]; then
        i=$((i+1))
        VARIANT="${args[$i]:-}"
        case "$VARIANT" in
            7|7b) ;;
            *) die "Unknown --variant '$VARIANT' (expected 7 or 7b)" ;;
        esac
    else
        filtered+=("${args[$i]}")
    fi
    i=$((i+1))
done
set -- "${filtered[@]+"${filtered[@]}"}"
export TOUCH_ESP32_VARIANT="$VARIANT"

# Sets STAGE_ARG from the next positional arg if it's present and doesn't
# look like another flag (e.g. `--build app` vs just `--build`), then shifts
# past it. Falls back to $1 (the provided default) otherwise.
consume_stage_arg() {
    local default_value="$1"
    if [[ $# -ge 2 && -n "$2" && "${2:0:2}" != "--" ]]; then
        STAGE_ARG="$2"
        shift_extra=1
    else
        STAGE_ARG="$default_value"
        shift_extra=0
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --setup) ACTION="setup"; shift ;;
        --web) ACTION="web"; shift ;;
        --build) ACTION="build"; shift; consume_stage_arg "all" "${1:-}"; if [[ $shift_extra -eq 1 ]]; then shift; fi ;;
        --flash) ACTION="flash"; shift; consume_stage_arg "all" "${1:-}"; if [[ $shift_extra -eq 1 ]]; then shift; fi ;;
        --install) ACTION="install"; shift; consume_stage_arg "all" "${1:-}"; if [[ $shift_extra -eq 1 ]]; then shift; fi ;;
        --run) ACTION="run"; shift; consume_stage_arg "app" "${1:-}"; if [[ $shift_extra -eq 1 ]]; then shift; fi ;;
        --monitor) ACTION="monitor"; shift; consume_stage_arg "" "${1:-}"; if [[ $shift_extra -eq 1 ]]; then shift; fi ;;
        --menuconfig) ACTION="menuconfig"; shift; consume_stage_arg "" "${1:-}"; if [[ $shift_extra -eq 1 ]]; then shift; fi ;;
        --clean) ACTION="clean"; shift; consume_stage_arg "all" "${1:-}"; if [[ $shift_extra -eq 1 ]]; then shift; fi ;;
        --mem) ACTION="mem"; shift; consume_stage_arg "0x0" "${1:-}"; if [[ $shift_extra -eq 1 ]]; then shift; fi ;;
        --mem-map) ACTION="mem_map"; shift ;;
        --help|-h) usage; exit 0 ;;
        "") shift ;;
        *) die "Unknown argument: $1 (see --help)" ;;
    esac
done

case "$ACTION" in
    setup) cmd_setup ;;
    web) cmd_web ;;
    build) cmd_build "$STAGE_ARG" ;;
    flash) ensure_dialout_group; cmd_flash "$STAGE_ARG" ;;
    install) ensure_dialout_group; cmd_install "$STAGE_ARG" ;;
    run) ensure_dialout_group; cmd_run "$STAGE_ARG" ;;
    monitor) ensure_dialout_group; cmd_monitor "$STAGE_ARG" ;;
    menuconfig) cmd_menuconfig "$STAGE_ARG" ;;
    clean) cmd_clean "$STAGE_ARG" ;;
    mem) ensure_dialout_group; cmd_mem "$STAGE_ARG" ;;
    mem_map) cmd_mem_map ;;
    "") usage; exit 1 ;;
esac


