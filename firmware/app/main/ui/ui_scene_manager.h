#pragma once

#include "lvgl.h"

/**
 * Switches between the top-level scenes (Machine, Network, Ports, Update,
 * Debug) shown below the header bar. Each scene is built once as a full-size
 * child of the screen; showing one just brings it to the foreground and
 * hides the others, rather than destroying/recreating widgets every time.
 */
namespace ui_scene_manager {

enum class Scene : uint8_t {
    Machine = 0,
    Network,
    Ports,
    Update,
    Debug,
    _Count,
};

/** Registers a scene's root container (as returned by its build()). Call
 *  once per scene during UI setup. */
void registerScene(Scene scene, lv_obj_t *root);

/** Shows the given scene and hides all others. */
void show(Scene scene);

} // namespace ui_scene_manager
