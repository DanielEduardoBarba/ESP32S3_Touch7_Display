#include "rs485.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"

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
// Startup rate; the Ports scene can change it at runtime via setBaud()
// (coordinated across both boards by ports::setBaud). Both ends of the bus
// must always use the same rate.
constexpr int RS485_BAUD_RATE = APP_PEER_BAUD_DEFAULT;
// Sized for a full firmware-update chunk frame (APP_UPDATE_CHUNK_SIZE +
// framing) with headroom.
constexpr int RS485_RX_BUF_SIZE = 4096;

RxCallback s_rx_cb;
QueueHandle_t s_uart_queue = nullptr;

/** Event-driven RX: the UART driver signals as soon as data arrives (or
 *  the line goes idle), so frames are delivered within ~1ms instead of
 *  after a fixed polling timeout. That latency matters a LOT for the
 *  firmware update's stop-and-wait ACKs: the old 50ms-per-direction
 *  polling capped transfers at ~1.5-2.5KB/s REGARDLESS of baud rate. */
void rxTask(void *arg)
{
    static uint8_t buf[RS485_RX_BUF_SIZE];
    uart_event_t event;
    int64_t last_rx_us = 0;
    while (true) {
        if (xQueueReceive(s_uart_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (event.type) {
        case UART_DATA: {
            // Idle-gap resync: a frame never legitimately pauses mid-flight
            // (at these rates a whole frame is <100ms), so a long silence
            // means any half-parsed frame is dead -- tell the parser BEFORE
            // delivering the new bytes. This is what lets a retransmission
            // after a 3s ACK timeout parse cleanly instead of being eaten
            // as "payload" of a truncated frame (deterministic livelock).
            int64_t now = esp_timer_get_time();
            if (last_rx_us != 0 && (now - last_rx_us) > 50000 && s_rx_cb) {
                s_rx_cb(nullptr, 0); // stream-break marker (no-op if parser idle)
            }
            last_rx_us = now;

            size_t to_read = event.size < sizeof(buf) ? event.size : sizeof(buf);
            int len = uart_read_bytes(RS485_UART, buf, to_read, 0);
            if (len > 0 && s_rx_cb) {
                s_rx_cb(buf, static_cast<size_t>(len));
            }
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            // Drop the backlog and resync. CRITICAL: the parser downstream
            // may be mid-frame -- deliver a zero-length "stream break" so it
            // resets too, otherwise every retransmission gets consumed as
            // payload of the truncated old frame and never parses again.
            ESP_LOGW(TAG, "RX overflow (%s) -- flushing input and resetting the frame parser",
                     event.type == UART_FIFO_OVF ? "hw FIFO" : "ring buffer");
            uart_flush_input(RS485_UART);
            xQueueReset(s_uart_queue);
            if (s_rx_cb) {
                s_rx_cb(nullptr, 0); // stream-break marker
            }
            break;
        default:
            break;
        }
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

    ESP_ERROR_CHECK(uart_driver_install(RS485_UART, RS485_RX_BUF_SIZE * 4, 0, 32, &s_uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(RS485_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART, RS485_TX_GPIO, RS485_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // 8KB stack: the RX callback chain includes comm_protocol parsing and
    // fw_update's esp_ota_write() (device-to-device updates), plus the 2KB
    // local frame buffer headroom.
    xTaskCreatePinnedToCore(rxTask, "rs485_rx", 8192, nullptr, 5, nullptr, tskNO_AFFINITY);

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

void setBaud(uint32_t baud)
{
    uart_wait_tx_done(RS485_UART, pdMS_TO_TICKS(500)); // drain at the old rate first
    uart_set_baudrate(RS485_UART, baud);
    ESP_LOGI(TAG, "RS485 baud rate changed to %lu", (unsigned long)baud);
}

} // namespace rs485
