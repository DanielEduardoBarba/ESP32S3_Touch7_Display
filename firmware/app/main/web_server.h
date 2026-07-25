#pragma once

/**
 * On-device HTTP server (native esp_http_server, fully async/event-driven --
 * nothing here blocks the LVGL, WiFi, or RS485 tasks). Serves the built
 * React UI (see ../../web) from whichever storage backend `storage::init()`
 * picked, on port 80, plus a small JSON API consumed by that UI:
 *
 *   GET  /api/status        -> device/WiFi/storage status
 *   GET  /api/wifi/scan     -> triggers + returns the latest WiFi scan
 *   POST /api/wifi/connect  -> {"ssid": "...", "password": "..."}
 *   POST /api/ota           -> raw firmware binary, flashed to the inactive
 *                              ota_0/ota_1 slot (see ota_handler.h)
 *
 * Also a WebSocket endpoint that makes the web UI a live extension of the
 * touchscreen (see machine_state.h for the full explanation):
 *
 *   WS /ws  -> server pushes the whole Machine state
 *                {"dial_value": N, "speed": N, "setpoint": N, "mode": N,
 *                 "toggles": [bool, ...], "pulse_count": N}
 *              whenever it changes (from the local touchscreen, RS485, or
 *              another browser), and accepts
 *                {"type": "dial", "value": N}
 *                {"type": "speed", "value": N}
 *                {"type": "setpoint", "value": N}
 *                {"type": "mode", "value": N}
 *                {"type": "toggle", "id": N, "state": bool}
 *                {"type": "pulse"}
 *              which are applied exactly as if done on the display itself.
 */
namespace web_server {

void start();

} // namespace web_server
