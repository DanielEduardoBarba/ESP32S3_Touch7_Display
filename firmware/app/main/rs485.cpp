#include "rs485.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace rs485 {
namespace {

const char *TAG = "rs485";

// TODO: verify these against your board revision's schematic
// (https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-7/ESP32-S3-Touch-LCD-7-Sch.pdf).
// The Waveshare wiki's "02_RS485_Test" demo uses UART1 for the RS485 header;
// exact TX/RX GPIOs vary by revision, so they're placeholders here.
constexpr uart_port_t RS485_UART = UART_NUM_1;
constexpr int RS485_TX_GPIO = 17;
constexpr int RS485_RX_GPIO = 18;
constexpr int RS485_BAUD_RATE = 9600;
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

    xTaskCreatePinnedToCore(rxTask, "rs485_rx", 3072, nullptr, 5, nullptr, tskNO_AFFINITY);

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
