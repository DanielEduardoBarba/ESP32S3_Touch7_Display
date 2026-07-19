#include "ui_update.h"

#include "../fw_update.h"
#include "lvgl_v8_port.h"

namespace ui_update {
namespace {

lv_obj_t *s_content = nullptr;
lv_obj_t *s_status_label = nullptr;
lv_obj_t *s_detail_label = nullptr;
lv_obj_t *s_progress_bar = nullptr;
lv_obj_t *s_update_btn = nullptr;
lv_obj_t *s_reboot_btn = nullptr;

void updateBtnClickedCb(lv_event_t *e)
{
    if (!fw_update::startSend()) {
        lv_label_set_text(s_status_label, "Cannot send: transfer busy or update pending reboot");
    }
}

void rebootBtnClickedCb(lv_event_t *e)
{
    lv_label_set_text(s_status_label, "Rebooting into the new firmware...");
    lv_refr_now(nullptr);
    fw_update::rebootIntoUpdate();
}

/** "m:ss" (or "Ns" under a minute) into `out`. */
void formatDuration(char *out, size_t len, uint32_t seconds)
{
    if (seconds >= 60) {
        snprintf(out, len, "%lum %02lus", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
    } else {
        snprintf(out, len, "%lus", (unsigned long)seconds);
    }
}

/** fw_update status hook. Runs on the transfer/RX task, NOT the LVGL task,
 *  so it must take the LVGL lock before touching widgets. */
void onUpdateStatus(const fw_update::Status &st)
{
    lvgl_port_lock(-1);

    lv_label_set_text(s_status_label, st.message.c_str());
    if (st.total_bytes > 0) {
        lv_bar_set_value(s_progress_bar, (int32_t)(100.0f * st.done_bytes / st.total_bytes), LV_ANIM_OFF);
    }

    // Percent (1 decimal), bytes of total, speed, elapsed and remaining time.
    if (st.total_bytes > 0 &&
        (st.state == fw_update::State::Sending || st.state == fw_update::State::Receiving ||
         st.state == fw_update::State::SendDone || st.state == fw_update::State::ReceiveDone)) {
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
        lv_label_set_text(s_detail_label, "");
    }

    switch (st.state) {
    case fw_update::State::ReceiveDone:
        // This device received an update: its own "send" ability is
        // disabled (per product requirement) and the red reboot button
        // appears until the user restarts into the new image.
        lv_obj_add_state(s_update_btn, LV_STATE_DISABLED);
        lv_obj_clear_flag(s_reboot_btn, LV_OBJ_FLAG_HIDDEN);
        break;
    case fw_update::State::Sending:
    case fw_update::State::Receiving:
        lv_obj_add_state(s_update_btn, LV_STATE_DISABLED);
        break;
    case fw_update::State::SendDone:
    case fw_update::State::Failed:
    case fw_update::State::Idle:
        lv_obj_clear_state(s_update_btn, LV_STATE_DISABLED);
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

} // namespace

lv_obj_t *build(lv_obj_t *screen, lv_coord_t header_height)
{
    s_content = lv_obj_create(screen);
    lv_obj_set_size(s_content, LV_PCT(100), LV_VER_RES - header_height);
    lv_obj_align(s_content, LV_ALIGN_TOP_MID, 0, header_height);
    lv_obj_set_style_bg_color(s_content, lv_color_hex(0x101317), 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 24, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_content);
    lv_label_set_text(title, "Update");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    // --- App/OTA info card ------------------------------------------------
    fw_update::AppInfo info = fw_update::appInfo();

    lv_obj_t *card = lv_obj_create(s_content);
    lv_obj_set_size(card, 480, LV_SIZE_CONTENT);
    lv_obj_align_to(card, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1c2128), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    char size_text[32];
    snprintf(size_text, sizeof(size_text), "%lu bytes", (unsigned long)info.image_size);
    addInfoRow(card, "Running slot", info.running_slot.c_str());
    addInfoRow(card, "App version", info.version.c_str());
    addInfoRow(card, "Image size", size_text);
    addInfoRow(card, "OTA state", info.ota_state.c_str());
    addInfoRow(card, "ESP-IDF", info.idf_version.c_str());
    addInfoRow(card, "Compiled", info.compile_time.c_str());

    // --- Transfer controls --------------------------------------------------
    s_update_btn = lv_btn_create(s_content);
    lv_obj_set_size(s_update_btn, 260, 56);
    lv_obj_align_to(s_update_btn, card, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
    lv_obj_add_event_cb(s_update_btn, updateBtnClickedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *update_label = lv_label_create(s_update_btn);
    lv_label_set_text(update_label, "Update peer device");
    lv_obj_center(update_label);

    // Red reboot button: hidden until an update has been received+verified.
    s_reboot_btn = lv_btn_create(s_content);
    lv_obj_set_size(s_reboot_btn, 260, 56);
    lv_obj_align_to(s_reboot_btn, s_update_btn, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
    lv_obj_set_style_bg_color(s_reboot_btn, lv_color_hex(0xc0392b), 0);
    lv_obj_add_flag(s_reboot_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_reboot_btn, rebootBtnClickedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *reboot_label = lv_label_create(s_reboot_btn);
    lv_label_set_text(reboot_label, "Reboot into update");
    lv_obj_center(reboot_label);

    s_progress_bar = lv_bar_create(s_content);
    lv_obj_set_size(s_progress_bar, 480, 14);
    lv_obj_align_to(s_progress_bar, s_reboot_btn, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 18);
    lv_bar_set_range(s_progress_bar, 0, 100);

    // Percent / bytes / speed / time line, updated live during transfers.
    s_detail_label = lv_label_create(s_content);
    lv_label_set_text(s_detail_label, "");
    lv_obj_set_style_text_color(s_detail_label, lv_color_hex(0x9aa4b2), 0);
    lv_obj_set_width(s_detail_label, 480);
    lv_label_set_long_mode(s_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(s_detail_label, s_progress_bar, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);

    s_status_label = lv_label_create(s_content);
    lv_label_set_text(s_status_label, "Idle");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xf0a500), 0);
    lv_obj_set_width(s_status_label, 480);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(s_status_label, s_detail_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);

    fw_update::onStatusChange(onUpdateStatus);
    return s_content;
}

} // namespace ui_update
