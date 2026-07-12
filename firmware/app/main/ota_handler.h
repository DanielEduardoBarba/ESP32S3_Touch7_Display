#pragma once

#include "esp_http_server.h"

/**
 * Handles POST /api/ota: streams the request body directly into whichever
 * OTA app slot (ota_0/ota_1) isn't currently running, via esp_ota_ops, then
 * reboots into it. This is the mechanism that lets a field unit's "brains"
 * firmware be updated from the web UI, independent of the ESP-IDF toolchain
 * -- the `factory` splash stage is never touched.
 */
namespace ota_handler {

esp_err_t handlePost(httpd_req_t *req);

} // namespace ota_handler
