#include "user_store.h"

#include <cstring>
#include <vector>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace user_store {
namespace {

const char *TAG = "user_store";

// All user records live in their own namespace so they can never collide
// with ESP-IDF's system entries (WiFi credentials etc. live elsewhere).
constexpr const char *NVS_NAMESPACE = "userdata";

// Tiny per-record header, prepended to every blob. The magic byte guards
// against reading a blob that was never written by this scheme.
constexpr uint8_t RECORD_MAGIC = 0xA5;
struct RecordHeader {
    uint8_t magic;
    uint8_t version;
} __attribute__((packed));

bool s_inited = false;

} // namespace

void init()
{
    if (s_inited) {
        return;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Partition is unusable as-is (e.g. truncated or from a newer IDF):
        // wipe and retry -- user data is expendable, boot data isn't here.
        ESP_LOGW(TAG, "NVS needs erase (%s), reformatting", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }
    s_inited = true;
}

bool put(const char *key, uint8_t version, const void *data, size_t len)
{
    init();
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    // Header + payload written as one blob so the pair is always atomic.
    std::vector<uint8_t> blob(sizeof(RecordHeader) + len);
    RecordHeader header = {RECORD_MAGIC, version};
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), data, len);

    esp_err_t err = nvs_set_blob(handle, key, blob.data(), blob.size());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "put('%s') failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool get(const char *key, uint8_t *version_out, void *out, size_t max_len, size_t *len_out)
{
    init();
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t blob_len = 0;
    esp_err_t err = nvs_get_blob(handle, key, nullptr, &blob_len);
    if (err != ESP_OK || blob_len < sizeof(RecordHeader)) {
        nvs_close(handle);
        return false;
    }

    std::vector<uint8_t> blob(blob_len);
    err = nvs_get_blob(handle, key, blob.data(), &blob_len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }

    RecordHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    if (header.magic != RECORD_MAGIC) {
        ESP_LOGW(TAG, "get('%s'): bad record magic 0x%02x", key, header.magic);
        return false;
    }

    size_t payload_len = blob_len - sizeof(RecordHeader);
    if (payload_len > max_len) {
        ESP_LOGW(TAG, "get('%s'): record %zu bytes > buffer %zu", key, payload_len, max_len);
        return false;
    }

    std::memcpy(out, blob.data() + sizeof(RecordHeader), payload_len);
    if (version_out != nullptr) {
        *version_out = header.version;
    }
    if (len_out != nullptr) {
        *len_out = payload_len;
    }
    return true;
}

bool putU8(const char *key, uint8_t version, uint8_t value)
{
    return put(key, version, &value, 1);
}

bool getU8(const char *key, uint8_t *version_out, uint8_t *value_out)
{
    size_t len = 0;
    uint8_t value = 0;
    if (!get(key, version_out, &value, 1, &len) || len != 1) {
        return false;
    }
    *value_out = value;
    return true;
}

} // namespace user_store
