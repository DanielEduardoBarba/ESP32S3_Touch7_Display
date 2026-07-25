#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * Bluetooth Low Energy manager (NimBLE host).
 *
 * The ESP32-S3 radio does BLE only -- there is no Classic BT/A2DP on this
 * chip -- so "pairing" here means BLE bonding, and the board can act in
 * both directions:
 *
 *   - CENTRAL: scan for nearby devices, connect to one, and pair/bond with
 *     it (startScan() -> connect() -> the pairing flow below).
 *   - PERIPHERAL: make itself discoverable so a phone/laptop can find it
 *     in ITS scan list and initiate the pairing (setDiscoverable(true)).
 *
 * Pairing flow, whichever side starts it:
 *   1. A link is established (State::Connecting -> State::Connected).
 *   2. Security is initiated; NimBLE reports what user interaction the
 *      chosen association model needs. With the display we advertise
 *      DisplayYesNo capability, so the usual outcome is numeric
 *      comparison: both ends show a 6-digit number.
 *   3. The PasskeyCallback fires with that number; the UI shows it and the
 *      user answers with confirmPairing(true/false).
 *   4. On success the link is encrypted and the bond is stored in NVS, so
 *      later reconnects skip all of this (see forget() to drop a bond).
 *
 * Everything is asynchronous: NimBLE runs its own host task and all
 * callbacks below are invoked from it, never from the LVGL task -- UI code
 * must therefore take the LVGL lock (ui_bluetooth.cpp does).
 *
 * This is deliberately independent of the peer link: Bluetooth pairings are
 * local to a board, like display brightness and unlike Wi-Fi credentials.
 */
namespace ble_manager {

struct DeviceInfo {
    std::string name;   ///< advertised name, or empty if it advertises none
    std::string addr;   ///< "aa:bb:cc:dd:ee:ff", also the id used by connect()
    int8_t rssi = 0;
    bool bonded = false;
};

enum class State {
    Off,          ///< radio disabled (default until the user turns it on)
    Idle,         ///< on, not scanning or connected
    Scanning,
    Connecting,
    Connected,    ///< linked; `bonded` on the device says whether it is paired
    Failed,
};

using ScanResultCallback = std::function<void(const std::vector<DeviceInfo> &)>;
using StateChangeCallback = std::function<void(State state, const DeviceInfo &device,
                                               const std::string &detail)>;
/** A pairing needs the user's blessing: show `passkey` (6 digits) and call
 *  confirmPairing(). `numeric_comparison` is false when the peer only wants
 *  us to display the number for them to type. */
using PasskeyCallback = std::function<void(uint32_t passkey, bool numeric_comparison)>;

/** Restores the persisted on/off + discoverable preferences and, if
 *  Bluetooth was left on, brings the stack up. Call once at startup. */
void init();

bool enabled();
/** Turns the radio (and NimBLE host) on or off; persisted to NVS. */
void setEnabled(bool on);

/** Advertise so other devices can discover and pair with THIS board.
 *  Persisted, and re-applied whenever the stack comes up. */
void setDiscoverable(bool on);
bool discoverable();
/** The name this board advertises under. */
std::string deviceName();

/** Scan for `seconds`, then deliver results (also delivered incrementally
 *  as devices are found). No-op unless enabled. */
void startScan(uint32_t seconds = 6);
void stopScan();

/** Connect + pair with a device from the scan list (address form above). */
void connect(const std::string &addr);
void disconnect();

/** Answer the PasskeyCallback prompt. */
void confirmPairing(bool accept);

/** Drop a stored bond, so the device must pair from scratch next time. */
void forget(const std::string &addr);

State state();
DeviceInfo connectedDevice();

void onScanResults(ScanResultCallback cb);
void onStateChange(StateChangeCallback cb);
void onPasskeyRequest(PasskeyCallback cb);

} // namespace ble_manager
