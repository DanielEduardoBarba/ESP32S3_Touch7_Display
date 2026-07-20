#include "ui_scene_manager.h"

#include "esp_log.h"

namespace ui_scene_manager {
namespace {

const char *TAG = "ui_scenes";

struct Entry {
    Builder build = nullptr;
    Destructor destroy = nullptr;
    lv_obj_t *root = nullptr;
    bool persistent = false;
};

lv_obj_t *s_screen = nullptr;
lv_coord_t s_header_height = 0;
Entry s_entries[static_cast<size_t>(Scene::_Count)] = {};

} // namespace

void init(lv_obj_t *screen, lv_coord_t header_height)
{
    s_screen = screen;
    s_header_height = header_height;
}

void registerScene(Scene scene, Builder build, Destructor destroy, bool persistent)
{
    Entry &e = s_entries[static_cast<size_t>(scene)];
    e.build = build;
    e.destroy = destroy;
    e.persistent = persistent;
    if (persistent) {
        // Built up front and kept alive forever: its widgets/callbacks keep
        // working in the background (e.g. an update arriving while the user
        // is on another scene).
        e.root = e.build(s_screen, s_header_height);
        lv_obj_add_flag(e.root, LV_OBJ_FLAG_HIDDEN);
    }
}

void show(Scene scene)
{
    const size_t target = static_cast<size_t>(scene);

    // Tear down every OTHER non-persistent scene first, freeing its whole
    // widget tree; underlying data lives in the data modules (machine
    // state, NVS-backed settings...), so rebuilding later loses nothing.
    for (size_t i = 0; i < static_cast<size_t>(Scene::_Count); i++) {
        Entry &e = s_entries[i];
        if (i == target || e.root == nullptr) {
            continue;
        }
        if (e.persistent) {
            lv_obj_add_flag(e.root, LV_OBJ_FLAG_HIDDEN);
        } else {
            e.destroy(); // deletes the root + nulls the module's statics
            e.root = nullptr;
            ESP_LOGD(TAG, "Scene %u destroyed (widgets freed)", (unsigned)i);
        }
    }

    Entry &e = s_entries[target];
    if (e.root == nullptr && e.build != nullptr) {
        e.root = e.build(s_screen, s_header_height);
    }
    if (e.root != nullptr) {
        lv_obj_clear_flag(e.root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(e.root);
    }
}

} // namespace ui_scene_manager
