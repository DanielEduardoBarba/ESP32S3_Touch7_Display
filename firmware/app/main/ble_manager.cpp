#include "ble_manager.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "app_config.h"
#include "user_store.h"

extern "C" void ble_store_config_init(void);

namespace ble_manager {
namespace {

const char *TAG = "ble";

constexpr const char *STORE_KEY_ENABLED = "bt_enabled";
constexpr const char *STORE_KEY_DISCOVER = "bt_discover";
constexpr uint8_t STORE_VERSION = 1;

constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;

// A busy room produces hundreds of advertisement packets per second, and
// every notification rebuilds the on-screen list (allocations + LVGL lock)
// on the NimBLE host task. Unthrottled, that starves the idle task and the
// task watchdog fires. Redraw at most this often; the scan-complete event
// always delivers a final, exact list.
constexpr int64_t SCAN_NOTIFY_INTERVAL_US = 500000;
// Signal wobble of a few dB is meaningless here and would force a redraw on
// every packet, so only a real change in level counts as "changed".
constexpr int8_t RSSI_SIGNIFICANT_CHANGE_DB = 6;

int64_t s_last_scan_notify_us = 0;

bool s_enabled = false;
bool s_discoverable = false;
bool s_host_running = false;
bool s_synced = false;          ///< host/controller ready (BLE_HS sync done)
uint8_t s_own_addr_type = 0;
uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
State s_state = State::Off;
DeviceInfo s_connected;
std::string s_device_name;

/** Set while a scan is running so DISC_COMPLETE knows it was ours. */
bool s_scanning = false;

std::mutex s_mutex;                      ///< guards s_found (NimBLE task vs UI task)
std::map<std::string, DeviceInfo> s_found;

std::vector<ScanResultCallback> s_scan_cbs;
std::vector<StateChangeCallback> s_state_cbs;
std::vector<PasskeyCallback> s_passkey_cbs;

int gapEvent(struct ble_gap_event *event, void *arg);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string addrToString(const ble_addr_t &addr)
{
    char buf[18];
    // NimBLE stores addresses little-endian; print them the way every phone
    // and every BLE tool shows them (most significant byte first).
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", addr.val[5], addr.val[4],
                  addr.val[3], addr.val[2], addr.val[1], addr.val[0]);
    return buf;
}

bool stringToAddr(const std::string &text, ble_addr_t &out)
{
    unsigned v[6];
    if (std::sscanf(text.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &v[0], &v[1], &v[2], &v[3],
                    &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out.val[5 - i] = static_cast<uint8_t>(v[i]);
    }
    out.type = BLE_ADDR_PUBLIC;
    return true;
}

/** True if we already hold a bond for this address (either address type). */
bool isBonded(const std::string &addr_str)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;
    if (ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS) != 0) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (addrToString(peers[i]) == addr_str) {
            return true;
        }
    }
    return false;
}

void notifyState(const std::string &detail = "")
{
    for (const auto &cb : s_state_cbs) {
        cb(s_state, s_connected, detail);
    }
}

void setState(State state, const std::string &detail = "")
{
    s_state = state;
    notifyState(detail);
}

void notifyScanResults(bool force = true)
{
    if (!force) {
        int64_t now = esp_timer_get_time();
        if (now - s_last_scan_notify_us < SCAN_NOTIFY_INTERVAL_US) {
            return;
        }
        s_last_scan_notify_us = now;
    } else {
        s_last_scan_notify_us = esp_timer_get_time();
    }

    std::vector<DeviceInfo> list;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        list.reserve(s_found.size());
        for (const auto &entry : s_found) {
            list.push_back(entry.second);
        }
    }
    // Strongest signal first: the device the user means is usually the one
    // in their hand, right next to the panel.
    std::sort(list.begin(), list.end(),
              [](const DeviceInfo &a, const DeviceInfo &b) { return a.rssi > b.rssi; });
    for (const auto &cb : s_scan_cbs) {
        cb(list);
    }
}

// ---------------------------------------------------------------------------
// Advertising (peripheral role: "discoverable")
// ---------------------------------------------------------------------------

void startAdvertising()
{
    if (!s_synced || !s_discoverable) {
        return;
    }
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = reinterpret_cast<const uint8_t *>(s_device_name.c_str());
    fields.name_len = static_cast<uint8_t>(s_device_name.size());
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;  // connectable
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;  // generally discoverable

    rc = ble_gap_adv_start(s_own_addr_type, nullptr, BLE_HS_FOREVER, &adv_params, gapEvent, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "Discoverable as '%s'", s_device_name.c_str());
}

void stopAdvertising()
{
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
}

// ---------------------------------------------------------------------------
// GAP events
// ---------------------------------------------------------------------------

void handleDiscovery(const struct ble_gap_disc_desc &disc)
{
    struct ble_hs_adv_fields fields = {};
    ble_hs_adv_parse_fields(&fields, disc.data, disc.length_data);

    DeviceInfo info;
    info.addr = addrToString(disc.addr);
    info.rssi = disc.rssi;
    if (fields.name != nullptr && fields.name_len > 0) {
        info.name.assign(reinterpret_cast<const char *>(fields.name), fields.name_len);
    }
    info.bonded = isBonded(info.addr);

    bool changed = false;
    bool is_new = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_found.find(info.addr);
        if (it == s_found.end()) {
            s_found.emplace(info.addr, info);
            changed = true;
            is_new = true;
        } else {
            // Later packets often carry the name the first one lacked.
            if (it->second.name.empty() && !info.name.empty()) {
                it->second.name = info.name;
                changed = true;
            }
            if (std::abs(it->second.rssi - info.rssi) >= RSSI_SIGNIFICANT_CHANGE_DB) {
                it->second.rssi = info.rssi;
                changed = true;
            }
        }
    }
    if (is_new) {
        ESP_LOGI(TAG, "Found %s [%s] rssi=%d%s",
                 info.name.empty() ? "(no name)" : info.name.c_str(), info.addr.c_str(),
                 (int)info.rssi, info.bonded ? " (bonded)" : "");
    }
    if (changed) {
        notifyScanResults(/*force=*/false);
    }
}

void handlePasskeyAction(struct ble_gap_event *event)
{
    struct ble_sm_io io = {};
    switch (event->passkey.params.action) {
    case BLE_SM_IOACT_NUMCMP:
        // Both ends show the same number; the user confirms they match.
        // The answer arrives later via confirmPairing().
        ESP_LOGI(TAG, "Pairing: confirm passkey %06" PRIu32, event->passkey.params.numcmp);
        for (const auto &cb : s_passkey_cbs) {
            cb(event->passkey.params.numcmp, /*numeric_comparison=*/true);
        }
        break;

    case BLE_SM_IOACT_DISP: {
        // We pick a passkey and show it; the peer types it in.
        io.action = BLE_SM_IOACT_DISP;
        io.passkey = esp_random() % 1000000;
        ESP_LOGI(TAG, "Pairing: enter %06" PRIu32 " on the other device", io.passkey);
        for (const auto &cb : s_passkey_cbs) {
            cb(io.passkey, /*numeric_comparison=*/false);
        }
        ble_sm_inject_io(event->passkey.conn_handle, &io);
        break;
    }

    case BLE_SM_IOACT_OOB:
    case BLE_SM_IOACT_INPUT:
        // Neither is possible with a touchscreen-only association model we
        // advertise; let the stack fall back / fail cleanly.
        ESP_LOGW(TAG, "Pairing needs an unsupported action (%d)", event->passkey.params.action);
        break;

    default:
        break;
    }
}

int gapEvent(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handleDiscovery(event->disc);
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        ESP_LOGI(TAG, "Scan finished (%u device(s))", (unsigned)s_found.size());
        notifyScanResults();
        if (s_state == State::Scanning) {
            setState(State::Idle);
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn_handle, &desc) == 0) {
                s_connected.addr = addrToString(desc.peer_id_addr);
                std::lock_guard<std::mutex> lock(s_mutex);
                auto it = s_found.find(s_connected.addr);
                if (it != s_found.end()) {
                    s_connected.name = it->second.name;
                    s_connected.rssi = it->second.rssi;
                }
            }
            s_connected.bonded = isBonded(s_connected.addr);
            ESP_LOGI(TAG, "Connected to %s", s_connected.addr.c_str());
            setState(State::Connected, "connected");
            if (!s_connected.bonded) {
                // Not paired yet -- ask for encryption, which kicks off the
                // pairing exchange (and the passkey prompt above).
                int rc = ble_gap_security_initiate(s_conn_handle);
                if (rc != 0 && rc != BLE_HS_EALREADY) {
                    ESP_LOGW(TAG, "security_initiate failed: %d", rc);
                }
            }
        } else {
            ESP_LOGW(TAG, "Connect failed: status %d", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            setState(State::Failed, "connection failed");
            startAdvertising(); // advertising stops on connect attempts
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected (reason 0x%x)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_connected = DeviceInfo{};
        setState(s_enabled ? State::Idle : State::Off, "disconnected");
        startAdvertising();
        break;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        if (event->enc_change.status == 0 && ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            s_connected.bonded = desc.sec_state.bonded;
            ESP_LOGI(TAG, "Link encrypted (bonded=%d)", (int)desc.sec_state.bonded);
            setState(State::Connected, desc.sec_state.bonded ? "paired" : "encrypted");
        } else {
            ESP_LOGW(TAG, "Pairing failed: status %d", event->enc_change.status);
            setState(State::Connected, "pairing failed");
        }
        break;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        handlePasskeyAction(event);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        startAdvertising(); // keep advertising if it timed out for any reason
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // The peer wants to pair again while we still hold an old bond:
        // drop the stale one and let the new pairing continue.
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Host bring-up / teardown
// ---------------------------------------------------------------------------

void onSync()
{
    // Pick the address type the controller can actually use.
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto failed: %d", rc);
        return;
    }
    s_synced = true;
    ESP_LOGI(TAG, "BLE host synced, advertising as '%s'", s_device_name.c_str());
    setState(State::Idle, "ready");
    startAdvertising();
}

void onReset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason %d", reason);
    s_synced = false;
}

void hostTask(void *)
{
    nimble_port_run(); // returns only when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

void startHost()
{
    if (s_host_running) {
        return;
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s -- Bluetooth unavailable",
                 esp_err_to_name(err));
        s_enabled = false;
        setState(State::Off, "radio unavailable");
        return;
    }

    ble_hs_cfg.reset_cb = onReset;
    ble_hs_cfg.sync_cb = onSync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // A touchscreen can show a number and take a yes/no -- that maps to the
    // numeric-comparison association model, which is the one that actually
    // protects against man-in-the-middle (unlike Just Works).
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_device_name.c_str());
    ble_store_config_init(); // bonds persist in NVS

    nimble_port_freertos_init(hostTask);
    s_host_running = true;
}

void stopHost()
{
    if (!s_host_running) {
        return;
    }
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    stopAdvertising();
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }
    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
    }
    s_host_running = false;
    s_synced = false;
    s_connected = DeviceInfo{};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_found.clear();
    }
    notifyScanResults();
}

} // namespace

void init()
{
    // Advertised name: product name + the tail of the MAC, so two boards on
    // the same bench are told apart in a phone's Bluetooth list.
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_BT);
    char name[32];
    std::snprintf(name, sizeof(name), "%s-%02x%02x", APP_BLE_NAME_PREFIX, mac[4], mac[5]);
    s_device_name = name;

    uint8_t version = 0;
    uint8_t stored = 0;
    if (user_store::getU8(STORE_KEY_ENABLED, &version, &stored) && version == STORE_VERSION) {
        s_enabled = stored != 0;
    }
    if (user_store::getU8(STORE_KEY_DISCOVER, &version, &stored) && version == STORE_VERSION) {
        s_discoverable = stored != 0;
    }

    if (s_enabled) {
        ESP_LOGI(TAG, "Bluetooth was left on -- starting NimBLE host");
        startHost();
    } else {
        ESP_LOGI(TAG, "Bluetooth off (turn it on from the header's Bluetooth menu)");
        s_state = State::Off;
    }
}

bool enabled()
{
    return s_enabled;
}

void setEnabled(bool on)
{
    if (on == s_enabled) {
        return;
    }
    s_enabled = on;
    user_store::putU8(STORE_KEY_ENABLED, STORE_VERSION, on ? 1 : 0);
    if (on) {
        startHost();
    } else {
        stopHost();
        setState(State::Off, "turned off");
    }
}

void setDiscoverable(bool on)
{
    s_discoverable = on;
    user_store::putU8(STORE_KEY_DISCOVER, STORE_VERSION, on ? 1 : 0);
    if (!s_host_running) {
        return;
    }
    if (on) {
        startAdvertising();
    } else {
        stopAdvertising();
        ESP_LOGI(TAG, "No longer discoverable");
    }
}

bool discoverable()
{
    return s_discoverable;
}

std::string deviceName()
{
    return s_device_name;
}

void startScan(uint32_t seconds)
{
    if (!s_enabled || !s_synced) {
        ESP_LOGW(TAG, "Scan requested while Bluetooth is not ready");
        return;
    }
    if (s_scanning) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_found.clear();
    }
    notifyScanResults();

    struct ble_gap_disc_params params = {};
    params.passive = 0;           // active scan: also request scan responses (names)
    params.filter_duplicates = 0; // we de-duplicate ourselves, and want RSSI updates
    int rc = ble_gap_disc(s_own_addr_type, seconds * 1000, &params, gapEvent, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        return;
    }
    s_scanning = true;
    setState(State::Scanning, "scanning");
}

void stopScan()
{
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
        if (s_state == State::Scanning) {
            setState(State::Idle);
        }
    }
}

void connect(const std::string &addr)
{
    if (!s_enabled || !s_synced) {
        return;
    }
    ble_addr_t target;
    {
        // Use the address type we actually saw in the advertisement --
        // random static addresses are the norm for phones, and connecting
        // with the wrong type simply times out.
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_found.find(addr);
        if (it == s_found.end()) {
            ESP_LOGW(TAG, "Unknown device %s (scan again)", addr.c_str());
            return;
        }
        s_connected = it->second;
    }
    if (!stringToAddr(addr, target)) {
        return;
    }
    // Scanning and connecting cannot overlap.
    stopScan();

    // Try the address exactly as advertised; NimBLE keeps the type it saw
    // in its own cache, so both public and random peers work.
    target.type = BLE_ADDR_PUBLIC;
    ble_addr_t random_target = target;
    random_target.type = BLE_ADDR_RANDOM;

    setState(State::Connecting, "connecting");
    int rc = ble_gap_connect(s_own_addr_type, &target, CONNECT_TIMEOUT_MS, nullptr, gapEvent,
                             nullptr);
    if (rc != 0) {
        rc = ble_gap_connect(s_own_addr_type, &random_target, CONNECT_TIMEOUT_MS, nullptr,
                             gapEvent, nullptr);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
        setState(State::Failed, "could not start connection");
    }
}

void disconnect()
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void confirmPairing(bool accept)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct ble_sm_io io = {};
    io.action = BLE_SM_IOACT_NUMCMP;
    io.numcmp_accept = accept ? 1 : 0;
    int rc = ble_sm_inject_io(s_conn_handle, &io);
    ESP_LOGI(TAG, "Pairing %s (inject rc=%d)", accept ? "accepted" : "rejected", rc);
    if (!accept) {
        disconnect();
    }
}

void forget(const std::string &addr)
{
    ble_addr_t target;
    if (!stringToAddr(addr, target)) {
        return;
    }
    for (uint8_t type : {BLE_ADDR_PUBLIC, BLE_ADDR_RANDOM}) {
        target.type = type;
        ble_store_util_delete_peer(&target);
    }
    ESP_LOGI(TAG, "Dropped bond for %s", addr.c_str());
    if (s_connected.addr == addr) {
        s_connected.bonded = false;
        disconnect();
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_found.find(addr);
        if (it != s_found.end()) {
            it->second.bonded = false;
        }
    }
    notifyScanResults();
}

State state()
{
    return s_state;
}

DeviceInfo connectedDevice()
{
    return s_connected;
}

void onScanResults(ScanResultCallback cb)
{
    s_scan_cbs.push_back(std::move(cb));
}

void onStateChange(StateChangeCallback cb)
{
    s_state_cbs.push_back(std::move(cb));
}

void onPasskeyRequest(PasskeyCallback cb)
{
    s_passkey_cbs.push_back(std::move(cb));
}

} // namespace ble_manager
