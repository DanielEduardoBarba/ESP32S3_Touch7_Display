#pragma once

#include "lvgl.h"

/** "Ports" scene: pick which peer-to-peer transport (RS485/CAN/I2C/UART)
 *  carries the machine-sync and firmware-update traffic. Only transports
 *  compiled in via ports_config.h are selectable. */
namespace ui_ports {

lv_obj_t *build(lv_obj_t *screen, lv_coord_t header_height);

} // namespace ui_ports
