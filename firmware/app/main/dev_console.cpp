#include "dev_console.h"

#include "app_config.h"

#if APP_DEV_MODE

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fw_update.h"
#include "ports.h"

namespace dev_console {
namespace {

const char *TAG = "dev_console";

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
    } else {
        ESP_LOGW(TAG, "unknown command: '%s' (try: sync|push|pull|baud N|baudlocal N|update-reset|status)", line);
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

} // namespace

void init()
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 2048,
        .rx_buffer_size = 256,
    };
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        // Route console VFS through the driver so log TX and our RX coexist.
        usb_serial_jtag_vfs_use_driver();
    }
    xTaskCreatePinnedToCore(consoleTask, "dev_console", 4096, nullptr, 3, nullptr, tskNO_AFFINITY);
    ESP_LOGI(TAG, "Dev console ready (sync|push|pull|baud N|baudlocal N|update-reset|status)");
}

} // namespace dev_console

#else // production build

namespace dev_console {
void init() {}
} // namespace dev_console

#endif // APP_DEV_MODE
