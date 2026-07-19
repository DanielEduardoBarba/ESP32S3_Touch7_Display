#pragma once

/**
 * Boot health / OTA rollback support for the main application.
 *
 * Implements the app side of the standard ESP-IDF high-reliability boot
 * flow (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, see
 * firmware/common/sdkconfig.defaults):
 *
 *   - checkRecoveryButtonAtBoot(): lets the user force the device into the
 *     factory/recovery app by holding the BOOT button while the app starts.
 *
 *   - commitRunningImageIfPending(): the "self-test passed" signal. A
 *     freshly OTA-installed image boots in pending-verify state; calling
 *     this marks it valid and cancels rollback. If the image crashes or
 *     resets before this is called, the stock bootloader automatically
 *     rolls back to the previous working image.
 */
namespace boot_health {

/**
 * Call FIRST in app_main(), before the display takes over the pins.
 * If the BOOT button (GPIO0) is held, points the OTA boot selector at the
 * factory/recovery partition and restarts.
 *
 * Usage gesture: press RESET, release it, then immediately hold BOOT while
 * the app starts. (Holding BOOT *through* a reset instead triggers the
 * ROM serial download mode -- that's a silicon feature, unrelated to this.)
 */
void checkRecoveryButtonAtBoot();

/**
 * Call once ALL critical subsystems have started successfully (display,
 * storage, WiFi manager, RS485, web server). Reaching that point without a
 * crash is the self-test; extend with deeper checks here if the product
 * ever needs them (e.g. sensor sanity, heap headroom).
 *
 * No-op when the running image isn't pending verification (normal reboots
 * of an already-validated image, or dev images flashed over serial).
 */
void commitRunningImageIfPending();

} // namespace boot_health
