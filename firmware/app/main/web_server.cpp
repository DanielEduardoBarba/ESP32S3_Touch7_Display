#include "web_server.h"

#include <algorithm>
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

#include "fw_update.h"
#include "log_store.h"
#include "machine_state.h"
#include "ota_handler.h"
#include "ports.h"
#include "storage.h"
#include "wifi_manager.h"

namespace web_server {
namespace {

const char *TAG = "web_server";

std::mutex s_scan_mutex;
std::vector<wifi_manager::ApInfo> s_latest_scan;

// --- WebSocket state (see machine_state.h for the sync behavior this
// implements) ---
httpd_handle_t s_server = nullptr;
std::mutex s_ws_mutex;
std::vector<int> s_ws_client_fds;

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
    cJSON_AddStringToObject(wifi, "saved_ssid", wifi_manager::savedSsid().c_str());
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

esp_err_t apiWifiForgetHandler(httpd_req_t *req)
{
    wifi_manager::forget();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"forgotten\":true}");
    return ESP_OK;
}

// --- Ports (peer-link transport selection) -------------------------------

esp_err_t apiPortsGetHandler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "active", ports::name(ports::active()));
    cJSON_AddNumberToObject(root, "baud", ports::baud());
    cJSON_AddBoolToObject(root, "locked", ports::changeLocked());
    cJSON *rates = cJSON_AddArrayToObject(root, "baud_rates");
    for (uint32_t rate : ports::baudRates()) {
        cJSON_AddItemToArray(rates, cJSON_CreateNumber(rate));
    }
    cJSON *list = cJSON_AddArrayToObject(root, "transports");
    for (const auto &info : ports::transports()) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", info.name);
        cJSON_AddStringToObject(item, "label", info.label);
        cJSON_AddBoolToObject(item, "enabled", info.enabled);
        cJSON_AddItemToArray(list, item);
    }
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t apiPortsSetHandler(httpd_req_t *req)
{
    char buf[128];
    int received = httpd_req_recv(req, buf, std::min(req->content_len, sizeof(buf) - 1));
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    cJSON *name_item = json ? cJSON_GetObjectItem(json, "transport") : nullptr;
    cJSON *baud_item = json ? cJSON_GetObjectItem(json, "baud") : nullptr;
    bool ok = false;
    const char *error = "unknown or disabled transport";
    if (cJSON_IsString(name_item)) {
        for (const auto &info : ports::transports()) {
            if (std::strcmp(info.name, name_item->valuestring) == 0) {
                ok = ports::setActive(info.id);
                break;
            }
        }
    } else if (cJSON_IsNumber(baud_item)) {
        // Broadcasts the change to the peer first -- see ports::setBaud().
        ok = ports::setBaud(static_cast<uint32_t>(baud_item->valuedouble));
        error = "unsupported baud rate";
    }
    if (!ok && ports::changeLocked()) {
        error = "locked: firmware update transfer in progress";
    }
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    if (ok) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        std::string resp = std::string("{\"ok\":false,\"error\":\"") + error + "\"}";
        httpd_resp_sendstr(req, resp.c_str());
    }
    return ESP_OK;
}

// --- Update (device-to-device firmware transfer) --------------------------

const char *updateStateName(fw_update::State s)
{
    switch (s) {
    case fw_update::State::Idle:        return "idle";
    case fw_update::State::Sending:     return "sending";
    case fw_update::State::Receiving:   return "receiving";
    case fw_update::State::SendDone:    return "send_done";
    case fw_update::State::ReceiveDone: return "receive_done";
    case fw_update::State::Failed:      return "failed";
    }
    return "?";
}

const char *peerCompareName(fw_update::PeerCompare c)
{
    switch (c) {
    case fw_update::PeerCompare::PeerNewer: return "peer_newer";
    case fw_update::PeerCompare::PeerOlder: return "peer_older";
    case fw_update::PeerCompare::Same:      return "same";
    default:                                return "unknown";
    }
}

esp_err_t apiUpdateInfoHandler(httpd_req_t *req)
{
    fw_update::AppInfo info = fw_update::appInfo();
    fw_update::Status st = fw_update::status();
    fw_update::PeerInfo peer = fw_update::peerInfo();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "running_slot", info.running_slot.c_str());
    cJSON_AddStringToObject(root, "app_version", info.app_version.c_str());
    cJSON_AddStringToObject(root, "version", info.version.c_str());
    cJSON_AddStringToObject(root, "idf_version", info.idf_version.c_str());
    cJSON_AddStringToObject(root, "compile_time", info.compile_time.c_str());
    cJSON_AddNumberToObject(root, "image_size", info.image_size);
    cJSON_AddStringToObject(root, "ota_state", info.ota_state.c_str());
    cJSON_AddStringToObject(root, "transfer_state", updateStateName(st.state));
    cJSON_AddNumberToObject(root, "transfer_total", st.total_bytes);
    cJSON_AddNumberToObject(root, "transfer_done", st.done_bytes);
    cJSON_AddNumberToObject(root, "transfer_bps", st.bytes_per_sec);
    cJSON_AddNumberToObject(root, "transfer_elapsed_ms", st.elapsed_ms);
    cJSON_AddStringToObject(root, "transfer_message", st.message.c_str());
    cJSON_AddBoolToObject(root, "peer_known", peer.known);
    cJSON_AddBoolToObject(root, "peer_no_response", peer.no_response);
    cJSON_AddStringToObject(root, "peer_version", peer.version.c_str());
    cJSON_AddStringToObject(root, "peer_compare", peerCompareName(peer.compare));

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t apiUpdateSendHandler(httpd_req_t *req)
{
    bool ok = fw_update::startSend();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}"
                                : "{\"ok\":false,\"error\":\"busy or update pending reboot\"}");
    return ESP_OK;
}

esp_err_t apiUpdateSyncHandler(httpd_req_t *req)
{
    fw_update::syncPeer();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t apiUpdatePullHandler(httpd_req_t *req)
{
    bool ok = fw_update::startPull();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}"
                                : "{\"ok\":false,\"error\":\"busy or update pending reboot\"}");
    return ESP_OK;
}

esp_err_t apiLogsHandler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *lines = cJSON_AddArrayToObject(root, "lines");
    for (const auto &line : log_store::snapshot()) {
        cJSON_AddItemToArray(lines, cJSON_CreateString(line.c_str()));
    }
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t apiUpdateRebootHandler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"rebooting\"}");
    fw_update::rebootIntoUpdate();
    return ESP_OK;
}

// --- WebSocket: makes the web UI a live extension of the touchscreen -----

std::string buildStateJson(const machine_state::State &state)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "dial_value", state.dial_value);
    cJSON_AddBoolToObject(root, "toggle_state", state.toggle_state);
    char *json = cJSON_PrintUnformatted(root);
    std::string result(json);
    cJSON_free(json);
    cJSON_Delete(root);
    return result;
}

/** Sends `json` to one client; if the send fails (client gone), drops it
 *  from our list so we stop wasting time on it. Safe to call from any task
 *  (uses the *_async send variant), which is what lets machine_state.cpp's
 *  RS485/local-UI callers broadcast without caring which task they run on. */
void sendJsonToFd(int fd, const std::string &json)
{
    // Guard against socket-fd reuse: if this client disconnected and the OS
    // recycled its fd number for a NEW plain-HTTP connection, the fd would
    // still be in our list but no longer speak WebSocket -- sending a WS
    // frame at it would corrupt that unrelated response.
    if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        std::lock_guard<std::mutex> lock(s_ws_mutex);
        s_ws_client_fds.erase(std::remove(s_ws_client_fds.begin(), s_ws_client_fds.end(), fd),
                               s_ws_client_fds.end());
        return;
    }

    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(json.data()));
    ws_pkt.len = json.size();

    if (httpd_ws_send_frame_async(s_server, fd, &ws_pkt) != ESP_OK) {
        std::lock_guard<std::mutex> lock(s_ws_mutex);
        s_ws_client_fds.erase(std::remove(s_ws_client_fds.begin(), s_ws_client_fds.end(), fd),
                               s_ws_client_fds.end());
    }
}

/** Registered with machine_state::onStateChange(); pushes the new state to
 *  every connected browser, whatever originally caused the change (local
 *  touchscreen, RS485, or even another browser). */
void broadcastState(const machine_state::State &state)
{
    if (s_server == nullptr) {
        return;
    }
    std::string json = buildStateJson(state);

    std::vector<int> fds_copy;
    {
        std::lock_guard<std::mutex> lock(s_ws_mutex);
        fds_copy = s_ws_client_fds;
    }
    for (int fd : fds_copy) {
        sendJsonToFd(fd, json);
    }
}

esp_err_t wsHandler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        // esp_http_server has already completed the low-level WS handshake
        // by the time this is called with method GET -- just remember this
        // client and bring it up to date with the current state.
        {
            std::lock_guard<std::mutex> lock(s_ws_mutex);
            if (std::find(s_ws_client_fds.begin(), s_ws_client_fds.end(), fd) == s_ws_client_fds.end()) {
                s_ws_client_fds.push_back(fd);
            }
        }
        sendJsonToFd(fd, buildStateJson(machine_state::current()));
        return ESP_OK;
    }

    // Any other invocation means an actual WS data frame arrived. Read it
    // in the standard two-step way: first with no buffer to learn the
    // length, then again into a buffer sized for it.
    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (err != ESP_OK || ws_pkt.len == 0) {
        return err;
    }

    std::string body(ws_pkt.len, '\0');
    ws_pkt.payload = reinterpret_cast<uint8_t *>(body.data());
    err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *json = cJSON_ParseWithLength(body.data(), body.size());
    if (json == nullptr) {
        return ESP_OK; // ignore malformed messages rather than dropping the connection
    }

    cJSON *type_item = cJSON_GetObjectItem(json, "type");
    if (cJSON_IsString(type_item)) {
        if (std::strcmp(type_item->valuestring, "dial") == 0) {
            cJSON *value_item = cJSON_GetObjectItem(json, "value");
            if (cJSON_IsNumber(value_item)) {
                int value = value_item->valueint;
                value = std::max(0, std::min(100, value));
                // Treated exactly like a touchscreen drag-release: moves the
                // on-screen dial AND sends an RS485 packet to the other board.
                machine_state::setDialFromWeb(static_cast<uint8_t>(value));
            }
        } else if (std::strcmp(type_item->valuestring, "toggle") == 0) {
            cJSON *state_item = cJSON_GetObjectItem(json, "state");
            if (cJSON_IsBool(state_item)) {
                machine_state::setToggleFromWeb(cJSON_IsTrue(state_item));
            }
        }
    }
    cJSON_Delete(json);

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
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t status_uri = {"/api/status", HTTP_GET, apiStatusHandler, nullptr};
    static const httpd_uri_t scan_uri = {"/api/wifi/scan", HTTP_GET, apiWifiScanHandler, nullptr};
    static const httpd_uri_t connect_uri = {"/api/wifi/connect", HTTP_POST, apiWifiConnectHandler, nullptr};
    static const httpd_uri_t forget_uri = {"/api/wifi/forget", HTTP_POST, apiWifiForgetHandler, nullptr};
    static const httpd_uri_t ota_uri = {"/api/ota", HTTP_POST, ota_handler::handlePost, nullptr};
    static const httpd_uri_t ports_get_uri = {"/api/ports", HTTP_GET, apiPortsGetHandler, nullptr};
    static const httpd_uri_t ports_set_uri = {"/api/ports", HTTP_POST, apiPortsSetHandler, nullptr};
    static const httpd_uri_t update_info_uri = {"/api/update/info", HTTP_GET, apiUpdateInfoHandler, nullptr};
    static const httpd_uri_t update_send_uri = {"/api/update/send", HTTP_POST, apiUpdateSendHandler, nullptr};
    static const httpd_uri_t update_sync_uri = {"/api/update/sync", HTTP_POST, apiUpdateSyncHandler, nullptr};
    static const httpd_uri_t update_pull_uri = {"/api/update/pull", HTTP_POST, apiUpdatePullHandler, nullptr};
    static const httpd_uri_t update_reboot_uri = {"/api/update/reboot", HTTP_POST, apiUpdateRebootHandler, nullptr};
    static const httpd_uri_t logs_uri = {"/api/logs", HTTP_GET, apiLogsHandler, nullptr};
    static const httpd_uri_t ws_uri = {"/ws", HTTP_GET, wsHandler, nullptr, true};
    static const httpd_uri_t static_uri = {"/*", HTTP_GET, staticFileHandler, nullptr};

    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &scan_uri);
    httpd_register_uri_handler(s_server, &connect_uri);
    httpd_register_uri_handler(s_server, &forget_uri);
    httpd_register_uri_handler(s_server, &ota_uri);
    httpd_register_uri_handler(s_server, &ports_get_uri);
    httpd_register_uri_handler(s_server, &ports_set_uri);
    httpd_register_uri_handler(s_server, &update_info_uri);
    httpd_register_uri_handler(s_server, &update_send_uri);
    httpd_register_uri_handler(s_server, &update_sync_uri);
    httpd_register_uri_handler(s_server, &update_pull_uri);
    httpd_register_uri_handler(s_server, &update_reboot_uri);
    httpd_register_uri_handler(s_server, &logs_uri);
    httpd_register_uri_handler(s_server, &ws_uri);
    httpd_register_uri_handler(s_server, &static_uri); // must be registered last (wildcard)

    // Whenever the Machine scene's state changes for ANY reason (touchscreen,
    // RS485, or another browser), push the new state to every connected client.
    machine_state::onStateChange(broadcastState);

    ESP_LOGI(TAG, "Web server listening on port %d (serving from %s)", config.server_port, storage::webRootPath());
}

} // namespace web_server
