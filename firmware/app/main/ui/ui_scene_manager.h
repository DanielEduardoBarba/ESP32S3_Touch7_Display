#pragma once

#include "lvgl.h"

/**
 * Switches between the top-level scenes (Machine, Network, Ports, Update,
 * Debug) shown below the header bar -- and manages their MEMORY: only the
 * scene currently on screen keeps its LVGL widgets alive. Leaving a scene
 * destroys its whole widget tree (each module's destroy() nulls its
 * statics); entering builds it fresh from the underlying data (machine
 * state, NVS-backed settings, wifi state...), which lives in the data
 * modules, never in widgets.
 *
 * Exception: a scene registered as PERSISTENT (the Update scene) is built
 * once and only hidden, never destroyed -- its widgets/callbacks must stay
 * live so a device-to-device update/sync keeps updating its UI while the
 * user is on any other scene.
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

using Builder = lv_obj_t *(*)(lv_obj_t *screen, lv_coord_t header_height);
using Destructor = void (*)();

/** Stores where scenes get built. Call once before registering scenes. */
void init(lv_obj_t *screen, lv_coord_t header_height);

/**
 * Registers a scene by its build/destroy functions. destroy() must delete
 * the root object AND null the module's widget statics. persistent = build
 * eagerly at registration and never destroy (hide only).
 */
void registerScene(Scene scene, Builder build, Destructor destroy, bool persistent = false);

/** Shows the given scene (building it if needed), hides persistent others,
 *  and destroys every non-persistent scene that isn't on screen. */
void show(Scene scene);

} // namespace ui_scene_manager
