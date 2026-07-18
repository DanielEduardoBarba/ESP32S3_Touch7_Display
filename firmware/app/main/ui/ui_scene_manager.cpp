#include "ui_scene_manager.h"

namespace ui_scene_manager {
namespace {

lv_obj_t *s_machine_root = nullptr;
lv_obj_t *s_network_root = nullptr;

} // namespace

void registerScenes(lv_obj_t *machine_root, lv_obj_t *network_root)
{
    s_machine_root = machine_root;
    s_network_root = network_root;
}

void show(Scene scene)
{
    if (s_machine_root == nullptr || s_network_root == nullptr) {
        return;
    }

    bool show_machine = (scene == Scene::Machine);

    if (show_machine) {
        lv_obj_clear_flag(s_machine_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_network_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_machine_root);
    } else {
        lv_obj_clear_flag(s_network_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_machine_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_network_root);
    }
}

} // namespace ui_scene_manager
