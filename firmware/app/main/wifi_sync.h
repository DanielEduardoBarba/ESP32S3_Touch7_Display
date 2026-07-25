#pragma once

/**
 * Mirrors Wi-Fi credentials between the two boards over the peer link.
 *
 * When THIS board successfully joins a network, the SSID + password are
 * handed to the peer, which stores them and joins the same network. The
 * result: you type a password on one screen (or in one web UI) and both
 * boards end up online, which is what you want for two halves of the same
 * machine.
 *
 * Loop avoidance works the same way as machine_state's: a connection that
 * was itself triggered by a peer frame never re-broadcasts, and identical
 * credentials are ignored rather than re-applied.
 *
 * SECURITY NOTE: the credentials cross the peer link (RS485 by default) in
 * clear text, exactly like the rest of the protocol. That link is a short
 * private wire between two boards of the same machine; if it ever leaves
 * the enclosure, treat it as a cable carrying secrets.
 */
namespace wifi_sync {

/** Subscribes to wifi_manager state changes + comm_protocol frames. Call
 *  once at startup, after both of those are initialized. */
void init();

/** Sends the currently saved credentials to the peer right now (used by the
 *  "share with peer" button in the Network scene). No-op when nothing is
 *  saved yet. */
void shareNow();

} // namespace wifi_sync
