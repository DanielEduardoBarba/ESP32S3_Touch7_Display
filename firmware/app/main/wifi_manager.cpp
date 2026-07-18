#include "wifi_manager.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace wifi_manager {
namespace {

const char *TAG = "wifi_manager";
const char *NVS_NAMESPACE = "wifi_cfg";

esp_netif_t *s_netif = nullptr;
State s_state = State::Disconnected;
std::string s_ssid;
std::string s_ip;
int8_t s_rssi = 0;
bool s_scan_pending = false;

std::vector<ScanResultCallback> s_scan_cbs;
std::vector<StateChangeCallback> s_state_cbs;

void notifyState()
{
    for (const auto &cb : s_state_cbs) {
        cb(s_state, s_ssid, s_ip);
    }
}

void saveCredentials(const std::string &ssid, const std::string &password)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "ssid", ssid.c_str());
    nvs_set_str(h, "pass", password.c_str());
    nvs_commit(h);
    nvs_close(h);
}

bool loadCredentials(std::string &ssid, std::string &password)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char ssid_buf[33] = {0};
    char pass_buf[65] = {0};
    size_t ssid_len = sizeof(ssid_buf);
    size_t pass_len = sizeof(pass_buf);
    bool ok = (nvs_get_str(h, "ssid", ssid_buf, &ssid_len) == ESP_OK) &&
              (nvs_get_str(h, "pass", pass_buf, &pass_len) == ESP_OK);
    nvs_close(h);
    if (ok) {
        ssid = ssid_buf;
        password = pass_buf;
    }
    return ok;
}

void eraseCredentials()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    // erase_key returns ESP_ERR_NVS_NOT_FOUND if there was nothing saved --
    // that's fine, forget() is safe to call with nothing saved.
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
}

void handleScanDone()
{
    s_scan_pending = false;

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    std::vector<ApInfo> results;
    if (num > 0) {
        std::vector<wifi_ap_record_t> records(num);
        if (esp_wifi_scan_get_ap_records(&num, records.data()) == ESP_OK) {
            results.reserve(num);
            for (uint16_t i = 0; i < num; i++) {
                ApInfo info;
                info.ssid = std::string(reinterpret_cast<const char *>(records[i].ssid));
                info.rssi = records[i].rssi;
                info.secure = (records[i].authmode != WIFI_AUTH_OPEN);
                if (!info.ssid.empty()) {
                    results.push_back(std::move(info));
                }
            }
        }
    }
    ESP_LOGI(TAG, "Scan complete: %d networks", (int)results.size());
    for (const auto &cb : s_scan_cbs) {
        cb(results);
    }
}

void eventHandler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_SCAN_DONE:
            handleScanDone();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "STA disconnected");
            s_state = (s_state == State::Connecting) ? State::Failed : State::Disconnected;
            s_ip.clear();
            notifyState();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        s_ip = ip_str;
        s_state = State::Connected;
        ESP_LOGI(TAG, "Got IP: %s", s_ip.c_str());
        notifyState();
    }
}

} // namespace

void init()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi manager initialized");
}

void autoConnect()
{
    std::string ssid, password;
    if (loadCredentials(ssid, password)) {
        ESP_LOGI(TAG, "Auto-connecting to saved network '%s'", ssid.c_str());
        connect(ssid, password);
    }
}

void startScan()
{
    if (s_scan_pending) {
        return;
    }
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan_config, false /* non-blocking */);
    if (err == ESP_OK) {
        s_scan_pending = true;
    } else {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
    }
}

void connect(const std::string &ssid, const std::string &password)
{
    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.password), password.c_str(),
                 sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    s_ssid = ssid;
    s_state = State::Connecting;
    notifyState();

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        s_state = State::Failed;
        notifyState();
        return;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        s_state = State::Failed;
        notifyState();
        return;
    }

    saveCredentials(ssid, password);
}

void forget()
{
    ESP_LOGI(TAG, "Forgetting saved network");
    eraseCredentials();
    esp_wifi_disconnect();
    s_ssid.clear();
    s_ip.clear();
    s_state = State::Disconnected;
    notifyState();
}

State state()
{
    return s_state;
}

std::string currentSsid()
{
    return s_ssid;
}

std::string savedSsid()
{
    std::string ssid, password;
    return loadCredentials(ssid, password) ? ssid : std::string();
}

std::string ipAddress()
{
    return s_ip;
}

int8_t rssi()
{
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        s_rssi = info.rssi;
    }
    return s_rssi;
}

void onScanResults(ScanResultCallback cb)
{
    s_scan_cbs.push_back(std::move(cb));
}

void onStateChange(StateChangeCallback cb)
{
    s_state_cbs.push_back(std::move(cb));
}

} // namespace wifi_manager
