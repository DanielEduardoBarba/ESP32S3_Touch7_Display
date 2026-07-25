#pragma once

/**
 * App-wide constants that don't belong to any single subsystem.
 *
 * This is the highest-level config header for the app firmware (alongside
 * ports_config.h for transport enables) -- add future global, cross-cutting
 * settings here rather than scattering them through individual modules.
 * Nothing in here depends on the board variant (7 vs 7B).
 */

// Bump this whenever the app's user-facing behavior changes. Shown in the
// header (next to the Home button), used by the peer version sync in the
// Update scene, and available to the web GUI/logs.
#define APP_VERSION "0.0.13"
// --- Development / production -------------------------------------------------
// 1 = development (default): on-screen FPS/CPU/RAM/PSRAM overlay card is
// shown (see firmware/common/dev_monitor.h). 0 = production: the overlay
// is compiled out entirely. Forced to 0 by `./build.sh ... --prod`
// (a compile definition set from the build scripts); `--dev` is the default.
#ifndef APP_DEV_MODE
#define APP_DEV_MODE 1
#endif
// --- Debug scene / log store -----------------------------------------------
// Every log line also goes into a RAM ring buffer for the Debug scene (the
// real console output is untouched). Oldest lines fall off past this cap.
// Kept deliberately small: the Debug scene is a quick glance at recent
// activity, not an exhaustive log (use the serial console for that).
#define APP_LOG_STORE_MAX        50
// How often the Debug scene refreshes its view of the ring buffer (ms).
#define APP_DEBUG_REFRESH_MS     500

// --- Peer link ---------------------------------------------------------------
// Baud used by RS485/UART peer links until the user picks another one in the
// Ports scene (both boards must match; changes are broadcast to stay in sync).
// Must be one of ports.cpp's EXACT-divisor rates (250000 = 80MHz/320).
#define APP_PEER_BAUD_DEFAULT    250000
// I2C link: fixed address each board writes the peer at.
#define APP_I2C_PEER_ADDR        0x42
// CAN link: identifier used for the byte-stream frames.
#define APP_CAN_MSG_ID           0x100
// Framing: payload ceiling for a request/response frame (bounds RX buffer).
#define APP_COMM_MAX_PAYLOAD     4096

// --- Bluetooth (BLE) ---------------------------------------------------------
// Advertised name prefix; the last two bytes of the board's MAC are appended
// so two boards on the same bench are distinguishable in a phone's list.
#define APP_BLE_NAME_PREFIX      "touch-esp32"
// How long a scan from the Bluetooth menu runs (seconds).
#define APP_BLE_SCAN_SECONDS     6

// --- Device-to-device firmware update ---------------------------------------
// Data bytes per CMD_DATA frame (fits within APP_COMM_MAX_PAYLOAD + headers).
// Bigger chunks = fewer stop-and-wait round trips = faster transfers.
#define APP_UPDATE_CHUNK_SIZE    2048
// How long the sender waits for each stop-and-wait ACK (ms).
#define APP_UPDATE_ACK_TIMEOUT_MS 3000
// How many times each frame (BEGIN/DATA/END) is retransmitted before the
// transfer is declared failed -- deliberately very generous: a chunk gets
// APP_UPDATE_MAX_RETRIES * APP_UPDATE_ACK_TIMEOUT_MS to make it across.
#define APP_UPDATE_MAX_RETRIES   10
// Receiver side: if no frame arrives for this long mid-transfer, the
// receive is aborted and the armed slot cleaned up (sender's full retry
// window + slack, so the receiver never gives up before the sender does).
#define APP_UPDATE_RX_STALL_MS   (APP_UPDATE_ACK_TIMEOUT_MS * (APP_UPDATE_MAX_RETRIES + 2))
// How long "Sync peer device" waits for a version reply before concluding
// the peer can't answer (e.g. it runs an older firmware without the
// version-sync feature). Push stays available; pull gets disabled.
#define APP_UPDATE_SYNC_TIMEOUT_MS 3000

// --- UI ----------------------------------------------------------------------
#define APP_UI_HEADER_HEIGHT     56

// --- Factory splash ------------------------------------------------------------
// The factory/recovery stage (firmware/factory) includes this header too:
// on every boot through it, it shows a silent splash for APP_SPLASH_SECONDS
// and then auto-boots the main app. Tapping the logo/text
// APP_SPLASH_TAPS_FOR_MENU times during the splash opens the recovery menu
// (switch/boot either OTA slot, erase settings) instead.
#define APP_SPLASH_BG_COLOR      0x101317   // full-screen background color
#define APP_SPLASH_SHOW_LOGO     0          // 1 = show APP_SPLASH_LOGO_IMG (an
                                            // lv_img_dsc_t linked into the
                                            // factory stage), 0 = show text
#define APP_SPLASH_TEXT          "Welcome!"
#define APP_SPLASH_TEXT_COLOR    0xe6edf3
#define APP_SPLASH_SECONDS       3
#define APP_SPLASH_TAPS_FOR_MENU 3          // rapid taps on the logo/text

// --- Web ---------------------------------------------------------------------
#define APP_WEB_SERVER_PORT      80
