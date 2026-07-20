#include "ui_update.h"

#include <cstdio>

#include "app_config.h"
#include "../fw_update.h"
#include "lvgl_v8_port.h"

namespace ui_update {
namespace {

lv_obj_t *s_content = nullptr;
lv_obj_t *s_status_label = nullptr;
lv_obj_t *s_detail_label = nullptr;
lv_obj_t *s_progress_bar = nullptr;
lv_obj_t *s_sync_btn = nullptr;
lv_obj_t *s_sync_label = nullptr;
lv_obj_t *s_peer_label = nullptr;
lv_obj_t *s_push_btn = nullptr;
lv_obj_t *s_push_label = nullptr;
lv_obj_t *s_pull_btn = nullptr;
lv_obj_t *s_pull_label = nullptr;
lv_obj_t *s_reboot_btn = nullptr;

// Downgrade confirmation: the non-recommended direction needs a second tap.
// An armed confirmation auto-reverts after a few seconds (s_confirm_timer)
// so a stray tap can't leave a live "confirm" button behind.
bool s_push_needs_confirm = false;
bool s_pull_needs_confirm = false;
bool s_push_confirm_armed = false;
bool s_pull_confirm_armed = false;
lv_timer_t *s_confirm_timer = nullptr;
// Pull is unavailable when the peer never answered the version sync (its
// version can't be verified); keeps it disabled across transfer-state
// changes too (see setTransferButtonsEnabled).
bool s_pull_unavailable = false;
// Last peer info, so the confirm-revert timer can redraw the buttons.
fw_update::PeerInfo s_last_peer;

void refreshPeerUi(const fw_update::PeerInfo &peer);

/** "m:ss" (or "Ns" under a minute) into `out`. */
void formatDuration(char *out, size_t len, uint32_t seconds)
{
    if (seconds >= 60) {
        snprintf(out, len, "%lum %02lus", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
    } else {
        snprintf(out, len, "%lus", (unsigned long)seconds);
    }
}

void setTransferButtonsEnabled(bool enabled)
{
    lv_obj_t *btns[] = {s_sync_btn, s_push_btn, s_pull_btn};
    for (lv_obj_t *btn : btns) {
        if (enabled && !(btn == s_pull_btn && s_pull_unavailable)) {
            lv_obj_clear_state(btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(btn, LV_STATE_DISABLED);
        }
    }
}

/** Auto-revert an armed (red) downgrade confirmation after a few seconds. */
void confirmRevertTimerCb(lv_timer_t *t)
{
    lv_timer_pause(t);
    if (s_push_confirm_armed || s_pull_confirm_armed) {
        refreshPeerUi(s_last_peer); // resets armed flags + button styling
    }
}

void armConfirmTimer()
{
    if (s_confirm_timer != nullptr) {
        lv_timer_reset(s_confirm_timer);
        lv_timer_resume(s_confirm_timer);
    }
}

/** Re-labels/re-colors the push/pull buttons from the current peer info:
 *  the direction that moves FORWARD in version is green and one-press;
 *  the other (a downgrade) is grey and demands a confirmation tap. */
void refreshPeerUi(const fw_update::PeerInfo &peer)
{
    s_last_peer = peer;
    s_push_confirm_armed = false;
    s_pull_confirm_armed = false;
    s_pull_unavailable = false;
    lv_obj_clear_state(s_pull_btn, LV_STATE_DISABLED);

    // A sync round-trip finished (reply or timeout) -- restore the button.
    lv_label_set_text(s_sync_label, LV_SYMBOL_REFRESH "  Sync peer device");
    lv_obj_clear_state(s_sync_btn, LV_STATE_DISABLED);

    if (!peer.known) {
        if (peer.no_response) {
            // The peer never answered: it's either offline or runs an older
            // firmware that predates the version-sync feature. Pushing our
            // image is still fine (it's how such a peer gets updated);
            // pulling is disabled because there's no verified version to pull.
            lv_label_set_text(s_peer_label,
                              "Peer did not respond to the version sync -- it is offline or runs an "
                              "older firmware without this feature. You can still push your update; "
                              "\"Pull\" is disabled because the peer's version cannot be verified.");
            s_push_needs_confirm = false;
            s_pull_unavailable = true;
            lv_label_set_text(s_push_label, LV_SYMBOL_UPLOAD "  Push my update (v" APP_VERSION ")");
            lv_label_set_text(s_pull_label, LV_SYMBOL_DOWNLOAD "  Pull their update  (unavailable)");
            lv_obj_set_style_bg_color(s_push_btn, lv_color_hex(0x2ea043), 0);
            lv_obj_set_style_bg_color(s_pull_btn, lv_color_hex(0x1a1f26), 0);
            lv_obj_add_state(s_pull_btn, LV_STATE_DISABLED);
            lv_obj_clear_flag(s_push_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_pull_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_peer_label,
                              "Peer version unknown -- tap \"Sync peer device\" to compare versions "
                              "and unlock push/pull.");
            lv_obj_add_flag(s_push_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_pull_btn, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    const char *verdict;
    switch (peer.compare) {
    case fw_update::PeerCompare::PeerNewer: verdict = "you are OUT OF DATE"; break;
    case fw_update::PeerCompare::PeerOlder: verdict = "you are AHEAD"; break;
    default:                                verdict = "same version"; break;
    }
    lv_label_set_text_fmt(s_peer_label, "Peer: v%s  |  You: v" APP_VERSION "  --  %s",
                          peer.version.c_str(), verdict);

    bool push_recommended = peer.compare == fw_update::PeerCompare::PeerOlder;
    bool pull_recommended = peer.compare == fw_update::PeerCompare::PeerNewer;
    s_push_needs_confirm = !push_recommended;
    s_pull_needs_confirm = !pull_recommended;

    lv_label_set_text_fmt(s_push_label, LV_SYMBOL_UPLOAD "  Push my update (v" APP_VERSION ")%s",
                          push_recommended ? "" : peer.compare == fw_update::PeerCompare::Same
                                                       ? "  (same)" : "  (their downgrade)");
    lv_label_set_text_fmt(s_pull_label, LV_SYMBOL_DOWNLOAD "  Pull their update (v%s)%s",
                          peer.version.c_str(),
                          pull_recommended ? "" : peer.compare == fw_update::PeerCompare::Same
                                                       ? "  (same)" : "  (your downgrade)");
    lv_obj_set_style_bg_color(s_push_btn, lv_color_hex(push_recommended ? 0x2ea043 : 0x272e37), 0);
    lv_obj_set_style_bg_color(s_pull_btn, lv_color_hex(pull_recommended ? 0x2ea043 : 0x272e37), 0);
    lv_obj_clear_flag(s_push_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_pull_btn, LV_OBJ_FLAG_HIDDEN);
}

void syncBtnClickedCb(lv_event_t *e)
{
    // Immediate feedback: the button itself shows the round-trip is running
    // (refreshPeerUi restores it when the reply or the timeout arrives).
    lv_label_set_text(s_sync_label, LV_SYMBOL_REFRESH "  Syncing...");
    lv_obj_add_state(s_sync_btn, LV_STATE_DISABLED);
    lv_label_set_text(s_status_label, "Requesting peer version...");
    fw_update::syncPeer();
}

void pushBtnClickedCb(lv_event_t *e)
{
    if (s_pull_confirm_armed) {
        refreshPeerUi(s_last_peer); // tapping the other button cancels an armed confirm
        return;
    }
    if (s_push_needs_confirm && !s_push_confirm_armed) {
        s_push_confirm_armed = true;
        lv_label_set_text(s_push_label,
                          LV_SYMBOL_WARNING "  NOT an upgrade -- tap again to confirm");
        lv_obj_set_style_bg_color(s_push_btn, lv_color_hex(0xc0392b), 0);
        armConfirmTimer();
        return;
    }
    if (!fw_update::startSend()) {
        lv_label_set_text(s_status_label, "Cannot send: transfer busy or update pending reboot");
    }
}

void pullBtnClickedCb(lv_event_t *e)
{
    if (s_push_confirm_armed) {
        refreshPeerUi(s_last_peer); // tapping the other button cancels an armed confirm
        return;
    }
    if (s_pull_needs_confirm && !s_pull_confirm_armed) {
        s_pull_confirm_armed = true;
        lv_label_set_text(s_pull_label,
                          LV_SYMBOL_WARNING "  NOT an upgrade -- tap again to confirm");
        lv_obj_set_style_bg_color(s_pull_btn, lv_color_hex(0xc0392b), 0);
        armConfirmTimer();
        return;
    }
    if (!fw_update::startPull()) {
        lv_label_set_text(s_status_label, "Cannot pull: transfer busy or update pending reboot");
    }
}

void rebootBtnClickedCb(lv_event_t *e)
{
    lv_label_set_text(s_status_label, "Rebooting into the new firmware...");
    lv_refr_now(nullptr);
    fw_update::rebootIntoUpdate();
}

/** fw_update peer-version hook. Runs on the RX task, NOT the LVGL task. */
void onPeerInfo(const fw_update::PeerInfo &peer)
{
    lvgl_port_lock(-1);
    refreshPeerUi(peer);
    lvgl_port_unlock();
}

/** fw_update status hook. Runs on the transfer/RX task, NOT the LVGL task,
 *  so it must take the LVGL lock before touching widgets. */
void onUpdateStatus(const fw_update::Status &st)
{
    lvgl_port_lock(-1);

    lv_label_set_text(s_status_label, st.message.c_str());

    const bool has_transfer =
        st.total_bytes > 0 &&
        (st.state == fw_update::State::Sending || st.state == fw_update::State::Receiving ||
         st.state == fw_update::State::SendDone || st.state == fw_update::State::ReceiveDone);

    // Progress bar + detail line only exist while there is a transfer to
    // describe (keeps the idle scene clean).
    if (has_transfer) {
        lv_obj_clear_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_detail_label, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_progress_bar, (int32_t)(100.0f * st.done_bytes / st.total_bytes), LV_ANIM_OFF);

        char elapsed[16] = "0s";
        formatDuration(elapsed, sizeof(elapsed), st.elapsed_ms / 1000);

        char remaining[16] = "--";
        const bool running =
            st.state == fw_update::State::Sending || st.state == fw_update::State::Receiving;
        if (running && st.bytes_per_sec > 0 && st.total_bytes > st.done_bytes) {
            formatDuration(remaining, sizeof(remaining),
                           (st.total_bytes - st.done_bytes) / st.bytes_per_sec);
        } else if (!running) {
            snprintf(remaining, sizeof(remaining), "done");
        }

        char detail[160];
        snprintf(detail, sizeof(detail),
                 "%.1f%%  --  %lu / %lu bytes  --  %.1f KB/s\nElapsed: %s   Remaining: ~%s",
                 100.0f * st.done_bytes / st.total_bytes,
                 (unsigned long)st.done_bytes, (unsigned long)st.total_bytes,
                 st.bytes_per_sec / 1024.0f, elapsed, remaining);
        lv_label_set_text(s_detail_label, detail);
    } else {
        lv_obj_add_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_detail_label, LV_OBJ_FLAG_HIDDEN);
    }

    switch (st.state) {
    case fw_update::State::ReceiveDone:
        // This device received a verified update: hide everything except
        // the one action that matters now -- the red reboot button.
        setTransferButtonsEnabled(false);
        lv_obj_add_flag(s_push_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pull_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_reboot_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_peer_label,
                          "Update received and verified. Reboot when ready -- sending is "
                          "disabled until this device runs the new image.");
        break;
    case fw_update::State::Sending:
    case fw_update::State::Receiving:
        // Also locks Ports changes -- see ports::changeLocked().
        setTransferButtonsEnabled(false);
        break;
    case fw_update::State::SendDone:
    case fw_update::State::Failed:
    case fw_update::State::Idle:
        setTransferButtonsEnabled(true);
        break;
    }

    lvgl_port_unlock();
}

lv_obj_t *addInfoRow(lv_obj_t *parent, const char *key, const char *value)
{
    lv_obj_t *row = lv_label_create(parent);
    lv_label_set_text_fmt(row, "%s: %s", key, value);
    lv_obj_set_style_text_color(row, lv_color_hex(0x9aa4b2), 0);
    return row;
}

lv_obj_t *makeCard(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 520, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *makeActionButton(lv_obj_t *parent, lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), 52);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_center(label);
    if (label_out != nullptr) {
        *label_out = label;
    }
    return btn;
}

} // namespace

lv_obj_t *build(lv_obj_t *screen, lv_coord_t header_height)
{
    // Flex column: hidden widgets collapse, so every state of the flow
    // (idle / synced / transferring / received) lays out cleanly with no
    // gaps or overlaps. Scrolls if it outgrows the display.
    s_content = lv_obj_create(screen);
    lv_obj_set_size(s_content, LV_PCT(100), LV_VER_RES - header_height);
    lv_obj_align(s_content, LV_ALIGN_TOP_MID, 0, header_height);
    lv_obj_set_style_bg_color(s_content, lv_color_hex(0x101317), 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 24, 0);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_content, 14, 0);

    lv_obj_t *title = lv_label_create(s_content);
    lv_label_set_text(title, "Update");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    // --- App/OTA info card --------------------------------------------------
    fw_update::AppInfo info = fw_update::appInfo();
    lv_obj_t *card = makeCard(s_content);
    lv_obj_set_style_pad_row(card, 6, 0);

    char size_text[32];
    snprintf(size_text, sizeof(size_text), "%lu bytes", (unsigned long)info.image_size);
    addInfoRow(card, "Running slot", info.running_slot.c_str());
    addInfoRow(card, "App version", info.app_version.c_str());
    addInfoRow(card, "Build", info.version.c_str());
    addInfoRow(card, "Image size", size_text);
    addInfoRow(card, "OTA state", info.ota_state.c_str());
    addInfoRow(card, "ESP-IDF", info.idf_version.c_str());
    addInfoRow(card, "Compiled", info.compile_time.c_str());

    // --- Peer update card: sync -> compare -> push/pull -> progress ---------
    lv_obj_t *peer_card = makeCard(s_content);

    lv_obj_t *peer_title = lv_label_create(peer_card);
    lv_label_set_text(peer_title, "Peer update");
    lv_obj_set_style_text_font(peer_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(peer_title, lv_color_white(), 0);

    s_sync_btn = makeActionButton(peer_card, syncBtnClickedCb, &s_sync_label);

    s_peer_label = lv_label_create(peer_card);
    lv_obj_set_style_text_color(s_peer_label, lv_color_hex(0x9aa4b2), 0);
    lv_obj_set_width(s_peer_label, LV_PCT(100));
    lv_label_set_long_mode(s_peer_label, LV_LABEL_LONG_WRAP);

    s_push_btn = makeActionButton(peer_card, pushBtnClickedCb, &s_push_label);
    lv_obj_add_flag(s_push_btn, LV_OBJ_FLAG_HIDDEN);

    s_pull_btn = makeActionButton(peer_card, pullBtnClickedCb, &s_pull_label);
    lv_obj_add_flag(s_pull_btn, LV_OBJ_FLAG_HIDDEN);

    // Red reboot button: hidden until an update has been received+verified.
    s_reboot_btn = makeActionButton(peer_card, rebootBtnClickedCb, nullptr);
    lv_obj_set_style_bg_color(s_reboot_btn, lv_color_hex(0xc0392b), 0);
    lv_obj_add_flag(s_reboot_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *reboot_label = lv_obj_get_child(s_reboot_btn, 0);
    lv_label_set_text(reboot_label, LV_SYMBOL_POWER "  Reboot into update");

    s_progress_bar = lv_bar_create(peer_card);
    lv_obj_set_size(s_progress_bar, LV_PCT(100), 14);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_obj_add_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);

    // Percent / bytes / speed / time line, updated live during transfers.
    s_detail_label = lv_label_create(peer_card);
    lv_label_set_text(s_detail_label, "");
    lv_obj_set_style_text_color(s_detail_label, lv_color_hex(0x9aa4b2), 0);
    lv_obj_set_width(s_detail_label, LV_PCT(100));
    lv_label_set_long_mode(s_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_detail_label, LV_OBJ_FLAG_HIDDEN);

    s_status_label = lv_label_create(peer_card);
    lv_label_set_text(s_status_label, "Idle");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xf0a500), 0);
    lv_obj_set_width(s_status_label, LV_PCT(100));
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);

    s_confirm_timer = lv_timer_create(confirmRevertTimerCb, 5000, nullptr);
    lv_timer_pause(s_confirm_timer);

    refreshPeerUi(fw_update::peerInfo());
    fw_update::onStatusChange(onUpdateStatus);
    fw_update::onPeerInfoChange(onPeerInfo);
    return s_content;
}

} // namespace ui_update
