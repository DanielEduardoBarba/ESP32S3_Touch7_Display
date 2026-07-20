#pragma once

#include <cstdint>
#include <functional>
#include <string>

/**
 * Device-to-device firmware update ("pipe an update from one running app
 * to the other") over the active ports transport, using comm_protocol's
 * TYPE_UPDATE frames with stop-and-wait acknowledgement:
 *
 *   sender                                receiver
 *   ------                                --------
 *   CMD_BEGIN {size, crc16}   ---->       esp_ota_begin on inactive slot
 *                             <----       CMD_ACK
 *   CMD_DATA {offset, bytes}  ---->       esp_ota_write
 *                             <----       CMD_ACK          (repeat...)
 *   CMD_END                   ---->       whole-image CRC16 check +
 *                                         esp_ota_end (structure check) +
 *                                         esp_ota_set_boot_partition
 *                             <----       CMD_RESULT {ok}
 *
 * The whole-image CRC16 (computed over every byte of the image on both
 * sides) means a corrupted transfer can never be armed for boot; on top of
 * that the standard rollback flow (boot_health.cpp) protects against an
 * image that transfers intact but fails to run. After a successful receive
 * the device asks the user to reboot (red button in the Update scene) and
 * its own "send update" ability is disabled until then.
 */
namespace fw_update {

enum class State : uint8_t {
    Idle,
    Sending,
    Receiving,
    SendDone,
    ReceiveDone,   // verified + armed for boot; reboot pending
    Failed,
};

struct Status {
    State state = State::Idle;
    uint32_t total_bytes = 0;
    uint32_t done_bytes = 0;
    uint32_t bytes_per_sec = 0;   // average transfer speed (0 until measurable)
    uint32_t elapsed_ms = 0;      // time since the transfer started
    std::string message;   // human text for GUIs/logs ("CRC OK", errors, ...)
};

/** Info about the running image, for the Update scene / web GUI. */
struct AppInfo {
    std::string running_slot;    // "factory", "ota_0", "ota_1"
    std::string app_version;     // APP_VERSION (semver, used for peer sync)
    std::string version;         // project git describe
    std::string idf_version;
    std::string compile_time;
    uint32_t image_size = 0;     // actual image bytes (not partition size)
    std::string ota_state;       // valid / pending verify / ...
};

/** How the peer's APP_VERSION compares to OURS after a version sync. */
enum class PeerCompare : uint8_t {
    Unknown,     // no sync yet / peer never answered
    PeerNewer,   // peer is ahead -> we are OUT OF DATE (pull recommended)
    Same,        // identical versions
    PeerOlder,   // peer is behind -> we are AHEAD (push recommended)
};

struct PeerInfo {
    bool known = false;
    bool no_response = false;        // last sync timed out: peer never answered
                                     // (likely an older firmware without the
                                     // version-sync feature) -- push is still
                                     // possible, pull is not
    std::string version;             // the peer's APP_VERSION
    PeerCompare compare = PeerCompare::Unknown;
};

using StatusCallback = std::function<void(const Status &)>;
using PeerInfoCallback = std::function<void(const PeerInfo &)>;

/** Registers the TYPE_UPDATE frame handler. Call once after comm_protocol::init(). */
void init();

AppInfo appInfo();
Status status();

/** Broadcasts our APP_VERSION and asks the peer for theirs ("Sync peer
 *  device"). Results arrive via onPeerInfoChange() on BOTH devices. */
void syncPeer();

/** Latest known peer version/comparison (known=false before any sync). */
PeerInfo peerInfo();

/** Streams this device's RUNNING app image to the peer (background task).
 *  Returns false if a transfer is already running or the device itself has
 *  a pending received update. */
bool startSend();

/** Asks the peer to stream ITS image to us ("Pull their update"). Same
 *  busy guards as startSend(). */
bool startPull();

/** After a successful receive: restart into the new image. */
void rebootIntoUpdate();

/** GUI/web hook: fired on every state/progress change (from the transfer
 *  task -- UI layers must lock LVGL themselves). May be called multiple
 *  times to register multiple observers. */
void onStatusChange(StatusCallback cb);

/** GUI/web hook: fired whenever the peer's version/comparison is (re)learned
 *  (from the RX task -- UI layers must lock LVGL themselves). */
void onPeerInfoChange(PeerInfoCallback cb);

} // namespace fw_update
