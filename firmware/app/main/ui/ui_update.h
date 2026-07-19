#pragma once

#include "lvgl.h"

/** "Update" scene: shows which OTA slot is running, app version/size, and
 *  drives the device-to-device firmware update (send to peer / reboot into
 *  a received update). See fw_update.h for the transfer protocol. */
namespace ui_update {

lv_obj_t *build(lv_obj_t *screen, lv_coord_t header_height);

} // namespace ui_update
