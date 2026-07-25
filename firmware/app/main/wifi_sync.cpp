#include "wifi_sync.h"

#include <string>

#include "esp_log.h"

#include "comm_protocol.h"
#include "wifi_manager.h"

namespace wifi_sync {
namespace {

const char *TAG = "wifi_sync";

/** True while a connection attempt that CAME FROM the peer is in flight.
 *  Such a connection must not be announced back, or the two boards would
 *  hand the same credentials to each other forever. */
bool s_connecting_from_peer = false;

/** What we last put on the wire, so a reconnect to the same network (common
 *  after a router reboot) doesn't re-announce it every time. */
std::string s_last_shared_ssid;
std::string s_last_shared_pass;

void onWifiStateChange(wifi_manager::State state, const std::string &ssid, const std::string &ip)
{
    (void)ip;
    if (state != wifi_manager::State::Connected) {
        if (state == wifi_manager::State::Failed) {
            // The peer-supplied credentials didn't work here (different
            // radio range, wrong band, ...). Clear the flag so a later
            // local success is announced normally.
            s_connecting_from_peer = false;
        }
        return;
    }

    if (s_connecting_from_peer) {
        ESP_LOGI(TAG, "Joined '%s' using credentials from the peer -- not echoing them back",
                 ssid.c_str());
        s_connecting_from_peer = false;
        s_last_shared_ssid = ssid;
        s_last_shared_pass = wifi_manager::savedPassword();
        return;
    }

    std::string password = wifi_manager::savedPassword();
    if (ssid == s_last_shared_ssid && password == s_last_shared_pass) {
        return; // peer already has exactly these
    }
    s_last_shared_ssid = ssid;
    s_last_shared_pass = password;
    ESP_LOGI(TAG, "Connected to '%s' -- sharing credentials with the peer board", ssid.c_str());
    comm_protocol::sendWifiCredentials(ssid.c_str(), password.c_str());
}

void onCredentialsFromPeer(const char *ssid, const char *password)
{
    if (ssid == nullptr || *ssid == '\0') {
        return;
    }
    // Already on exactly this network with exactly these credentials?
    // Nothing to do -- and staying quiet here is what stops the exchange
    // from ping-ponging between the boards.
    if (wifi_manager::state() == wifi_manager::State::Connected &&
        wifi_manager::currentSsid() == ssid && wifi_manager::savedPassword() == password) {
        ESP_LOGI(TAG, "Peer shared '%s', already connected to it -- ignoring", ssid);
        return;
    }

    ESP_LOGI(TAG, "Peer shared credentials for '%s' -- joining the same network", ssid);
    s_connecting_from_peer = true;
    s_last_shared_ssid = ssid;
    s_last_shared_pass = password;
    // connect() persists the credentials itself, so this board will also
    // rejoin the shared network on its own after a reboot.
    wifi_manager::connect(ssid, password);
}

} // namespace

void init()
{
    wifi_manager::onStateChange(onWifiStateChange);
    comm_protocol::onWifiCredentialsReceived(onCredentialsFromPeer);
}

void shareNow()
{
    std::string ssid = wifi_manager::savedSsid();
    if (ssid.empty()) {
        ESP_LOGW(TAG, "No saved network to share");
        return;
    }
    std::string password = wifi_manager::savedPassword();
    s_last_shared_ssid = ssid;
    s_last_shared_pass = password;
    comm_protocol::sendWifiCredentials(ssid.c_str(), password.c_str());
}

} // namespace wifi_sync
