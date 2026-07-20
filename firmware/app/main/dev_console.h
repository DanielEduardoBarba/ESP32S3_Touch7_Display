#pragma once

/**
 * Development-only serial command console (compiled out in --prod builds).
 *
 * Reads newline-terminated commands from the USB-Serial-JTAG console (the
 * same port the logs go out on), so host tooling -- tools/dev_monitor.py's
 * hotkeys and tools/baud_test.py's automated baud ladder -- can drive the
 * device remotely:
 *
 *   sync            fw_update::syncPeer()
 *   push            fw_update::startSend()   (push my image to the peer)
 *   pull            fw_update::startPull()   (ask the peer for its image)
 *   baud <rate>     ports::setBaud(rate)     (announced to the peer first)
 *   baudlocal <rate> ports::setBaud(rate, announce=false)  (this end only)
 *   update-reset    fw_update::resetForTesting()  (clear transfer state)
 *   status          one-line summary: transport/baud/transfer state
 */
namespace dev_console {

/** Starts the reader task. No-op in production builds. */
void init();

} // namespace dev_console
