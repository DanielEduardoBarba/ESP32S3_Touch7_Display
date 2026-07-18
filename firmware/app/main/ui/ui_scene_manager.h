#pragma once

#include "lvgl.h"

/**
 * Switches between the two top-level scenes (Machine, Network) shown below
 * the header bar. Each scene is built once as a full-size child of the
 * screen; showing one just brings it to the foreground and hides the
 * other, rather than destroying/recreating widgets every time.
 */
namespace ui_scene_manager {

enum class Scene {
    Machine,
    Network,
};

/** Registers the two scenes' root containers (as returned by
 *  ui_machine::build() / ui_network::build()). Call once during UI setup. */
void registerScenes(lv_obj_t *machine_root, lv_obj_t *network_root);

/** Shows the given scene and hides the other one. */
void show(Scene scene);

} // namespace ui_scene_manager
