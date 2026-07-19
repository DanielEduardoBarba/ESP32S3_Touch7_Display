#include "rs485.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace rs485 {
namespace {

const char *TAG = "rs485";

// RS485 pins confirmed from Waveshare's OFFICIAL demos for BOTH board
// variants (identical on each):
//   - "7":  ESP32-S3-Touch-LCD-7-Demo  Arduino/examples/02_RS485_Test: RX=15, TX=16
//   - "7B": github.com/waveshareteam/ESP32-S3-Touch-LCD-7B
//           examples/Arduino/examples/05_RS485: RX=15, TX=16
// (An earlier revision of this file had placeholder TX=17/RX=18 -- those are
// RGB LCD data lines on both boards, which both broke RS485 AND fought the
// display signals. Never reuse 17/18 here.)
constexpr uart_port_t RS485_UART = UART_NUM_1;
constexpr int RS485_TX_GPIO = 16;
constexpr int RS485_RX_GPIO = 15;
// Same rate as Waveshare's own RS485 demos for these boards. The "Machine
// sync" packet protocol (see rs485_protocol.h) only sends a handful of
// bytes per user action, so throughput is irrelevant -- reliability wins.
// (Both ends of the bus must always use the same baud rate.)
constexpr int RS485_BAUD_RATE = 115200;
constexpr int RS485_RX_BUF_SIZE = 512;

RxCallback s_rx_cb;

void rxTask(void *arg)
{
    uint8_t buf[RS485_RX_BUF_SIZE];
    while (true) {
        int len = uart_read_bytes(RS485_UART, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (len > 0 && s_rx_cb) {
            s_rx_cb(buf, static_cast<size_t>(len));
        }
        // Loop tick, non-blocking beyond the 50ms read timeout above, so this
        // task never starves the WiFi/LVGL/web-server tasks.
    }
}

} // namespace

void init()
{
    uart_config_t cfg = {};
    cfg.baud_rate = RS485_BAUD_RATE;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(RS485_UART, RS485_RX_BUF_SIZE * 2, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(RS485_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART, RS485_TX_GPIO, RS485_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // 6KB stack: the RX callback chain now includes comm_protocol parsing
    // and fw_update's esp_ota_write() (device-to-device updates), which
    // need more headroom than plain byte forwarding did.
    xTaskCreatePinnedToCore(rxTask, "rs485_rx", 6144, nullptr, 5, nullptr, tskNO_AFFINITY);

    ESP_LOGI(TAG, "RS485 initialized on UART%d @ %d baud", (int)RS485_UART, RS485_BAUD_RATE);
}

void send(const uint8_t *data, size_t len)
{
    uart_write_bytes(RS485_UART, reinterpret_cast<const char *>(data), len);
}

void send(const std::string &text)
{
    send(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

void onReceive(RxCallback cb)
{
    s_rx_cb = std::move(cb);
}

} // namespace rs485
