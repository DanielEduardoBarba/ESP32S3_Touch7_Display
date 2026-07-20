#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

/**
 * RS485 transport on UART1. Reading happens on its own FreeRTOS task and
 * delivers data via the callback below (register it before calling init());
 * writing is non-blocking (buffered by the UART driver). Neither blocks the
 * LVGL UI, WiFi, or the web server.
 */
namespace rs485 {

using RxCallback = std::function<void(const uint8_t *data, size_t len)>;

void init();
void send(const uint8_t *data, size_t len);
void send(const std::string &text);
void onReceive(RxCallback cb);

/** Runtime baud change (drains pending TX first). Both ends of the bus
 *  must always match -- coordinated by ports::setBaud(). */
void setBaud(uint32_t baud);

} // namespace rs485
