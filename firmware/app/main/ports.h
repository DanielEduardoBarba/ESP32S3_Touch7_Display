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

/** Initializes every ENABLED transport and activates the persisted (or
 *  default RS485) selection. Call once at startup, before comm_protocol. */
void init();

/** All four transports with their enabled/disabled status, for the GUIs. */
const std::vector<TransportInfo> &transports();

Transport active();
const char *name(Transport t);

/** Switches the active transport (must be an enabled one; returns false
 *  otherwise). Persists the choice for future boots. */
bool setActive(Transport t);

/** Sends raw bytes over the ACTIVE transport. */
void send(const uint8_t *data, size_t len);

/** Registers the single receive sink (comm_protocol's parser). Bytes from
 *  whichever transport is active are delivered here. */
void onReceive(RxCallback cb);

} // namespace ports
