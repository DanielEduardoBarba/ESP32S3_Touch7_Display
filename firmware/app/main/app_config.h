#pragma once

/**
 * App-wide constants that don't belong to any single subsystem.
 *
 * This is the highest-level config header for the app firmware (alongside
 * ports_config.h for transport enables) -- add future global, cross-cutting
 * settings here rather than scattering them through individual modules.
 */

// Bump this whenever the app's user-facing behavior changes. Shown in the
// header (next to the Home button) and available to the web GUI/logs.
#define APP_VERSION "0.0.3"
