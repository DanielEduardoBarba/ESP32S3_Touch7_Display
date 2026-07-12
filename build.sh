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
#                                    connected board. stage: factory|app|all
#                                    (default: all).
#   ./build.sh --install [stage]    web + build + flash in one go (default:
#                                    all). This is what you want for a brand
#                                    new board.
#   ./build.sh --run [stage]        Build + flash + immediately watch live
#                                    serial output, all in one command
#                                    (stage: factory|app, default: app).
#                                    While watching: press 'r' to rebuild +
#                                    reflash + resume (Expo-Go style manual
#                                    reload), Ctrl+C to quit.
#   ./build.sh --monitor [stage]    Just watch live serial output (same 'r'
#                                    reload shortcut as --run).
#   ./build.sh --menuconfig <stage> Open idf.py menuconfig for one stage.
#   ./build.sh --clean [stage]      Remove build/ dirs.
#   ./build.sh --port /dev/ttyXXX   Override auto-detected serial port for
#                                    any of the above (can appear anywhere).
#   ./build.sh --help
#
# Notes:
#   - If your shell session was opened before your user was added to the
#     'dialout' group (e.g. right after the first `--setup` run), any
#     command that needs the serial port transparently re-runs itself via
#     `sg dialout` -- you don't need to log out/in first.
#   - If you have more than one ESP32/serial device plugged in, pass
#     `--port /dev/ttyXXX` to pick a specific one; otherwise the first
#     /dev/ttyUSB*|/dev/ttyACM* found is used.
#
# Examples:
#   ./build.sh --setup
#   ./build.sh --install
#   ./build.sh --run app            # iterate: build, flash, watch logs
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

usage() { sed -n '2,43p' "$0" | sed 's/^# \{0,1\}//'; }

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
    candidates+=(/dev/ttyUSB* /dev/ttyACM*)
    shopt -u nullglob

    if [[ ${#candidates[@]} -eq 0 ]]; then
        die "No /dev/ttyUSB* or /dev/ttyACM* device found. Plug in the board (USB port labeled UART), and make sure your user is in the 'dialout' group (./build.sh --setup)."
    elif [[ ${#candidates[@]} -eq 1 ]]; then
        echo "${candidates[0]}"
    else
        warn "Multiple serial ports found: ${candidates[*]}" >&2
        warn "Using the first one (${candidates[0]}). Override with --port /dev/ttyXXX." >&2
        echo "${candidates[0]}"
    fi
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
        factory) idf.py -C "$(stage_dir factory)" build ;;
        app)     idf.py -C "$(stage_dir app)" build ;;
        all)     cmd_build factory; cmd_build app ;;
        *) die "Unknown stage '$stage' (expected factory|app|all)" ;;
    esac
}

flash_factory() {
    local port="$1"
    log "Flashing 'factory' stage (bootloader + partition table + factory app) to $port ..."
    idf.py -C "$(stage_dir factory)" -p "$port" -b "$FLASH_BAUD" flash
}

flash_app() {
    local port="$1"
    ensure_idf_env
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
        *) die "Unknown stage '$stage' (expected factory|app|all)" ;;
    esac

    log "Flash complete. Reset the board (or power-cycle) to boot into the new firmware."
}

cmd_install() {
    local stage="${1:-all}"
    if [[ "$stage" == "all" || "$stage" == "app" ]]; then
        if [[ ! -f "$WEB_DIR/dist/index.html" ]]; then
            cmd_web
        else
            log "web/dist already built (skipping; run './build.sh --web' to rebuild it)."
        fi
    fi
    cmd_build "$stage"
    cmd_flash "$stage"
}

cmd_monitor() {
    local stage="${1:?Usage: ./build.sh --monitor <factory|app>}"
    ensure_idf_env
    local port; port="$(find_esp32_port)"
    # NOTE: we deliberately don't use `idf.py monitor` here. It resets the
    # board on connect (via RTS/DTR) to guarantee it captures the full boot
    # log -- but on this board's native USB-Serial/JTAG port, that same
    # reset sequence leaves the chip sitting in bootloader/download mode
    # instead of running the app. Our own tools/dev_monitor.py never touches
    # RTS/DTR, so it just watches whatever is already running, and adds an
    # Expo-Go-style "press r to rebuild+reflash+resume" shortcut.
    python3 "$SCRIPT_DIR/tools/dev_monitor.py" --port "$port" --stage "$stage" --repo-root "$SCRIPT_DIR"
}

# Build + flash + immediately watch live serial output, in one command --
# the typical "change code, see what happens on real hardware" loop.
# Defaults to the 'app' stage since that's what you iterate on day-to-day;
# 'factory' is rarely touched once it's on the board.
cmd_run() {
    local stage="${1:-app}"
    if [[ "$stage" == "all" ]]; then
        cmd_build all
        cmd_flash all
        stage="app" # monitor the app stage afterwards (most relevant logs)
    else
        cmd_build "$stage"
        cmd_flash "$stage"
    fi
    log "Flashed. Watching live serial output for '$stage' (press 'r' to rebuild+reflash+resume, Ctrl+C to quit)..."
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
        factory) rm -rf "$(stage_dir factory)/build" ;;
        app)     rm -rf "$(stage_dir app)/build" ;;
        all)     rm -rf "$(stage_dir factory)/build" "$(stage_dir app)/build" ;;
        *) die "Unknown stage '$stage' (expected factory|app|all)" ;;
    esac
    log "Cleaned build output for: $stage"
}

# --------------------------------------------------------------------------
# Argument parsing
# --------------------------------------------------------------------------
ACTION=""
STAGE_ARG=""

# First pass: pull out --port anywhere in the args.
args=("$@")
filtered=()
i=0
while [[ $i -lt ${#args[@]} ]]; do
    if [[ "${args[$i]}" == "--port" ]]; then
        i=$((i+1))
        PORT_OVERRIDE="${args[$i]:-}"
    else
        filtered+=("${args[$i]}")
    fi
    i=$((i+1))
done
set -- "${filtered[@]+"${filtered[@]}"}"

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
    "") usage; exit 1 ;;
esac


