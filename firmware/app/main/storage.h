#pragma once

namespace esp_panel::board { class Board; }

/**
 * Storage backend for the on-device web UI bundle. Always mounts the
 * internal `webapp` SPIFFS partition (populated at flash time from
 * web/dist, see firmware/app/CMakeLists.txt). Optionally mounts a TF/SD
 * card as an alternate source that takes priority if it contains a UI
 * bundle -- this is what lets you update the UI in the field by just
 * swapping the SD card, without re-flashing anything.
 */
namespace storage {

enum class WebRootSource {
    None,
    Spiffs,
    SdCard,
};

/** Mounts SPIFFS (always) and attempts to mount an SD card (best-effort). */
void init(esp_panel::board::Board *board);

WebRootSource webRootSource();

/** Base VFS path to serve the web UI from, e.g. "/webapp" or "/sdcard/www". */
const char *webRootPath();

bool sdCardPresent();

} // namespace storage
