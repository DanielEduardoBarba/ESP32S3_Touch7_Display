#pragma once

#include <functional>
#include <string>
#include <vector>
#include <cstdint>

/**
 * Event-driven WiFi station manager built on native esp_wifi + esp_event.
 * Nothing here ever blocks: scans and connection attempts are kicked off
 * asynchronously, and results are delivered later via the callbacks below
 * (invoked from the default esp_event task). This is what lets the LVGL UI,
 * the web server, and RS485 all keep running without stalling on WiFi I/O.
 */
namespace wifi_manager {

struct ApInfo {
    std::string ssid;
    int8_t rssi = 0;
    bool secure = false;
};

enum class State {
    Disconnected,
    Connecting,
    Connected,
    Failed,
};

using ScanResultCallback = std::function<void(const std::vector<ApInfo> &)>;
using StateChangeCallback = std::function<void(State state, const std::string &ssid, const std::string &ip)>;

/** Bring up esp_netif/esp_event/esp_wifi in STA mode. Call once at startup. */
void init();

/** Attempt to reconnect using the last-saved credentials in NVS, if any. */
void autoConnect();

/** Kick off an async WiFi scan. Safe to call again while a scan is pending
 *  (it will simply be ignored until the current one finishes). */
void startScan();

/** Kick off an async connection attempt with the given SSID/password. Saves
 *  the credentials to NVS on success so autoConnect() can use them later. */
void connect(const std::string &ssid, const std::string &password);

State state();
std::string currentSsid();
std::string ipAddress();
int8_t rssi();

/** Registers an additional scan-results subscriber (does not replace any
 *  previously-registered ones). */
void onScanResults(ScanResultCallback cb);

/** Registers an additional state-change subscriber (does not replace any
 *  previously-registered ones). */
void onStateChange(StateChangeCallback cb);

} // namespace wifi_manager
