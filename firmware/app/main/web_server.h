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
 */
namespace web_server {

void start();

} // namespace web_server
