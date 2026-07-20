#include "log_store.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app_config.h"

namespace log_store {
namespace {

// Fixed-size ring of lines in ONE block of PSRAM. Internal RAM is precious
// (task stacks and WiFi/httpd internals can ONLY live there) -- an earlier
// std::string-based version of this store slowly ate 30-40KB of internal
// heap and starved task creation (web server, fw_update sender).
constexpr size_t LINE_MAX = 160;

vprintf_like_t s_original = nullptr;
std::mutex s_mutex;
char (*s_lines)[LINE_MAX] = nullptr;
size_t s_head = 0;   // next slot to write
size_t s_count = 0;  // valid lines stored (<= APP_LOG_STORE_MAX)
uint32_t s_revision = 0;

/** The vprintf hook: forward to the original console writer untouched,
 *  then store a cleaned copy (no ANSI color codes, no trailing newline).
 *  Must never call ESP_LOGx itself (would recurse). */
int hook(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int written = s_original != nullptr ? s_original(fmt, args) : vprintf(fmt, args);

    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);

    char line[LINE_MAX];
    size_t pos = 0;
    for (const char *p = buf; *p != '\0' && pos < LINE_MAX - 1;) {
        if (*p == '\033') { // ANSI escape: skip through the terminating letter
            while (*p != '\0' && *p != 'm') {
                p++;
            }
            if (*p != '\0') {
                p++;
            }
        } else if (*p == '\r' || *p == '\n') {
            p++;
        } else {
            line[pos++] = *p++;
        }
    }
    line[pos] = '\0';

    if (pos > 0 && s_lines != nullptr) {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::memcpy(s_lines[s_head], line, pos + 1);
        s_head = (s_head + 1) % APP_LOG_STORE_MAX;
        if (s_count < APP_LOG_STORE_MAX) {
            s_count++;
        }
        s_revision++;
    }
    return written;
}

} // namespace

void init()
{
    // One 300 x 160B block (~48KB) in PSRAM; internal RAM stays untouched.
    s_lines = static_cast<char(*)[LINE_MAX]>(
        heap_caps_calloc(APP_LOG_STORE_MAX, LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_lines == nullptr) { // no PSRAM? fall back rather than lose the feature
        s_lines = static_cast<char(*)[LINE_MAX]>(calloc(APP_LOG_STORE_MAX, LINE_MAX));
    }
    s_original = esp_log_set_vprintf(hook);
}

uint32_t revision()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_revision;
}

std::vector<std::string> snapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    std::vector<std::string> out;
    out.reserve(s_count);
    size_t start = (s_head + APP_LOG_STORE_MAX - s_count) % APP_LOG_STORE_MAX;
    for (size_t i = 0; i < s_count; i++) {
        out.emplace_back(s_lines[(start + i) % APP_LOG_STORE_MAX]);
    }
    return out;
}

} // namespace log_store
