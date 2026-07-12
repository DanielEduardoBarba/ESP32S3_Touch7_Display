#include "web_server.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "ota_handler.h"
#include "storage.h"
#include "wifi_manager.h"

namespace web_server {
namespace {

const char *TAG = "web_server";

std::mutex s_scan_mutex;
std::vector<wifi_manager::ApInfo> s_latest_scan;

bool endsWith(const std::string &s, const char *suffix)
{
    size_t len = std::strlen(suffix);
    return s.size() >= len && s.compare(s.size() - len, len, suffix) == 0;
}

const char *contentTypeFor(const std::string &path)
{
    if (endsWith(path, ".html")) return "text/html";
    if (endsWith(path, ".js")) return "application/javascript";
    if (endsWith(path, ".css")) return "text/css";
    if (endsWith(path, ".json")) return "application/json";
    if (endsWith(path, ".svg")) return "image/svg+xml";
    if (endsWith(path, ".png")) return "image/png";
    if (endsWith(path, ".ico")) return "image/x-icon";
    if (endsWith(path, ".woff2")) return "font/woff2";
    return "application/octet-stream";
}

const char *wifiStateToString(wifi_manager::State state)
{
    switch (state) {
    case wifi_manager::State::Connecting:
        return "connecting";
    case wifi_manager::State::Connected:
        return "connected";
    case wifi_manager::State::Failed:
        return "failed";
    default:
        return "disconnected";
    }
}

esp_err_t staticFileHandler(httpd_req_t *req)
{
    std::string uri = req->uri;
    auto q = uri.find('?');
    if (q != std::string::npos) {
        uri = uri.substr(0, q);
    }
    if (uri.empty() || uri == "/") {
        uri = "/index.html";
    }

    std::string path = std::string(storage::webRootPath()) + uri;
    FILE *f = fopen(path.c_str(), "r");
    if (!f) {
        // SPA fallback: unknown routes (e.g. client-side router paths) serve
        // index.html instead of a 404.
        path = std::string(storage::webRootPath()) + "/index.html";
        f = fopen(path.c_str(), "r");
        if (!f) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, contentTypeFor(path));
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t apiStatusHandler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "state", wifiStateToString(wifi_manager::state()));
    cJSON_AddStringToObject(wifi, "ssid", wifi_manager::currentSsid().c_str());
    cJSON_AddStringToObject(wifi, "ip", wifi_manager::ipAddress().c_str());
    cJSON_AddNumberToObject(wifi, "rssi", wifi_manager::rssi());
    cJSON_AddItemToObject(root, "wifi", wifi);

    const char *src = "none";
    switch (storage::webRootSource()) {
    case storage::WebRootSource::Spiffs:
        src = "spiffs";
        break;
    case storage::WebRootSource::SdCard:
        src = "sdcard";
        break;
    default:
        break;
    }
    cJSON_AddStringToObject(root, "web_root_source", src);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t apiWifiScanHandler(httpd_req_t *req)
{
    wifi_manager::startScan();

    cJSON *arr = cJSON_CreateArray();
    {
        std::lock_guard<std::mutex> lock(s_scan_mutex);
        for (const auto &ap : s_latest_scan) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "ssid", ap.ssid.c_str());
            cJSON_AddNumberToObject(item, "rssi", ap.rssi);
            cJSON_AddBoolToObject(item, "secure", ap.secure);
            cJSON_AddItemToArray(arr, item);
        }
    }
    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(arr);
    return ESP_OK;
}

esp_err_t apiWifiConnectHandler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request body");
        return ESP_FAIL;
    }

    std::string body(req->content_len, '\0');
    int received = httpd_req_recv(req, body.data(), body.size());
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_ParseWithLength(body.data(), received);
    if (json == nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(json, "password");
    std::string ssid = cJSON_IsString(ssid_item) ? ssid_item->valuestring : "";
    std::string password = cJSON_IsString(pass_item) ? pass_item->valuestring : "";
    cJSON_Delete(json);

    if (ssid.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'ssid'");
        return ESP_FAIL;
    }

    wifi_manager::connect(ssid, password);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"connecting\":true}");
    return ESP_OK;
}

} // namespace

void start()
{
    wifi_manager::onScanResults([](const std::vector<wifi_manager::ApInfo> &results) {
        std::lock_guard<std::mutex> lock(s_scan_mutex);
        s_latest_scan = results;
    });

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 12;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t status_uri = {"/api/status", HTTP_GET, apiStatusHandler, nullptr};
    static const httpd_uri_t scan_uri = {"/api/wifi/scan", HTTP_GET, apiWifiScanHandler, nullptr};
    static const httpd_uri_t connect_uri = {"/api/wifi/connect", HTTP_POST, apiWifiConnectHandler, nullptr};
    static const httpd_uri_t ota_uri = {"/api/ota", HTTP_POST, ota_handler::handlePost, nullptr};
    static const httpd_uri_t static_uri = {"/*", HTTP_GET, staticFileHandler, nullptr};

    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &scan_uri);
    httpd_register_uri_handler(server, &connect_uri);
    httpd_register_uri_handler(server, &ota_uri);
    httpd_register_uri_handler(server, &static_uri); // must be registered last (wildcard)

    ESP_LOGI(TAG, "Web server listening on port %d (serving from %s)", config.server_port, storage::webRootPath());
}

} // namespace web_server
