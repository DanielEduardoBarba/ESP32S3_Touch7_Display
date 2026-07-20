#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "ports_config.h"

/**
 * Peer-to-peer transport abstraction ("Ports").
 *
 * Wraps the four physical links two boards can be wired together with --
 * RS485 (default), CAN/TWAI, I2C, and the raw UART header -- behind one
 * byte-stream interface, so comm_protocol.cpp (framing) and everything
 * above it (machine sync, firmware update) never care which wire the bytes
 * ride on.
 *
 * Which transports exist at all is a compile-time choice (ports_config.h);
 * which enabled transport is ACTIVE is a runtime choice, made in the Ports
 * scene (ESP GUI or web GUI) and persisted via user_store so it survives
 * reboots.
 */
namespace ports {

enum class Transport : uint8_t {
    RS485 = 0,
    CAN   = 1,
    I2C   = 2,
    UART  = 3,
};

struct TransportInfo {
    Transport id;
    const char *name;    // short name for logs/APIs ("rs485", "can", ...)
    const char *label;   // human label for the GUIs ("RS485", "CAN bus", ...)
    bool enabled;        // compiled in via ports_config.h?
};

using RxCallback = std::function<void(const uint8_t *data, size_t len)>;
using ChangeCallback = std::function<void()>;

/** Initializes every ENABLED transport and activates the persisted (or
 *  default RS485) selection. Call once at startup, before comm_protocol. */
void init();

/** All four transports with their enabled/disabled status, for the GUIs. */
const std::vector<TransportInfo> &transports();

Transport active();
const char *name(Transport t);

/** Switches the active transport (must be an enabled one; returns false
 *  otherwise, or while a firmware transfer is running). Persists the
 *  choice for future boots. */
bool setActive(Transport t);

/** Supported baud rates for the serial links (RS485/UART), slowest first. */
const std::vector<uint32_t> &baudRates();

/** Currently active baud rate. */
uint32_t baud();

/**
 * Switches the serial baud rate. With `announce` (the default), a
 * CMD_BAUD_SET broadcast goes out at the CURRENT rate first so every peer
 * switches in sync; the RX path passes announce=false to follow a peer's
 * broadcast without re-broadcasting. Persisted across reboots. Returns
 * false for unknown rates or while a firmware transfer is running.
 */
bool setBaud(uint32_t baud, bool announce = true);

/** True while a firmware update transfer is in flight -- transport and
 *  baud changes are refused so the link can't be yanked mid-update. */
bool changeLocked();

/** GUI hook: fired after the active transport or baud rate changes from
 *  ANY source (local GUI, web API, or a peer's baud broadcast), so every
 *  view stays in sync. May fire from the RX task -- GUI layers must lock
 *  LVGL themselves. Can be called multiple times to add observers. */
void onChange(ChangeCallback cb);

/** Sends raw bytes over the ACTIVE transport. */
void send(const uint8_t *data, size_t len);

/** Registers the single receive sink (comm_protocol's parser). Bytes from
 *  whichever transport is active are delivered here. */
void onReceive(RxCallback cb);

} // namespace ports
