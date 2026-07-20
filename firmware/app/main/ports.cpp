#include "ports.h"

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "comm_protocol.h"
#include "fw_update.h"
#include "rs485.h"
#include "user_store.h"

#if PORTS_ENABLE_CAN
#include "driver/twai.h"
#include "driver/i2c.h"   // IO EXTENSION chip access (CAN transceiver enable)
#endif
#if PORTS_ENABLE_I2C
#include "driver/i2c.h"
#endif
#if PORTS_ENABLE_UART
#include "driver/uart.h"
#endif

namespace ports {
namespace {

const char *TAG = "ports";

// user_store record: { active transport }, schema v1.
constexpr const char *STORE_KEY = "active_port";
constexpr uint8_t STORE_VERSION = 1;

// user_store record: { baud rate, 4 bytes LE }, schema v1.
constexpr const char *BAUD_KEY = "peer_baud";
constexpr uint8_t BAUD_VERSION = 1;

// Baud ladder for the peer link -- EXACT 80MHz divisors ONLY (zero baud
// error; both ends are ESP32-S3s so "standard" PC rates are unnecessary):
// 100k=/800, 200k=/400, 250k=/320, 400k=/200, 500k=/160, 640k=/125,
// 800k=/100, 1M=/80, 1.25M=/64, 1.6M=/50, 2M=/40, 2.5M=/32, 4M=/20,
// 5M=/16 (the S3 UART's ceiling). The REAL limit is the board's RC-timed
// RS485 auto-direction circuit: run tools/baud_test.py and adopt the
// highest rate with zero CRC/resync warnings.
const std::vector<uint32_t> s_baud_rates = {
    100000, 200000, 250000, 400000, 500000, 640000, 800000,
    1000000, 1250000, 1600000, 2000000, 2500000, 4000000, 5000000,
};

Transport s_active = Transport::RS485;
uint32_t s_baud = APP_PEER_BAUD_DEFAULT;
RxCallback s_rx_cb;
std::vector<ChangeCallback> s_change_cbs;

void notifyChange()
{
    for (const auto &cb : s_change_cbs) {
        cb();
    }
}

/** Fans received bytes from whichever transport produced them into the
 *  single registered sink -- but only when that transport is the active
 *  one, so an inactive link can't inject frames. */
void deliver(Transport source, const uint8_t *data, size_t len)
{
    if (source == s_active && s_rx_cb) {
        s_rx_cb(data, len);
    }
}

// ---------------------------------------------------------------------------
// CAN / TWAI (TX=GPIO20, RX=GPIO19; 7B needs IO-extension IO5 high)
// ---------------------------------------------------------------------------
#if PORTS_ENABLE_CAN
namespace can_link {

// CAN frames carry at most 8 data bytes, so the byte stream is chopped into
// consecutive 8-byte frames with a fixed identifier; comm_protocol's framing
// (STX/len/CRC/ETX) reassembles the stream on the far side.
constexpr uint32_t CAN_MSG_ID = APP_CAN_MSG_ID;

void enableTransceiver()
{
    // 7B route: the CAN transceiver shares a USB/CAN mux controlled by the
    // IO EXTENSION chip (I2C 0x24): reg 0x03 = output bitmask, IO5 high
    // selects CAN. Harmless no-op on boards without the chip.
    const uint8_t mode_all_out[2] = {0x02, 0xFF};
    const uint8_t io5_high[2] = {0x03, 0xFF}; // all high incl. IO5
    i2c_master_write_to_device(I2C_NUM_0, 0x24, mode_all_out, 2, pdMS_TO_TICKS(50));
    i2c_master_write_to_device(I2C_NUM_0, 0x24, io5_high, 2, pdMS_TO_TICKS(50));
}

void rxTask(void *)
{
    while (true) {
        twai_message_t msg;
        if (twai_receive(&msg, pdMS_TO_TICKS(100)) == ESP_OK && !msg.rtr) {
            deliver(Transport::CAN, msg.data, msg.data_length_code);
        }
    }
}

void init()
{
    enableTransceiver();
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_20, GPIO_NUM_19, TWAI_MODE_NORMAL);
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, &t, &f) == ESP_OK && twai_start() == ESP_OK) {
        xTaskCreatePinnedToCore(rxTask, "can_rx", 3072, nullptr, 5, nullptr, tskNO_AFFINITY);
        ESP_LOGI(TAG, "CAN/TWAI up (TX=20 RX=19 @500kbit)");
    } else {
        ESP_LOGE(TAG, "CAN/TWAI init failed");
    }
}

void send(const uint8_t *data, size_t len)
{
    while (len > 0) {
        twai_message_t msg = {};
        msg.identifier = CAN_MSG_ID;
        msg.data_length_code = len > 8 ? 8 : len;
        std::memcpy(msg.data, data, msg.data_length_code);
        twai_transmit(&msg, pdMS_TO_TICKS(100));
        data += msg.data_length_code;
        len -= msg.data_length_code;
    }
}

} // namespace can_link
#endif // PORTS_ENABLE_CAN

// ---------------------------------------------------------------------------
// I2C link (SCL=GPIO9, SDA=GPIO8 -- shared with touch/IO-extension bus)
// ---------------------------------------------------------------------------
#if PORTS_ENABLE_I2C
namespace i2c_link {

// Master-write-only link: each board writes frames at the peer's fixed
// address. Half-duplex by design; comm_protocol's request/response pattern
// keeps the two ends from talking over each other.
constexpr uint8_t PEER_ADDR = APP_I2C_PEER_ADDR;

void init()
{
    // The touch driver already installed I2C_NUM_0 on these pins during
    // board bring-up; nothing to install here, we just borrow the bus.
    ESP_LOGI(TAG, "I2C link ready (shared bus, peer addr 0x%02x)", PEER_ADDR);
}

void send(const uint8_t *data, size_t len)
{
    i2c_master_write_to_device(I2C_NUM_0, PEER_ADDR, data, len, pdMS_TO_TICKS(100));
}

} // namespace i2c_link
#endif // PORTS_ENABLE_I2C

// ---------------------------------------------------------------------------
// Raw UART header link (UART2, TX=GPIO43, RX=GPIO44)
// ---------------------------------------------------------------------------
#if PORTS_ENABLE_UART
namespace uart_link {

constexpr uart_port_t PORT = UART_NUM_2;
constexpr int BUF_SIZE = 512;

void rxTask(void *)
{
    uint8_t buf[BUF_SIZE];
    while (true) {
        int len = uart_read_bytes(PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (len > 0) {
            deliver(Transport::UART, buf, static_cast<size_t>(len));
        }
    }
}

void init()
{
    uart_config_t cfg = {};
    cfg.baud_rate = APP_PEER_BAUD_DEFAULT;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(PORT, BUF_SIZE * 2, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(PORT, 43, 44, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreatePinnedToCore(rxTask, "uart_link_rx", 3072, nullptr, 5, nullptr, tskNO_AFFINITY);
    ESP_LOGI(TAG, "UART link up (UART2 TX=43 RX=44 @115200)");
}

void send(const uint8_t *data, size_t len)
{
    uart_write_bytes(PORT, reinterpret_cast<const char *>(data), len);
}

} // namespace uart_link
#endif // PORTS_ENABLE_UART

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

const std::vector<TransportInfo> s_transports = {
    {Transport::RS485, "rs485", "RS485",   PORTS_ENABLE_RS485 != 0},
    {Transport::CAN,   "can",   "CAN bus", PORTS_ENABLE_CAN != 0},
    {Transport::I2C,   "i2c",   "I2C",     PORTS_ENABLE_I2C != 0},
    {Transport::UART,  "uart",  "UART",    PORTS_ENABLE_UART != 0},
};

bool isEnabled(Transport t)
{
    for (const auto &info : s_transports) {
        if (info.id == t) {
            return info.enabled;
        }
    }
    return false;
}

/** Applies a baud rate to whichever serial links are compiled in. */
void applyBaud(uint32_t baud)
{
#if PORTS_ENABLE_RS485
    rs485::setBaud(baud);
#endif
#if PORTS_ENABLE_UART
    uart_set_baudrate(uart_link::PORT, baud);
#endif
}

} // namespace

void init()
{
    // Bring up every compiled-in transport. Each keeps receiving in the
    // background; deliver() drops bytes from whichever isn't active.
#if PORTS_ENABLE_RS485
    rs485::init();
    rs485::onReceive([](const uint8_t *data, size_t len) {
        deliver(Transport::RS485, data, len);
    });
#endif
#if PORTS_ENABLE_CAN
    can_link::init();
#endif
#if PORTS_ENABLE_I2C
    i2c_link::init();
#endif
#if PORTS_ENABLE_UART
    uart_link::init();
#endif

    // Restore the persisted selection if it's still a compiled-in transport
    // (a config change may have disabled it since it was saved).
    uint8_t stored_version = 0, stored_value = 0;
    if (user_store::getU8(STORE_KEY, &stored_version, &stored_value) &&
        stored_version == STORE_VERSION && isEnabled(static_cast<Transport>(stored_value))) {
        s_active = static_cast<Transport>(stored_value);
    } else {
        s_active = Transport::RS485;
    }
    ESP_LOGI(TAG, "Active transport: %s", name(s_active));

    // Restore the persisted baud rate (if it's still a supported one).
    uint8_t baud_version = 0;
    uint32_t stored_baud = 0;
    size_t baud_len = 0;
    if (user_store::get(BAUD_KEY, &baud_version, &stored_baud, sizeof(stored_baud), &baud_len) &&
        baud_version == BAUD_VERSION && baud_len == sizeof(stored_baud) &&
        std::find(s_baud_rates.begin(), s_baud_rates.end(), stored_baud) != s_baud_rates.end()) {
        s_baud = stored_baud;
        ESP_LOGI(TAG, "Restored saved baud rate %lu from NVS", (unsigned long)s_baud);
    } else {
        ESP_LOGI(TAG, "No saved baud rate in NVS -- using default %d", APP_PEER_BAUD_DEFAULT);
    }
    if (s_baud != APP_PEER_BAUD_DEFAULT) {
        applyBaud(s_baud);
    }
    ESP_LOGI(TAG, "Peer link baud rate: %lu", (unsigned long)s_baud);

    // Follow a peer's "switch baud NOW" broadcast (without re-broadcasting,
    // which would ping-pong forever).
    comm_protocol::onBaudChangeReceived([](uint32_t baud) {
        ESP_LOGI(TAG, "Peer requested baud switch to %lu", (unsigned long)baud);
        setBaud(baud, /*announce=*/false);
    });

    // The GUI is built BEFORE this init runs (it registers an onChange
    // observer at build time) -- fire it now so every view picks up the
    // restored transport + baud instead of showing compile-time defaults.
    notifyChange();
}

const std::vector<TransportInfo> &transports()
{
    return s_transports;
}

Transport active()
{
    return s_active;
}

const char *name(Transport t)
{
    for (const auto &info : s_transports) {
        if (info.id == t) {
            return info.name;
        }
    }
    return "?";
}

bool setActive(Transport t)
{
    if (!isEnabled(t)) {
        ESP_LOGW(TAG, "Transport '%s' is not enabled in ports_config.h", name(t));
        return false;
    }
    if (changeLocked()) {
        ESP_LOGW(TAG, "Transport change refused: firmware update transfer in progress");
        return false;
    }
    s_active = t;
    if (!user_store::putU8(STORE_KEY, STORE_VERSION, static_cast<uint8_t>(t))) {
        ESP_LOGE(TAG, "FAILED to persist transport selection to NVS -- it will NOT survive a reboot");
    }
    ESP_LOGI(TAG, "Active transport switched to %s", name(t));
    notifyChange();
    return true;
}

const std::vector<uint32_t> &baudRates()
{
    return s_baud_rates;
}

uint32_t baud()
{
    return s_baud;
}

bool setBaud(uint32_t baud, bool announce)
{
    if (std::find(s_baud_rates.begin(), s_baud_rates.end(), baud) == s_baud_rates.end()) {
        ESP_LOGW(TAG, "Unsupported baud rate %lu", (unsigned long)baud);
        return false;
    }
    if (changeLocked()) {
        ESP_LOGW(TAG, "Baud change refused: firmware update transfer in progress");
        return false;
    }
    if (baud == s_baud) {
        // Already there -- also makes the peer's reaction to our second
        // callout (below) a harmless no-op instead of an echo loop.
        return true;
    }
    if (announce) {
        // Callout #1 AT THE CURRENT RATE: peers still on the old baud take
        // this as a command (like the toggle frames) and switch immediately.
        ESP_LOGI(TAG, "Baud switch: calling out %lu -> %lu at the current rate",
                 (unsigned long)s_baud, (unsigned long)baud);
        comm_protocol::sendBaudChange(baud);
        vTaskDelay(pdMS_TO_TICKS(100)); // let the frame drain before retuning
    }
    s_baud = baud;
    applyBaud(baud);
    // Remembered as a user setting -- restored on every boot (see init()).
    if (!user_store::put(BAUD_KEY, BAUD_VERSION, &baud, sizeof(baud))) {
        ESP_LOGE(TAG, "FAILED to persist baud rate to NVS -- it will NOT survive a reboot");
    } else {
        ESP_LOGI(TAG, "Baud rate %lu saved to NVS (user setting, survives reboot)",
                 (unsigned long)baud);
    }
    if (announce) {
        // Callout #2 AT THE NEW RATE: catches any peer that was already on
        // the target baud (or missed the first frame) so everyone converges.
        vTaskDelay(pdMS_TO_TICKS(50));
        comm_protocol::sendBaudChange(baud);
        ESP_LOGI(TAG, "Baud switch complete: now at %lu (callout repeated at new rate)",
                 (unsigned long)baud);
    } else {
        ESP_LOGI(TAG, "Baud rate switched to %lu (following peer's callout)",
                 (unsigned long)baud);
    }
    notifyChange();
    return true;
}

bool changeLocked()
{
    fw_update::State st = fw_update::status().state;
    return st == fw_update::State::Sending || st == fw_update::State::Receiving;
}

void onChange(ChangeCallback cb)
{
    s_change_cbs.push_back(std::move(cb));
}

void send(const uint8_t *data, size_t len)
{
    switch (s_active) {
#if PORTS_ENABLE_RS485
    case Transport::RS485: rs485::send(data, len); break;
#endif
#if PORTS_ENABLE_CAN
    case Transport::CAN:   can_link::send(data, len); break;
#endif
#if PORTS_ENABLE_I2C
    case Transport::I2C:   i2c_link::send(data, len); break;
#endif
#if PORTS_ENABLE_UART
    case Transport::UART:  uart_link::send(data, len); break;
#endif
    default:
        ESP_LOGW(TAG, "send() dropped: active transport not compiled in");
        break;
    }
}

void onReceive(RxCallback cb)
{
    s_rx_cb = std::move(cb);
}

} // namespace ports
