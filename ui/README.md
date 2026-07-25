# `ui/` — LVGL Pro Editor project

Visual, live-preview UI design for this project's touchscreen, using the
official [LVGL Pro Editor](https://lvgl.io/pro) (the successor of what used to
be called `lvgl_editor`).

```bash
./build.sh --editor      # installs the editor if needed, then opens this folder
```

First launch asks for a license: pick **Community** (free for personal and
open-source use) or **Evaluation**. `./build.sh --setup` installs the editor
automatically; set `TOUCH_ESP32_SKIP_EDITOR=1` to skip that ~165MB download on
CI machines or containers.

## Layout

| Path | What it is |
| --- | --- |
| `project.xml` | Target display: 1024x600, matching the Waveshare panel, so the preview is pixel-accurate |
| `globals.xml` | Palette constants, shared styles, and one **subject** per synced control |
| `components/` | Reusable pieces (`card.xml` mirrors `makeCard()` in the firmware) |
| `screens/` | Full screens (`machine.xml` is a design-time copy of the Machine scene) |

The subjects in `globals.xml` deliberately use the same names as the fields of
`machine_state::State` (`dial_value`, `speed`, `setpoint`, `mode`,
`toggle_*`, `pulse_count`). Anything bound to them with `bind_value` /
`bind_text` / `bind_checked` is drivable from C with `lv_subject_set_int()`,
which is how a design made here gets wired to the real RS485-synced state.

## Important: the editor targets LVGL 9, the firmware runs LVGL 8.4

This is the one thing to know before designing screens for the device:

* The firmware pins `lvgl/lvgl: "^8.4"` (see `firmware/*/idf_component.yml`),
  and every hand-written scene under `firmware/app/main/ui/` uses the LVGL 8
  API.
* The editor's exported C code — and its runtime XML loader — use the LVGL 9
  API (`lv_screen_load()`, `lv_subject_*`, ...). There is no LVGL 8 export
  target.

So today this project is a **design surface**: it gives a live, pixel-accurate
preview of screens for the real panel resolution, a version-controllable
description of the UI, and a Figma import path — but its output cannot be
compiled into the current v8 firmware as-is.

Getting these designs onto the board needs one of:

1. **A second, LVGL 9 firmware target** (e.g. `firmware/uilab`) flashed to a
   spare OTA slot, which loads this XML at runtime. The working v8 app stays
   untouched; you gain a "see it on the real panel" loop.
2. **Migrating the app to LVGL 9**, after which exported C drops straight into
   `firmware/app/main/ui/`. Cleanest long-term, largest change.
3. **Design-only** (today): use the preview to iterate on layout, then
   hand-write the equivalent v8 code, as `ui_machine.cpp` does now.

## Headless code generation (`lved-cli.js`)

The LVGL Pro CLI can generate C, validate XML, run UI tests and take
screenshots from the terminal — ideal for wiring codegen into `build.sh`. Note
it is a **paid Pro feature** (needs `LVGL_CLI_TOKEN`); the Community and
Evaluation licenses cover the editor GUI only, where export is a button.
