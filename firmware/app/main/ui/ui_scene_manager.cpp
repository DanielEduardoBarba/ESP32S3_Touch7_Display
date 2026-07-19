#include "ui_scene_manager.h"

namespace ui_scene_manager {
namespace {

lv_obj_t *s_roots[static_cast<size_t>(Scene::_Count)] = {};

} // namespace

void registerScene(Scene scene, lv_obj_t *root)
{
    s_roots[static_cast<size_t>(scene)] = root;
}

void show(Scene scene)
{
    for (size_t i = 0; i < static_cast<size_t>(Scene::_Count); i++) {
        lv_obj_t *root = s_roots[i];
        if (root == nullptr) {
            continue;
        }
        if (i == static_cast<size_t>(scene)) {
            lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root);
        } else {
            lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

} // namespace ui_scene_manager
