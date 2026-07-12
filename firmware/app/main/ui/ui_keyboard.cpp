#include "ui_keyboard.h"

namespace ui_keyboard {
namespace {

lv_obj_t *s_keyboard = nullptr;

void keyboardEventCb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        hide();
    }
}

} // namespace

void init(lv_obj_t *parent)
{
    if (s_keyboard != nullptr) {
        return;
    }

    s_keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(s_keyboard, LV_PCT(100), LV_PCT(45));
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboardEventCb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_keyboard, keyboardEventCb, LV_EVENT_CANCEL, nullptr);
    lv_obj_move_foreground(s_keyboard);
}

void attach(lv_obj_t *textarea)
{
    if (s_keyboard == nullptr) {
        return;
    }
    lv_keyboard_set_textarea(s_keyboard, textarea);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

void hide()
{
    if (s_keyboard == nullptr) {
        return;
    }
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_keyboard, nullptr);
}

} // namespace ui_keyboard
