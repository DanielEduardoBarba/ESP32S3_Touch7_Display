# touch-esp32

Boilerplate firmware + on-device web UI for the **Waveshare ESP32-S3-Touch-LCD-7**
(800×480 RGB LCD, GT911 5-point capacitive touch, WiFi/BLE, RS485, TF/SD slot).

Built with **native ESP-IDF** (no Arduino/PlatformIO) + **LVGL** for the local
touchscreen UI, and a **Vite + React + Tailwind CSS** app served by the device
itself on port 80.

## Architecture

The flash is split into a small, rarely-changed **factory** stage and a
swappable **app** stage, so the app can be updated in the field (e.g. from its
own web UI) without ever touching the factory stage or needing this dev
toolchain again:

```
16MB flash
├─ nvs        (Wi-Fi credentials, small KV data)
├─ otadata    (which app slot to boot: factory vs ota_0/ota_1)
├─ factory    ── firmware/factory ── splash screen, "Continue" placeholder UI.
│                On first boot (or after a factory-reset), shows this screen,
│                then points the boot selector at ota_0 and reboots.
├─ ota_0      ─┐
├─ ota_1      ─┴ firmware/app ── "the brains": LVGL UI, WiFi manager, RS485,
│                and the web server. A/B OTA pair so a future firmware update
│                (POST /api/ota) can write the *inactive* slot and reboot into
│                it, independent of this repo's toolchain.
└─ webapp     (SPIFFS) the built React UI (web/dist), served on port 80.
               Can also be served from an SD card instead (see storage.cpp).
```

See [firmware/partitions.csv](firmware/partitions.csv) for exact offsets/sizes.

Everything in the `app` stage is event-driven / runs as its own FreeRTOS task,
so the LVGL UI, WiFi, RS485, and the web server never block each other:

- LVGL runs in its own task (`lvgl_v8_port.cpp`), driven by an esp_timer tick.
- WiFi is 100% callback-based via `esp_event` (`wifi_manager.cpp`) -- scans and
  connects are fire-and-forget, results arrive later via callbacks.
- RS485 has its own reader task (`rs485.cpp`).
- The web server (`esp_http_server`) is async under the hood and runs in its
  own task once started (`web_server.cpp`).

### Local touchscreen UI

- Header bar: hamburger menu (dropdown, currently just "Home") + Home icon on
  the left; network icon (WiFi dropdown → connect modal) + info icon
  (placeholder, no-op) on the right.
- Tapping any text field (SSID/password) pops up a full-width on-screen
  keyboard (LVGL's built-in `lv_keyboard`, with shift/caps-lock and a
  special-characters mode already built in).
- See `firmware/app/main/ui/`.

### Web UI (web/)

A small React dashboard mirroring the same functionality (device status +
WiFi scan/connect), served directly by the device. See [web/README](web/README.md)-ish
usage below.

## Connecting the board

This board has multiple USB-C/UART connectors. **Use the one labeled `UART1`**,
not the plain `USB` port next to it:

- `UART1` goes through a CH343 USB-UART bridge chip and has reliable classic
  auto-reset -- flashing, resetting, and the serial console all just work.
- The plain `USB` port is the ESP32-S3's *native* USB-Serial/JTAG peripheral.
  Its auto-reset is unreliable on this board (it can leave the chip stuck in
  bootloader/download mode after flashing) -- this matches Waveshare's own
  docs, which say to manually press the RESET button after flashing when
  using it. Avoid it unless `UART1` isn't available.

## Quick start (fresh Ubuntu machine)

```bash
git clone <this repo>
cd touch-esp32
./build.sh --setup     # apt packages, serial port permissions, ESP-IDF, Node.js
# start a NEW shell (group membership needs a fresh login), then:
./build.sh --install   # builds the React UI + both firmware stages, flashes a connected board
```

`--setup` is safe to re-run. It:
- installs ESP-IDF's Linux build prerequisites via `apt`,
- adds your user to the `dialout` group (serial port access) and installs a
  udev rule so ModemManager leaves the board's CH34x USB-serial port alone,
- clones & installs ESP-IDF (`esp32s3` target only) into `~/esp/esp-idf`,
  and adds a `get_idf` shell alias for future sessions,
- installs Node.js (>= 18) if missing/too old, needed to build the React UI.

`--install` auto-detects the board's serial port (`/dev/ttyUSB*`/`/dev/ttyACM*`);
pass `--port /dev/ttyXXX` explicitly if you have more than one device plugged in
(e.g. both the `UART1` and native `USB` ports connected at once).

See `./build.sh --help` for the full command list (`--build`, `--flash`,
`--web`, `--monitor`, `--menuconfig`, `--clean`, each taking an optional
`factory|app|all` stage argument).

### Day-to-day iteration: `./build.sh --run app`

Builds + flashes + immediately starts watching live serial output. While
watching, press **`r`** at any time to rebuild + reflash + reset the board
and resume watching -- no need to re-run the command (Expo Go-style manual
reload; there's no file-watching, it's a deliberate keypress). Ctrl+C to quit.
This also works with just `./build.sh --monitor app` if you don't need to
build first.

## Manual (without build.sh)

```bash
source ~/esp/esp-idf/export.sh
cd web && npm ci && npm run build && cd ..   # produces web/dist, packed into the webapp partition

idf.py -C firmware/factory -p /dev/ttyACM0 flash   # bootloader + partition table + factory splash
idf.py -C firmware/app build
# app.bin/webapp.bin must go to the ota_0/webapp offsets specifically (NOT via
# `idf.py flash`, which would default-target the `factory` partition since it
# comes first in partitions.csv) -- see build.sh's flash_app() for the exact
# esptool.py write_flash commands, or just use ./build.sh --flash.
#
# Also avoid `idf.py monitor` on this board -- its reset-on-connect can land
# the chip in bootloader/download mode. Use `./build.sh --monitor app`
# (tools/dev_monitor.py), which never touches RTS/DTR.
```

## First boot

1. Board powers on into the `factory` splash screen.
2. Tap **Continue** → device reboots into the `app` stage (`ota_0`).
3. Use the network icon (top-right) to connect to WiFi.
4. Once connected, visit `http://<device-ip>/` in a browser for the web UI.

## Known TODOs / things to verify against your specific board revision

- **RS485 pins** (`firmware/app/main/rs485.cpp`) and **SD card SPI pins**
  (`firmware/app/main/storage.cpp`, disabled by default via
  `STORAGE_SD_ENABLED`) are placeholders -- verify against the
  [schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-7/ESP32-S3-Touch-LCD-7-Sch.pdf)
  for your unit before relying on them. The RGB LCD, GT911 touch, and CH422G
  IO-expander wiring are handled automatically by ESP32_Display_Panel's
  built-in `BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7` support and don't need any
  of this.
- `POST /api/ota` (see `firmware/app/main/ota_handler.cpp`) accepts a raw
  firmware binary and flashes it to the inactive OTA slot -- there's no auth
  on it yet; add some before exposing this to an untrusted network.
- 80MHz flash/PSRAM is used for reliability; Waveshare's wiki documents a
  120MHz-with-experimental-features tuning for higher LVGL frame rates if
  you want to opt into it (`firmware/common/sdkconfig.defaults`).
- The console (log output) is routed to classic UART0 (`CONFIG_ESP_CONSOLE_UART_DEFAULT`),
  which is what the `UART1` port's CH343 bridge exposes -- and also what the
  2nd-stage bootloader always logs to regardless of app config. If you ever
  switch to the native `USB` port, you won't see any app logs (only the
  bootloader's) unless you change this back to `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`.
# ESP32S3_Touch7_Display
