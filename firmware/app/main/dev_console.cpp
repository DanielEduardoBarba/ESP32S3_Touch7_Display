#include "dev_console.h"

#include "app_config.h"

#if APP_DEV_MODE

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_manager.h"
#include "display_settings.h"
#include "fw_update.h"
#include "machine_state.h"
#include "ports.h"
#include "wifi_manager.h"
#include "wifi_sync.h"

namespace dev_console {
namespace {

const char *TAG = "dev_console";

// The two ways a host can be attached to these boards:
//   - UART0, wired to the on-board CH343 USB-serial bridge (the connector
//     labeled UART, VID 1a86). This is the one build.sh flashes and
//     tools/dev_monitor.py opens, and it carries the ESP-IDF console.
//   - The ESP32-S3's native USB-Serial-JTAG (the connector labeled USB,
//     VID 303a).
// Commands are accepted on BOTH, because which one is plugged in is a
// property of the cable, not of the firmware. Listening only on the native
// USB port made every command sent over the CH343 cable disappear silently
// (logs still came out, since those go through the console's own TX path).
constexpr uart_port_t CONSOLE_UART = UART_NUM_0;

void handleLine(char *line)
{
    // Trim trailing CR/LF and leading spaces.
    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    while (*line == ' ') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    if (std::strcmp(line, "sync") == 0) {
        ESP_LOGI(TAG, "cmd: sync");
        fw_update::syncPeer();
    } else if (std::strcmp(line, "push") == 0) {
        ESP_LOGI(TAG, "cmd: push -> %s", fw_update::startSend() ? "started" : "REFUSED");
    } else if (std::strcmp(line, "pull") == 0) {
        ESP_LOGI(TAG, "cmd: pull -> %s", fw_update::startPull() ? "requested" : "REFUSED");
    } else if (std::strncmp(line, "baudlocal ", 10) == 0) {
        uint32_t rate = std::strtoul(line + 10, nullptr, 10);
        ESP_LOGI(TAG, "cmd: baudlocal %lu -> %s", (unsigned long)rate,
                 ports::setBaud(rate, /*announce=*/false) ? "ok" : "REFUSED");
    } else if (std::strncmp(line, "baud ", 5) == 0) {
        uint32_t rate = std::strtoul(line + 5, nullptr, 10);
        ESP_LOGI(TAG, "cmd: baud %lu -> %s", (unsigned long)rate,
                 ports::setBaud(rate) ? "ok" : "REFUSED");
    } else if (std::strcmp(line, "update-reset") == 0) {
        fw_update::resetForTesting();
        ESP_LOGI(TAG, "cmd: update-reset -> done");
    } else if (std::strcmp(line, "status") == 0) {
        fw_update::Status st = fw_update::status();
        ESP_LOGI(TAG, "status: transport=%s baud=%lu transfer_state=%d done=%lu/%lu",
                 ports::name(ports::active()), (unsigned long)ports::baud(),
                 (int)st.state, (unsigned long)st.done_bytes, (unsigned long)st.total_bytes);
    // --- Machine scene controls -------------------------------------------
    // Driven as Source::Web, i.e. exactly like a web command: the local
    // widgets move AND the change goes out to the peer board. Handy for
    // exercising the peer link without touching the screen.
    } else if (std::strncmp(line, "dial ", 5) == 0) {
        long v = std::strtol(line + 5, nullptr, 10);
        machine_state::setDial(static_cast<uint8_t>(v < 0 ? 0 : v), machine_state::Source::Web);
        ESP_LOGI(TAG, "cmd: dial %ld", v);
    } else if (std::strncmp(line, "speed ", 6) == 0) {
        long v = std::strtol(line + 6, nullptr, 10);
        machine_state::setSpeed(static_cast<uint16_t>(v < 0 ? 0 : v), machine_state::Source::Web);
        ESP_LOGI(TAG, "cmd: speed %ld", v);
    } else if (std::strncmp(line, "setpoint ", 9) == 0) {
        long v = std::strtol(line + 9, nullptr, 10);
        machine_state::setSetpoint(static_cast<int16_t>(v), machine_state::Source::Web);
        ESP_LOGI(TAG, "cmd: setpoint %ld", v);
    } else if (std::strncmp(line, "mode ", 5) == 0) {
        long v = std::strtol(line + 5, nullptr, 10);
        machine_state::setMode(static_cast<uint8_t>(v), machine_state::Source::Web);
        ESP_LOGI(TAG, "cmd: mode %ld", v);
    } else if (std::strncmp(line, "toggle ", 7) == 0) {
        int id = 0, on = 0;
        if (std::sscanf(line + 7, "%d %d", &id, &on) == 2) {
            machine_state::setToggle(static_cast<uint8_t>(id), on != 0, machine_state::Source::Web);
            ESP_LOGI(TAG, "cmd: toggle %d %d", id, on);
        } else {
            ESP_LOGW(TAG, "usage: toggle <id 0-%d> <0|1>", machine_state::TOGGLE_COUNT - 1);
        }
    } else if (std::strcmp(line, "pulse") == 0) {
        machine_state::firePulse(machine_state::Source::Web);
        ESP_LOGI(TAG, "cmd: pulse");
    } else if (std::strcmp(line, "mstate") == 0) {
        machine_state::State m = machine_state::current();
        ESP_LOGI(TAG, "mstate: dial=%u speed=%u setpoint=%d mode=%s toggles=%d%d%d%d pulses=%lu",
                 (unsigned)m.dial_value, (unsigned)m.speed, (int)m.setpoint,
                 machine_state::modeName(m.mode), m.toggles[0], m.toggles[1], m.toggles[2],
                 m.toggles[3], (unsigned long)m.pulse_count);
    // --- Local display + radios ------------------------------------------
    } else if (std::strncmp(line, "bright ", 7) == 0) {
        long v = std::strtol(line + 7, nullptr, 10);
        display_settings::setBrightness(static_cast<uint8_t>(v < 0 ? 0 : v));
        ESP_LOGI(TAG, "cmd: brightness -> %u%%", (unsigned)display_settings::brightness());
    } else if (std::strcmp(line, "wifi") == 0) {
        ESP_LOGI(TAG, "wifi: state=%d ssid='%s' ip=%s saved='%s'", (int)wifi_manager::state(),
                 wifi_manager::currentSsid().c_str(), wifi_manager::ipAddress().c_str(),
                 wifi_manager::savedSsid().c_str());
    } else if (std::strcmp(line, "wifi share") == 0) {
        wifi_sync::shareNow();
        ESP_LOGI(TAG, "cmd: wifi share -> sent");
    } else if (std::strcmp(line, "wifi forget") == 0) {
        wifi_manager::forget();
        ESP_LOGI(TAG, "cmd: wifi forget -> saved credentials erased");
    } else if (std::strncmp(line, "wifi connect ", 13) == 0) {
        // "wifi connect <ssid> [password]" -- password may be omitted for an
        // open network. SSIDs with spaces need the UI or the web API.
        char *ssid = line + 13;
        char *pass = std::strchr(ssid, ' ');
        if (pass != nullptr) {
            *pass = '\0';
            pass++;
        }
        ESP_LOGI(TAG, "cmd: wifi connect '%s'", ssid);
        wifi_manager::connect(ssid, pass != nullptr ? pass : "");
    } else if (std::strcmp(line, "bt on") == 0 || std::strcmp(line, "bt off") == 0) {
        bool on = line[3] == 'o' && line[4] == 'n';
        ble_manager::setEnabled(on);
        ESP_LOGI(TAG, "cmd: bluetooth %s", on ? "on" : "off");
    } else if (std::strcmp(line, "bt scan") == 0) {
        ble_manager::startScan(APP_BLE_SCAN_SECONDS);
        ESP_LOGI(TAG, "cmd: bt scan (%d s)", APP_BLE_SCAN_SECONDS);
    } else if (std::strcmp(line, "bt discover on") == 0 ||
               std::strcmp(line, "bt discover off") == 0) {
        bool on = std::strcmp(line, "bt discover on") == 0;
        ble_manager::setDiscoverable(on);
        ESP_LOGI(TAG, "cmd: bt discoverable %s", on ? "on" : "off");
    } else if (std::strncmp(line, "bt connect ", 11) == 0) {
        ble_manager::connect(line + 11);
        ESP_LOGI(TAG, "cmd: bt connect %s", line + 11);
    } else if (std::strcmp(line, "bt pair") == 0) {
        ble_manager::confirmPairing(true);
    } else if (std::strcmp(line, "bt disconnect") == 0) {
        ble_manager::disconnect();
    } else if (std::strcmp(line, "bt") == 0) {
        ble_manager::DeviceInfo dev = ble_manager::connectedDevice();
        ESP_LOGI(TAG, "bt: enabled=%d state=%d discoverable=%d name='%s' peer='%s' bonded=%d",
                 (int)ble_manager::enabled(), (int)ble_manager::state(),
                 (int)ble_manager::discoverable(), ble_manager::deviceName().c_str(),
                 dev.addr.c_str(), (int)dev.bonded);
    } else {
        ESP_LOGW(TAG, "unknown command: '%s'", line);
        ESP_LOGW(TAG, "  peer:    sync|push|pull|baud N|baudlocal N|update-reset|status");
        ESP_LOGW(TAG, "  machine: dial N|speed N|setpoint N|mode N|toggle ID 0|1|pulse|mstate");
        ESP_LOGW(TAG, "  local:   bright N|wifi|wifi share|bt|bt on|bt off|bt scan|");
        ESP_LOGW(TAG, "           bt discover on|off|bt connect AA:BB:CC:DD:EE:FF|bt pair|bt disconnect");
    }
}

void consoleTask(void *)
{
    char line[96];
    size_t pos = 0;
    uint8_t ch;
    while (true) {
        // Byte-at-a-time via the driver (installed in init); blocks cheaply.
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(500));
        if (n <= 0) {
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                handleLine(line);
                pos = 0;
            }
        } else if (pos < sizeof(line) - 1) {
            line[pos++] = (char)ch;
        } else {
            pos = 0; // overlong line: drop and resync
        }
    }
}

/** Same line assembly for the UART0/CH343 side. */
void uartConsoleTask(void *)
{
    char line[96];
    size_t pos = 0;
    uint8_t ch;
    while (true) {
        int n = uart_read_bytes(CONSOLE_UART, &ch, 1, pdMS_TO_TICKS(500));
        if (n <= 0) {
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                handleLine(line);
                pos = 0;
            }
        } else if (pos < sizeof(line) - 1) {
            line[pos++] = (char)ch;
        } else {
            pos = 0; // overlong line: drop and resync
        }
    }
}

} // namespace

void init()
{
    int listeners = 0;

    // --- UART0 / CH343 bridge (the cable used for flashing + monitoring) --
    // RX-only: tx_buffer_size 0 leaves console logging on its existing
    // (polling, never-blocking) path instead of routing it through a driver
    // buffer that can block when the host lags.
    esp_err_t err = uart_driver_install(CONSOLE_UART, 256, 0, 0, nullptr, 0);
    if (err == ESP_OK) {
        xTaskCreatePinnedToCore(uartConsoleTask, "dev_console_uart", 4096, nullptr, 3, nullptr,
                                tskNO_AFFINITY);
        listeners++;
    } else {
        ESP_LOGW(TAG, "UART0 console input unavailable: %s", esp_err_to_name(err));
    }

    // --- Native USB-Serial-JTAG (the board's other connector) -------------
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 2048,
        .rx_buffer_size = 1024,
    };
    // Driver is used for RX ONLY, for the same reason as above: routing logs
    // through the driver's bounded TX buffer made log calls BLOCK whenever
    // the host lagged, stalling the RS485 RX task mid-transfer and
    // overflowing its ring buffer (seen as "RX overflow" during updates).
    err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK) {
        xTaskCreatePinnedToCore(consoleTask, "dev_console_usb", 4096, nullptr, 3, nullptr,
                                tskNO_AFFINITY);
        listeners++;
    } else {
        ESP_LOGW(TAG, "native USB console input unavailable: %s", esp_err_to_name(err));
    }

    if (listeners == 0) {
        ESP_LOGE(TAG, "No console input available -- dev commands are DISABLED");
        return;
    }
    ESP_LOGI(TAG, "Dev console ready on %d port(s) (sync|push|pull|baud N|baudlocal N|update-reset|status|dial N|speed N|setpoint N|mode N|toggle ID 0|1|pulse|mstate)",
             listeners);
}

} // namespace dev_console

#else // production build

namespace dev_console {
void init() {}
} // namespace dev_console

#endif // APP_DEV_MODE
