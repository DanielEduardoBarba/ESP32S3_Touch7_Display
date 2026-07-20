#include "log_store.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "esp_log.h"

#include "app_config.h"

namespace log_store {
namespace {

vprintf_like_t s_original = nullptr;
std::mutex s_mutex;
std::deque<std::string> s_lines;
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

    std::string line;
    line.reserve(std::strlen(buf));
    for (const char *p = buf; *p != '\0';) {
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
            line.push_back(*p++);
        }
    }
    if (!line.empty()) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_lines.push_back(std::move(line));
        while (s_lines.size() > APP_LOG_STORE_MAX) {
            s_lines.pop_front();
        }
        s_revision++;
    }
    return written;
}

} // namespace

void init()
{
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
    return {s_lines.begin(), s_lines.end()};
}

} // namespace log_store
