#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * Splits the ESP-IDF log stream: every line still goes to the real console
 * (unchanged), and a copy of the most recent APP_LOG_STORE_MAX lines is
 * kept in a RAM ring buffer for on-device viewing (the Debug scene) and
 * the web GUI (/api/logs).
 */
namespace log_store {

/** Installs the vprintf hook. Call as early as possible in app_main(). */
void init();

/** Increments on every stored line -- cheap "anything new?" check. */
uint32_t revision();

/** Copy of the buffered lines, oldest first (ANSI colors stripped). */
std::vector<std::string> snapshot();

} // namespace log_store
