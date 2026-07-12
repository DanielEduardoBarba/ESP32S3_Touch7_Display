#include "ota_handler.h"

#include <algorithm>
#include <cinttypes>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ota_handler {
namespace {
const char *TAG = "ota_handler";
constexpr size_t CHUNK_SIZE = 4096;
} // namespace

esp_err_t handlePost(httpd_req_t *req)
{
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
    if (update_partition == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Writing OTA update (%zu bytes) to partition '%s' at 0x%" PRIx32,
             req->content_len, update_partition->label, update_partition->address);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }

    char buf[CHUNK_SIZE];
    int remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, buf, std::min((size_t)remaining, sizeof(buf)));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write failed");
            return ESP_FAIL;
        }
        remaining -= received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                             err == ESP_ERR_OTA_VALIDATE_FAILED ? "Image validation failed" : "esp_ota_end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_set_boot_partition failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Update applied, rebooting...\"}");
    ESP_LOGI(TAG, "OTA update complete, rebooting into '%s'", update_partition->label);

    // Give the HTTP response time to flush before restarting.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

} // namespace ota_handler
