#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

/**
 * Versioned, forward-compatible user-data storage on top of NVS.
 *
 * Every record is stored as an NVS blob under its own string key, prefixed
 * with a tiny header: { magic, schema_version }. Readers always get the
 * version the record was WRITTEN with, so:
 *
 *   - a newer app reading an older record sees the old version number and
 *     can migrate the payload (instead of misreading it),
 *   - an older app reading a newer record sees an unknown version and can
 *     fall back to defaults (instead of crashing),
 *   - records this app doesn't know about are simply left untouched in
 *     NVS, so different/future apps never destroy each other's data.
 *
 * Boot decisions never live here (they live in otadata) -- this is strictly
 * user/application state: selected comm transport, UI preferences, etc.
 */
namespace user_store {

/** Mounts/initializes NVS (safe to call more than once). */
void init();

/**
 * Writes `data` under `key`, stamped with `version` (the caller's schema
 * version for that record type). Returns false on NVS failure.
 */
bool put(const char *key, uint8_t version, const void *data, size_t len);

/**
 * Reads the record at `key` into `out` (up to `max_len` bytes).
 * On success: returns true, sets `version_out` to the version the record
 * was written with and `len_out` to the payload size.
 * Returns false if the key doesn't exist, the record is corrupt, or the
 * buffer is too small -- callers should then use their defaults.
 */
bool get(const char *key, uint8_t *version_out, void *out, size_t max_len, size_t *len_out);

/** Convenience wrappers for the very common "one small integer" records. */
bool putU8(const char *key, uint8_t version, uint8_t value);
bool getU8(const char *key, uint8_t *version_out, uint8_t *value_out);

} // namespace user_store
